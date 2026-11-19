"""Execute linked passive baud capture against an explicit MMIO/DMA model.

Only our public-source functions execute. Power APIs and driverlib ROM calls
are intercepted. DMA is an untimed descriptor model: this cannot establish
real bus latency, pin timing, complete MCU boot or Zigbee2MQTT startup.
"""
import argparse
import hashlib
import json
from pathlib import Path
from test_arm_radio import Image, ROOT, INPUT
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_PC, UC_ARM_REG_LR

DMA = 0x40020000
IOC = 0x40081000
GPIO = 0x40022000
EVENT = 0x40083000
AON = 0x40093008
RX = 1 << 12
CHANNEL = 1 << 14

class CaptureImage(Image):
    def __init__(self, path):
        super().__init__(path)
        self.vm.mem_map(0x40000000, 0x100000)
        self.allowed = []
        for name in ["PortBaudCapture_start", "PortBaudCapture_stop", "PortBaudCapture_read", "PortBaudCapture_clock", "port_autobaud_estimate",
                     "port_broadcast_send", "port_broadcast_receive", "OsalPort_registerTask", "OsalPort_msgEnqueue", "OsalPort_msgDequeue",
                     "OsalPort_msgSend", "OsalPort_msgReceive", "OsalPort_msgPush", "OsalPort_msgExtract", "OsalPort_msgFindDequeue",
                     "OsalPort_msgEnqueueMax", "port_task_queue", "port_task_tail_encode", "port_task_tail_decode"]:
            if name not in self.symbols: continue  # Optimizer may inline helpers.
            s = self.symbols[name]
            self.allowed.append((s["st_value"] & ~1, (s["st_value"] & ~1) + s["st_size"]))
        self.dependencies, self.constraints = {}, {}
        self.enabled = 6  # UART0's existing DMA RX/TX channels.
        self.attributes = 0
        self.tasks = None
        self.stubs = {}
        self.critical = 0
        self.events = 0
        self.task_events = False
        self.instructions = 0
        for name in ["OsalPort_enterCS", "OsalPort_leaveCS", "OsalPort_setEvent", "OsalPort_clearEvent", "Power_getDependencyCount", "Power_setDependency", "Power_releaseDependency", "Power_setConstraint", "Power_releaseConstraint", "ClockP_getCpuFreq"]:
            self.stubs[self.address(name) & ~1] = name
        for group, index, name in [(18, 0, "TimerConfigure"), (21, 0, "DmaAttributesEnable"), (21, 1, "DmaAttributesDisable"), (21, 5, "DmaScatterGather")]:
            table = 0x10001000 + group * 0x100
            self.put(0x10000180 + 4 * group, table)
            address = 0x1003e000 + len(self.stubs) * 4
            self.put(table + 4 * index, address | 1)
            self.stubs[address] = name
        self.vm.hook_add(UC_HOOK_MEM_READ, self.read_mmio)
        self.vm.hook_add(UC_HOOK_MEM_WRITE, self.write_mmio)
        self.put(IOC + 48, 0x2000600f)  # UART0 RX, input enabled, no edge owner.
        self.put(GPIO + 0xc0, RX)
        self.put(AON, 0x002b2b2b)
        self.put(DMA + 8, 0x20001800)

    def get(self, address): return int.from_bytes(self.vm.mem_read(address, 4), "little")
    def put(self, address, value): self.vm.mem_write(address, (value & 0xffffffff).to_bytes(4, "little"))

    def read_mmio(self, vm, access, address, size, value, context):
        if address == DMA + 0x28: self.put(address, self.enabled)

    def write_mmio(self, vm, access, address, size, value, context):
        if address == DMA + 0x28: self.enabled |= value
        if address == DMA + 0x2c: self.enabled &= ~value

    def hook(self, vm, address, size, context):
        self.instructions += 1
        if address not in self.stubs:
            if not any(a <= address < b for a, b in self.allowed):
                raise AssertionError(f"Capture executed outside own public-source functions: {address:x}")
            return
        name = self.stubs[address]
        a, b, c, d = [vm.reg_read(reg) for reg in [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]]
        result = 0
        if name == "OsalPort_enterCS": result = self.critical; self.critical += 1
        elif name == "OsalPort_leaveCS": self.critical = a
        elif name == "OsalPort_setEvent":
            if self.task_events: assert a < 15 and b == 0x8000 and self.critical
            else: assert (a, b) == (3, 0x2000) and not self.critical
            self.events += 1
        elif name == "OsalPort_clearEvent": assert self.task_events and a < 15 and b == 0x8000 and self.critical
        elif name == "Power_getDependencyCount": result = self.dependencies.get(a, 0)
        elif name in {"Power_setDependency", "Power_releaseDependency", "Power_setConstraint", "Power_releaseConstraint"}:
            counts = self.constraints if "Constraint" in name else self.dependencies
            counts[a] = counts.get(a, 0) + (1 if "set" in name else -1)
            assert counts[a] >= 0
        elif name == "ClockP_getCpuFreq": self.put(a, 0); self.put(a + 4, 48000000)
        elif name == "TimerConfigure": assert (a, b) == (0x40010000, 0x32)
        elif name in {"DmaAttributesEnable", "DmaAttributesDisable"}:
            assert a == DMA and b == 14 and c & ~15 == 0
            self.attributes = self.attributes | c if "Enable" in name else self.attributes & ~c
        elif name == "DmaScatterGather":
            assert (a, b, c) == (DMA, 14, 8)
            assert self.get(vm.reg_read(UC_ARM_REG_SP)) == 1
            assert d < 0x50000 and self.attributes == 5
            self.tasks = [tuple(self.get(d + i * 16 + j * 4) for j in range(4)) for i in range(8)]
            destinations = []
            for i, (src, dst, control, spare) in enumerate(self.tasks):
                assert spare == 0 and control >> 4 & 1023 == 0
                assert control & 7 == (1 if i == 7 else 7)
                assert control >> 24 == 0xee  # 32-bit words; no address increments.
                if i % 2 == 0:
                    assert src == 0x40010048 and 0x20001800 <= dst < 0x20013000
                    destinations.append(dst)
                else: assert dst == GPIO + 0xe0 and self.get(src) == RX
            assert destinations == [destinations[0] + 4 * i for i in range(4)]
        else: raise AssertionError(name)
        vm.reg_write(UC_ARM_REG_R0, result)
        vm.reg_write(UC_ARM_REG_PC, vm.reg_read(UC_ARM_REG_LR))

    def edges(self, values):
        assert self.tasks and self.enabled & CHANNEL
        assert self.get(IOC + 48) & 0x00ff003f == 0x0083000f
        assert self.get(EVENT + 0x574) == 3 and self.get(AON) >> 16 & 63 == 0
        for index, value in enumerate(values):
            self.put(self.tasks[index * 2][1], value)
            # Model the paired W1C task. Other GPIO owners are never cleared.
            assert self.get(self.tasks[index * 2 + 1][0]) == RX
        if len(values) == 4: self.enabled &= ~CHANNEL

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", type=Path)
    ap.add_argument("--report", type=Path)
    args = ap.parse_args()
    image = CaptureImage(args.image)
    assert image.run("PortBaudCapture_clock", 0) == 48000000
    cases = 0
    for rate in [115200, 460800, 1000000]:
        for jitter in range(9):
            # The actual MCU reset route is AON_PROG0, not NONE. Test both
            # allowed idle states and require exact restoration of each.
            before_dma = jitter % 2
            image.put(EVENT + 0x574, before_dma)
            before_ioc, before_aon = image.get(IOC + 48), image.get(AON)
            assert image.run("PortBaudCapture_start", 0) == 1
            assert image.run("PortBaudCapture_read", INPUT) == 0
            values = [0xfffffff0, (0xfffffff0 + round(96000000 / rate) + jitter) & 0xffffffff,
                      (0xfffffff0 + round(480000000 / rate) + jitter // 2) & 0xffffffff,
                      (0xfffffff0 + round(912000000 / rate) + jitter // 2) & 0xffffffff]
            image.edges(values)
            assert image.run("PortBaudCapture_read", INPUT) == 1
            assert [image.get(INPUT + i * 4) for i in range(4)] == values
            assert image.run("port_autobaud_estimate", INPUT, 48000000) == rate
            image.run("PortBaudCapture_stop", 0)
            assert image.get(IOC + 48) == before_ioc and image.get(AON) == before_aon
            assert image.enabled == 6 and image.get(EVENT + 0x574) == before_dma
            assert not any(image.dependencies.values()) and not any(image.constraints.values()) and not image.critical
            cases += 1
    # Contended resources and a non-idle RX must be left untouched.
    for address, value in [(GPIO + 0xc0, 0), (IOC + 48, 0x2003600f), (IOC, 1 << 23),
                           (EVENT + 0x574, 7), (EVENT + 0x78, 3), (AON, 0x002a2b2b)]:
        previous = image.get(address); image.put(address, value)
        assert image.run("PortBaudCapture_start", 0) == 0
        assert image.get(address) == value and image.enabled == 6
        image.put(address, previous); cases += 1
    # Aborting a partial acquisition (ordinary UART frame already valid) is safe.
    assert image.run("PortBaudCapture_start", 0) == 1
    image.edges([100, 200]); image.run("PortBaudCapture_stop", 0)
    assert image.enabled == 6 and image.get(IOC + 48) == 0x2000600f
    image.vm.mem_write(image.address("NWK_TaskID"), bytes([3]))
    assert image.run("port_broadcast_receive", 0) == 0
    assert image.run("port_broadcast_send", 0) != 0
    broadcast_cases = 2
    for turn in range(100):
        for i in range(10):
            assert image.run("port_broadcast_send", INPUT + i * 4) == 0
            broadcast_cases += 1
        assert image.run("port_broadcast_send", INPUT + 100) != 0
        for i in range(10):
            assert image.run("port_broadcast_receive", 0) == INPUT + i * 4
            broadcast_cases += 1
        assert image.run("port_broadcast_receive", 0) == 0
        broadcast_cases += 2
    assert not image.critical and image.events == 1900
    image.task_events = True
    for task in range(15): assert image.run("OsalPort_registerTask", 0, 0, INPUT + task * 4) == task
    assert image.run("OsalPort_registerTask", 0, 0, INPUT) == 255
    message_base = 0x20010008
    for index in range(257): image.vm.mem_write(message_base + index * 16 - 8, bytes(16))
    append_instructions = []
    for index in range(257):
        image.instructions = 0
        assert image.run("OsalPort_msgSend", 0, message_base + index * 16) == 0
        if index: append_instructions.append(image.instructions)
    assert max(append_instructions) - min(append_instructions) <= 4
    for index in range(257): assert image.run("OsalPort_msgReceive", 0) == message_base + index * 16
    assert image.run("OsalPort_msgReceive", 0) == 0 and not image.critical
    report = {"passed": True, "capture_cases": cases + 1, "broadcast_cases": broadcast_cases, "hardware_tested": False,
        "task_queue_messages": 257, "task_append_instruction_range": [min(append_instructions), max(append_instructions)],
        "backend": "Unicorn 2.1.3 actual linked ARM capture/estimator; untimed MMIO and DMA descriptor model",
        "rom_calls": "intercepted; no TI ROM/closed NWK instructions executed",
        "z2m_startup_tested": False, "real_DMA_latency_tested": False,
        "elf_sha256": hashlib.sha256(args.image.read_bytes()).hexdigest(),
        "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "requirements_sha256": hashlib.sha256((ROOT / "tools/arm-tests-requirements.txt").read_bytes()).hexdigest()}
    if args.report: args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))

if __name__ == "__main__": main()
