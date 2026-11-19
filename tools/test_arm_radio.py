"""Execute the linked ARM radio guard and public callback prefix in Unicorn.

No boot, radio, MCU peripheral, TI ROM or closed NWK/security emulation.
ROM calls are intercepted. Valid frames stop at the first native MAC call;
rejected DATA_IND frames must release the original message exactly once.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".tooling/arm-tests"))
try:
    import unicorn
    import elftools
    from elftools.elf.elffile import ELFFile
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC
    from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_XPSR
except ImportError as exc:
    raise SystemExit("Install: python -m pip install --require-hashes --only-binary=:all: --target .tooling/arm-tests -r tools/arm-tests-requirements.txt") from exc

if unicorn.__version__ != "2.1.3" or elftools.__version__ != "0.32":
    raise SystemExit("ARM test dependency version mismatch")

INPUT = 0x21000000
MESSAGE = 0x20010000
END = 0x5f000

def unwrap(die):
    while die.tag in {"DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"}:
        die = die.get_DIE_from_attribute("DW_AT_type")
    return die

def field(die, path):
    offset = 0
    for name in path:
        die = unwrap(die)
        members = [c for c in die.iter_children() if c.tag == "DW_TAG_member" and
                   c.attributes.get("DW_AT_name") and c.attributes["DW_AT_name"].value.decode() == name]
        if len(members) != 1: raise ValueError("Missing/ambiguous DWARF field: " + name)
        member = members[0]
        location = member.attributes["DW_AT_data_member_location"].value
        if not isinstance(location, int): raise ValueError("Unsupported DWARF member location")
        offset += location
        die = member.get_DIE_from_attribute("DW_AT_type")
    die = unwrap(die)
    size = die.attributes.get("DW_AT_byte_size")
    if size: return offset, size.value
    if die.tag == "DW_TAG_pointer_type": return offset, die.cu["address_size"]
    raise ValueError("DWARF field size is unavailable")

class Image:
    def __init__(self, path):
        self.vm = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.vm.mem_map(0, 0x60000, UC_PROT_READ | UC_PROT_EXEC)
        self.vm.mem_map(0x20000000, 0x14000, UC_PROT_READ | UC_PROT_WRITE)
        self.vm.mem_map(INPUT, 0x1000, UC_PROT_READ | UC_PROT_WRITE)
        self.vm.mem_map(0x10000000, 0x40000, UC_PROT_READ | UC_PROT_EXEC)
        with path.open("rb") as stream:
            elf = ELFFile(stream)
            symbols = elf.get_section_by_name(".symtab")
            self.symbols = {s.name: s for s in symbols.iter_symbols() if s.name}
            for section in elf.iter_sections():
                if section["sh_flags"] & 2 and section["sh_type"] != "SHT_NOBITS" and section["sh_size"]:
                    address = section["sh_addr"]
                    if address < 0x60000 or 0x20000000 <= address < 0x20014000:
                        self.vm.mem_write(address, section.data())
            callback_type = nib_type = None
            for cu in elf.get_dwarf_info().iter_CUs():
                for die in cu.iter_DIEs():
                    name = die.attributes.get("DW_AT_name")
                    if not name: continue
                    if die.tag == "DW_TAG_typedef" and name.value == b"macCbackEvent_t": callback_type = die
                    if die.tag == "DW_TAG_variable" and name.value == b"_NIB" and "DW_AT_type" in die.attributes:
                        nib_type = die.get_DIE_from_attribute("DW_AT_type")
                if callback_type and nib_type: break
            if not callback_type or not nib_type: raise ValueError("Required TI DWARF types missing")
            self.event_field = field(callback_type, ["hdr", "event"])
            self.pointer_field = field(callback_type, ["dataInd", "msdu", "p"])
            self.length_field = field(callback_type, ["dataInd", "msdu", "len"])
            self.level_field = field(nib_type, ["SecurityLevel"])
        # Public rom_jt_def_154.h / hmac_rom_jt.h: RAM slot 2 selects HMAC,
        # API base 72, deallocate +141 and MlmeGetReq +194. Only these two
        # entries are modeled; the pointed-to ROM instructions are never run.
        self.vm.mem_write(self.address("RAM_MAC_BASE_ADDR") + 8, (0x10000000).to_bytes(4, "little"))
        self.vm.mem_write(0x10000000 + 4 * (72 + 141), (0x1003f001).to_bytes(4, "little"))
        self.vm.mem_write(0x10000000 + 4 * (72 + 194), (0x1003f005).to_bytes(4, "little"))
        self.allowed = []
        for name in ["port_radio_guard_accept", "MAC_CbackEvent"]:
            symbol = self.symbols[name]
            start = symbol["st_value"] & ~1
            self.allowed.append((start, start + symbol["st_size"]))
        self.vm.hook_add(UC_HOOK_CODE, self.hook)
        self.mode = None

    def address(self, name): return self.symbols[name]["st_value"]

    def hook(self, vm, address, size, _):
        if 0x10000000 <= address < 0x10040000:
            if self.mode != "callback": raise AssertionError("Unexpected ROM call")
            self.native_calls += 1
            if not self.reject_expected:
                if address != 0x1003f004: raise AssertionError("Expected native MlmeGetReq")
                self.native_reached = True
                vm.emu_stop()
                return
            # The public callback's rejected-DATA_IND path calls deallocate
            # with a pointer to the original message pointer on its stack.
            pointer = vm.reg_read(UC_ARM_REG_R0)
            if address != 0x1003f000: raise AssertionError("Expected native deallocate")
            original = int.from_bytes(vm.mem_read(pointer, 4), "little")
            if original != MESSAGE or self.native_calls != 1:
                raise AssertionError("Incorrect rejected-frame ownership")
            vm.mem_write(pointer, bytes(4))
            vm.reg_write(UC_ARM_REG_PC, vm.reg_read(UC_ARM_REG_LR))
            return
        if not any(start <= address < end for start, end in self.allowed):
            raise AssertionError(f"Execution outside the two public source functions: 0x{address:x}")

    def run(self, name, r0, r1=0, r2=0):
        self.native_calls = 0
        self.native_reached = False
        self.vm.reg_write(UC_ARM_REG_XPSR, 0x01000000)
        self.vm.reg_write(UC_ARM_REG_SP, 0x20013f00)
        self.vm.reg_write(UC_ARM_REG_LR, END | 1)
        for reg, value in [(UC_ARM_REG_R0, r0), (UC_ARM_REG_R1, r1), (UC_ARM_REG_R2, r2)]:
            self.vm.reg_write(reg, value)
        self.vm.emu_start(self.address(name), END, count=2000)
        if self.vm.reg_read(UC_ARM_REG_PC) != END and not self.native_reached:
            raise AssertionError("ARM function did not return within the instruction budget")
        return self.vm.reg_read(UC_ARM_REG_R0)

    def input(self, data):
        address = INPUT + 0x1000 - len(data)
        if data: self.vm.mem_write(address, bytes(data))
        return address

    def check(self, data, expected, level=5):
        self.mode = "guard"
        address = self.input(data)
        actual = self.run("port_radio_guard_accept", address, len(data), level)
        if bool(actual) != expected: raise AssertionError((bytes(data).hex(), len(data), level, actual, expected))

    def callback(self, data, valid, level=5):
        self.mode = "callback"
        self.reject_expected = not valid
        self.vm.mem_write(MESSAGE, bytes(128))
        address = self.input(data)
        for (offset, size), value in [(self.event_field, 13), (self.pointer_field, address), (self.length_field, len(data))]:
            self.vm.mem_write(MESSAGE + offset, value.to_bytes(size, "little"))
        offset, size = self.level_field
        self.vm.mem_write(self.address("_NIB") + offset, level.to_bytes(size, "little"))
        self.run("MAC_CbackEvent", MESSAGE)
        if self.native_calls != 1 or self.native_reached != valid:
            raise AssertionError("Unexpected native callback dispatch")

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", type=Path)
    ap.add_argument("--report", type=Path)
    args = ap.parse_args()
    image = Image(args.image)
    image.mode = "callback"
    image.reject_expected = False
    image.run("MAC_CbackEvent", 0)
    if image.native_calls: raise AssertionError("NULL callback reached native MAC")
    image.vm.mem_write(MESSAGE, bytes([255]) + bytes(127))
    image.run("MAC_CbackEvent", MESSAGE)
    if image.native_calls: raise AssertionError("Unknown event reached native MAC")
    checks = callbacks = 0
    for flags in range(32):
        for level in range(8):
            data = bytearray([8, flags] + [0] * 6)
            data.extend(bytes((8 if flags & 8 else 0) + (8 if flags & 16 else 0) + (1 if flags & 1 else 0)))
            if flags & 4: data.extend([3, 2, 0, 0, 0, 0, 0, 0])
            if flags & 2:
                data.extend([0x28] + [0] * 13)
                data.extend(bytes([0, 4, 8, 16, 0, 4, 8, 16][level]))
            data.append(0)
            image.check(data, True, level); checks += 1
            image.callback(data, True, level); callbacks += 1
            for cut in range(len(data)):
                image.check(data[:cut], False, level); checks += 1
                image.callback(data[:cut], False, level); callbacks += 1
    for data, expected in [(b'\x8c\x00', True), (b'\x8d\x00', True), (b'\x8e\x00', False),
                           (b'\x8f\x00', False), (b'\x0b\x00\x03' + bytes(4), True),
                           (b'\x0b\x00\x03' + bytes(3), False)]:
        image.check(data, expected); checks += 1
        image.callback(data, expected); callbacks += 1
    report = {"passed": True, "backend": "Unicorn 2.1.3 ARM M-class, actual TI-linked public source instructions",
              "hardware_tested": False, "z2m_startup_tested": False, "radio_tested": False,
              "guard_cases": checks, "callback_prefix_cases": callbacks,
              "rom_calls": "intercepted; no ROM or closed NWK/security code executed",
              "elf_sha256": hashlib.sha256(args.image.read_bytes()).hexdigest(),
              "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "requirements_sha256": hashlib.sha256((ROOT / "tools/arm-tests-requirements.txt").read_bytes()).hexdigest(),
              "unicorn_version": unicorn.__version__, "pyelftools_version": elftools.__version__,
              "mac_abi_dwarf": {"event": image.event_field, "msdu_pointer": image.pointer_field,
                                "msdu_length": image.length_field, "nib_security_level": image.level_field}}
    if args.report: args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))

if __name__ == "__main__": main()
