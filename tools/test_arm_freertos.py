"""Execute the Cortex-M4F instructions of the FreeRTOS target's portable
modules in Unicorn, against the same reference vectors as the host tests.

Scope: compiled ARM code of AES-128, CCM*, MMO/install codes, the existing
NWK security engine with the software CCM backend, MAC FCS/parser and the
radio RX element parser. No peripheral, RTOS or radio is emulated. Reported
instruction counts are emulator instruction counts, NOT MCU cycles.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".tooling/arm-tests"))
import unicorn  # noqa: E402
from elftools.elf.elffile import ELFFile  # noqa: E402
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_PROT_ALL  # noqa: E402
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,  # noqa: E402
                               UC_ARM_REG_SP, UC_ARM_REG_LR)

if unicorn.__version__ != "2.1.3":
    raise SystemExit("ARM test dependency version mismatch")

STOP = 0x0004FFF0
HEAP = 0x20040000
STACK_TOP = 0x2005FFF0


def parse_vectors():
    text = (ROOT / "firmware/freertos/tests/vectors_crypto.h").read_text()

    def arrays(name):
        block = re.search(rf"{name}\[\] = \{{(.*?)\n\}};", text, re.S).group(1)
        rows = []
        for line in block.strip().splitlines():
            groups = re.findall(r"\{([^{}]*)\}", line)
            nums = re.findall(r"\}, (\d+)", line)
            rows.append(([bytes(int(x, 16) for x in g.split(", ") if x.startswith("0x")) for g in groups], nums))
        return rows

    def scalar(name):
        m = re.search(rf"{name}\[\d+\] = \{{([^}}]*)\}}", text)
        return bytes(int(x, 16) for x in m.group(1).split(", "))

    header_len = int(re.search(r"NWK_SECURED_HEADER_LEN (\d+)u", text).group(1))
    return arrays, scalar, header_len


class Arm:
    def __init__(self, elf_path):
        self.vm = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.vm.mem_map(0, 0x50000, UC_PROT_ALL)
        self.vm.mem_map(0x20000000, 0x60000, UC_PROT_ALL)
        with open(elf_path, "rb") as f:
            elf = ELFFile(f)
            self.sym = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols() if s.name}
            for s in elf.iter_sections():
                if s["sh_flags"] & 2 and s["sh_type"] != "SHT_NOBITS" and s["sh_size"]:
                    self.vm.mem_write(s["sh_addr"], s.data())
            data = elf.get_section_by_name(".data")
            if data and data["sh_size"]:
                self.vm.mem_write(data["sh_addr"], data.data())
        self.count = 0
        self.vm.hook_add(UC_HOOK_CODE, self._count)
        self.heap = HEAP

    def _count(self, uc, address, size, user):
        self.count += 1

    def alloc(self, data_or_size):
        size = data_or_size if isinstance(data_or_size, int) else len(data_or_size)
        addr = self.heap
        self.heap = (self.heap + max(size, 1) + 7) & ~7
        if self.heap >= STACK_TOP - 0x2000:
            raise RuntimeError("test heap exhausted")
        if not isinstance(data_or_size, int):
            self.vm.mem_write(addr, bytes(data_or_size))
        else:
            self.vm.mem_write(addr, bytes(size))
        return addr

    def reset_heap(self):
        self.heap = HEAP

    def call(self, name, *args):
        regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
        sp = STACK_TOP - 4 * max(0, len(args) - 4)
        sp &= ~7
        for i, a in enumerate(args):
            a &= 0xFFFFFFFF
            if i < 4:
                self.vm.reg_write(regs[i], a)
            else:
                self.vm.mem_write(sp + 4 * (i - 4), a.to_bytes(4, "little"))
        self.vm.reg_write(UC_ARM_REG_SP, sp)
        self.vm.reg_write(UC_ARM_REG_LR, STOP | 1)
        # Paint the stack area to measure the deepest use of this call.
        paint_base = STACK_TOP - 0x2000
        self.vm.mem_write(paint_base, bytes([0xA5]) * (sp - paint_base))
        start = self.count
        self.vm.emu_start(self.sym[name] | 1, STOP, count=50_000_000)
        used = self.vm.mem_read(paint_base, sp - paint_base)
        low = next((i for i, b in enumerate(used) if b != 0xA5), len(used))
        self.last_stack = len(used) - low
        if low == 0:
            raise RuntimeError(f"{name}: stack deeper than the 8 KiB measuring area")
        return self.vm.reg_read(UC_ARM_REG_R0), self.count - start

    def read(self, addr, n):
        return bytes(self.vm.mem_read(addr, n))


def main(elf_path, report_path):
    arm = Arm(elf_path)
    arrays, scalar, header_len = parse_vectors()
    checks = 0
    metrics = {}

    ctx = arm.alloc(176)
    for (key, pt, ct), _ in arrays("AES_VECTORS"):
        k, i, o = arm.alloc(key), arm.alloc(pt), arm.alloc(16)
        arm.call("aes128_init", ctx, k)
        _, n = arm.call("aes128_encrypt", ctx, i, o)
        assert arm.read(o, 16) == ct
        checks += 1
        metrics["aes128_encrypt_block_instructions"] = n

    for groups, nums in arrays("CCM_VECTORS"):
        key, nonce, aad, pt, ct, mic = groups
        aad_len, pt_len, m = int(nums[0]), int(nums[1]), int(nums[2])
        aad, pt, ct, mic = aad[:aad_len], pt[:pt_len], ct[:pt_len], mic[:m]
        arm.reset_heap()
        c = arm.alloc(176)
        arm.call("aes128_init", c, arm.alloc(key))
        nn, a, i, o, mm = arm.alloc(nonce), arm.alloc(aad), arm.alloc(pt), arm.alloc(max(pt_len, 1)), arm.alloc(16)
        r, _ = arm.call("ccm_star_seal", c, nn, a, aad_len, i, pt_len, o, mm, m, 1)
        assert r == 1 and arm.read(o, pt_len) == ct and arm.read(mm, m) == mic
        bad = bytearray(mic)
        bad[0] ^= 1
        b = arm.alloc(bytes(bad))
        r, _ = arm.call("ccm_star_open", c, nn, a, aad_len, arm.alloc(ct), pt_len, o, b, m, 1)
        assert r == 0 and arm.read(o, pt_len) == bytes(pt_len)
        checks += 2

    for groups, nums in arrays("MMO_VECTORS"):
        msg, digest = groups
        n = int(nums[0])
        arm.reset_heap()
        d = arm.alloc(16)
        r, _ = arm.call("zb_mmo_hash", arm.alloc(msg[:n]), n, d)
        assert r == 1 and arm.read(d, 16) == digest
        checks += 1

    arm.reset_heap()
    code = bytes.fromhex("83FED3407A939723A5C639B26916D505C3B5")
    out = arm.alloc(16)
    r, n = arm.call("zb_install_code_to_key", arm.alloc(code), 18, out)
    assert r == 1 and arm.read(out, 16) == bytes.fromhex("66B6900981E1EE3CA4206B6B861C02BB")
    metrics["install_code_to_key_instructions"] = n
    checks += 1

    # Existing NWK security engine + software CCM backend on ARM.
    arm.reset_heap()
    frame = scalar("NWK_SECURED_FRAME")
    plaintext = scalar("NWK_PLAINTEXT")
    state = arm.alloc(0x2400)
    arm.call("core_security_init", state, 0, arm.sym["zb_ccm_backend_sw"] | 1, 0, 0)
    r, _ = arm.call("core_security_set_key", state, 0, 0, arm.alloc(scalar("NWK_KEY")))
    assert r == 1
    buf, plen = arm.alloc(frame), arm.alloc(4)
    r, n = arm.call("core_security_open_authenticated_peer", state, buf, len(frame), header_len, plen)
    assert r == 1 and int.from_bytes(arm.read(plen, 4), "little") == len(plaintext)
    assert arm.read(buf + header_len + 14, len(plaintext)) == plaintext
    metrics[f"nwk_open_{len(plaintext)}B_payload_instructions"] = n
    arm.vm.mem_write(buf, frame)
    r, _ = arm.call("core_security_open", state, buf, len(frame), header_len, plen)
    assert r == 0, "replay must be refused"
    checks += 3

    arm.reset_heap()
    r, _ = arm.call("mac_fcs", arm.alloc(b"123456789"), 9)
    assert r == 0x2189
    beacon = bytes.fromhex("00800962 1a0000 ffcf0000 0022 84" .replace(" ", "")) + bytes([0xDD] * 8) + b"\xff\xff\xff\x03"
    f = arm.alloc(160)
    r, n = arm.call("mac_frame_parse", arm.alloc(beacon), len(beacon), f)
    assert r == 0
    metrics["mac_frame_parse_beacon_instructions"] = n
    element = bytes([11, 0x02, 0x00, 0x77, 0xAA, 0xBB, 0xC4, 0x25, 0x78, 0x56, 0x34, 0x12])
    out = arm.alloc(160)
    r, _ = arm.call("rf_rx_parse_element", arm.alloc(element), len(element), out)
    assert r == 1 and arm.read(out, 3) == bytes([0x02, 0x00, 0x77])
    r, _ = arm.call("rf_rx_parse_element", arm.alloc(element), 11, out)
    assert r == 0
    checks += 4

    # Coordinator MAC with the test-only simulated radio (arm_sim.c).
    arm.reset_heap()
    mac = arm.alloc(0x3000)   # >= sizeof(mac_t) (10 264 B with 48 MAC buffers)
    arm.call("arm_sim_mac_setup", mac)
    def sim(name, n=4):
        return int.from_bytes(arm.read(arm.sym[name], n), "little")
    breq = bytes([0x03, 0x08, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07])
    _, n = arm.call("mac_rx", mac, arm.alloc(breq), len(breq), 255, 0xD8)
    assert sim("arm_sim_count") == 1 and sim("arm_sim_len") == 26
    beacon_tx = arm.read(arm.sym["arm_sim_last"], 26)
    assert beacon_tx[:2] == bytes([0x00, 0x80]) and beacon_tx[7:9] == bytes([0xFF, 0xCF]) and beacon_tx[12:14] == bytes([0x22, 0x84])
    metrics["mac_rx_beacon_request_to_tx_instructions"] = n
    arm.call("mac_tx_done", mac, 0, 0)
    dev = bytes.fromhex("010000000038c1a4")  # A4C1380000000001 little endian
    assoc = bytes([0x23, 0xC8, 0x07, 0x62, 0x1A, 0x00, 0x00, 0xFF, 0xFF]) + dev + bytes([0x01, 0x8E])
    _, n = arm.call("mac_rx", mac, arm.alloc(assoc), len(assoc), 180, 0xC4)
    assert sim("arm_sim_assoc_count") == 1 and sim("arm_sim_match_pending") == 1
    assert sim("arm_sim_match_addr", 8) == 0xA4C1380000000001
    metrics["mac_rx_assoc_request_instructions"] = n
    poll = bytes([0x63, 0xC8, 0x08, 0x62, 0x1A, 0x00, 0x00]) + dev + bytes([0x04])
    _, n = arm.call("mac_rx", mac, arm.alloc(poll), len(poll), 180, 0xC4)
    assert sim("arm_sim_count") == 2
    resp = arm.read(arm.sym["arm_sim_last"], sim("arm_sim_len"))
    assert resp[-4:] == bytes([0x02, 0x21, 0x4E, 0x00]) and resp[5:13] == dev
    metrics["mac_rx_data_request_to_tx_instructions"] = n
    arm.call("mac_tx_done", mac, 0, 0)
    assert sim("arm_sim_match_pending") == 0
    checks += 8

    # End-to-end paths of the real coordinator / ZNP code (arm_perf.c).
    arm.reset_heap()
    arm.call("arm_perf_setup")
    _, n = arm.call("arm_perf_ccm_sw")
    metrics["ccm_seal_22B_aad_40B_payload_instructions"] = n
    stacks = {"ccm_seal_sw": arm.last_stack}
    for i in range(3):
        assert arm.call("arm_perf_build_rx")[0] > 0
        up, n = arm.call("arm_perf_rx")
        assert up == i + 1
    metrics["coord_rx_secured_zcl_report_instructions"] = n
    stacks["coord_rx_secured_frame"] = arm.last_stack
    assert arm.call("arm_perf_build_rx")[0] > 0
    before = arm.call("arm_perf_znp_out_count")[0] if "arm_perf_znp_out_count" in arm.sym else None
    out, n = arm.call("arm_perf_rx_znp")
    metrics["coord_rx_to_znp_af_incoming_instructions"] = n
    stacks["coord_rx_to_znp_af_incoming"] = arm.last_stack
    status, n = arm.call("arm_perf_send")
    assert status == 0
    metrics["coord_send_zcl_command_instructions"] = n
    stacks["coord_send_aps"] = arm.last_stack
    arm.call("arm_perf_tx_done")
    out_before = arm.call("arm_perf_znp_af")[0]
    arm.call("arm_perf_tx_done")
    out_after, n = arm.call("arm_perf_znp_af")
    assert out_after >= out_before + 1
    metrics["znp_af_data_request_to_mac_instructions"] = n
    stacks["znp_af_data_request"] = arm.last_stack
    arm.call("arm_perf_tx_done")
    ok, _ = arm.call("arm_perf_ccm_hw_inplace")   # block hook = software AES: stack only
    assert ok == 1
    stacks["ccm_hw_backend_inplace"] = arm.last_stack
    assert arm.call("arm_perf_tick_link_status")[0] >= 1
    stacks["coord_tick_link_status"] = arm.last_stack
    assert arm.call("arm_perf_zdo_mgmt_lqi")[0] > 3
    stacks["zdo_mgmt_lqi"] = arm.last_stack
    checks += 10

    report = {
        "level": "ARM Cortex-M4 instructions executed in Unicorn 2.1.3 (no peripherals, no RTOS)",
        "elf": str(Path(elf_path).relative_to(ROOT)).replace("\\", "/"),
        "elf_sha256": hashlib.sha256(Path(elf_path).read_bytes()).hexdigest(),
        "checks": checks,
        "instruction_counts_not_cycles": metrics,
        "stack_bytes_from_call": stacks,
        "hardware_tested": False,
    }
    Path(report_path).write_text(json.dumps(report, indent=2) + "\n")
    print(f"arm: {checks} checks passed; " + ", ".join(f"{k}={v}" for k, v in metrics.items()))
    print("arm stack (bytes below the caller): " + ", ".join(f"{k}={v}" for k, v in stacks.items()))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
