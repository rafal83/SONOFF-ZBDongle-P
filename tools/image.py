"""Strict ELF32/Intel HEX reader, exporter and CC26x2 (352 KiB only) static recovery checks."""
import argparse
import json
from pathlib import Path
import struct

FLASH_END = 0x58000
CCFG = 0x57FA8

def read_elf(path, sections_only=False):
    raw = Path(path).read_bytes()
    if raw[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Expected ELF32 little endian")
    if struct.unpack_from("<H", raw, 18)[0] != 40:
        raise ValueError("Expected ARM ELF")
    offset = struct.unpack_from("<I", raw, 28)[0]
    entry_size, count = struct.unpack_from("<HH", raw, 42)
    memory = {}
    for i in range(count):
        kind, start, _, address, size, _, _, _ = struct.unpack_from("<8I", raw, offset + i * entry_size)
        if kind != 1 or not size:
            continue
        if address >= FLASH_END:
            continue  # SRAM-only load segments have no flash contents
        if address + size > FLASH_END or start + size > len(raw):
            raise ValueError("ELF load segment outside Flash/file")
        for j, byte in enumerate(raw[start:start + size]):
            if address + j in memory and memory[address + j] != byte:
                raise ValueError("Conflicting ELF load segments")
            memory[address + j] = byte
    if sections_only:
        # TI objcopy exports allocated sections, while ELF PT_LOAD segments
        # may additionally contain alignment padding between those sections.
        sh_offset = struct.unpack_from("<I", raw, 32)[0]
        sh_size, sh_count = struct.unpack_from("<HH", raw, 46)
        selected = {}
        if sh_size < 40 or not sh_count or sh_offset + sh_size * sh_count > len(raw):
            raise ValueError("Missing or invalid ELF section table")
        for index in range(sh_count):
            _, kind, flags, address, start, size, _, _, _, _ = struct.unpack_from("<10I", raw, sh_offset + index * sh_size)
            if not flags & 2 or kind == 8 or not size or address >= FLASH_END:
                continue
            if start + size > len(raw) or address + size > FLASH_END:
                raise ValueError("ELF section outside Flash/file")
            for i, byte in enumerate(raw[start:start + size]):
                if memory.get(address + i) != byte:
                    raise ValueError("ELF section / load segment mismatch")
                selected[address + i] = byte
        if not selected:
            raise ValueError("No allocated Flash sections")
        return selected
    return memory

def read_hex(path):
    memory, base, ended = {}, 0, False
    for line in Path(path).read_text().splitlines():
        if not line.strip():
            continue
        if ended or not line.startswith(":"):
            raise ValueError("Data after EOF or invalid HEX prefix")
        data = bytes.fromhex(line[1:])
        if len(data) < 5 or len(data) != data[0] + 5 or sum(data) & 255:
            raise ValueError("Invalid HEX record length/checksum")
        size, high, low, kind = data[:4]
        address, payload = (high << 8) | low, data[4:-1]
        if kind == 0:
            for i, byte in enumerate(payload):
                dest = base + address + i
                if dest >= FLASH_END or dest in memory:
                    raise ValueError("Out of range/overlapping HEX data")
                memory[dest] = byte
        elif kind in (2, 4) and size == 2 and address == 0:
            base = int.from_bytes(payload, "big") << (4 if kind == 2 else 16)
        elif kind == 1 and size == 0 and address == 0:
            ended = True
        elif kind in (3, 5) and size == 4 and address == 0:
            pass
        else:
            raise ValueError("Unsupported or malformed HEX record")
    if not ended:
        raise ValueError("HEX EOF missing")
    return memory

def record(address, kind, data):
    raw = bytes([len(data), address >> 8, address & 255, kind]) + bytes(data)
    return ":" + (raw + bytes([(-sum(raw)) & 255])).hex().upper()

def write_hex(memory, path):
    lines, upper, addresses, i = [], None, sorted(memory), 0
    while i < len(addresses):
        start = addresses[i]
        current = start >> 16
        if current != upper:
            lines.append(record(0, 4, current.to_bytes(2, "big")))
            upper = current
        chunk = []
        while i < len(addresses) and len(chunk) < 16 and addresses[i] == start + len(chunk) and addresses[i] >> 16 == current:
            chunk.append(memory[addresses[i]])
            i += 1
        lines.append(record(start & 65535, 0, chunk))
    lines.append(record(0, 1, []))
    Path(path).write_text("\n".join(lines) + "\n", encoding="ascii")

def word(memory, address):
    try:
        return int.from_bytes(bytes(memory[address + i] for i in range(4)), "little")
    except KeyError as exc:
        raise ValueError(f"Missing bytes at 0x{address:05x}") from exc

def check(memory):
    # Offsets from TI cc13x2_cc26x2/inc/hw_ccfg.h. Do not reuse for P7/P10.
    for address in range(CCFG, FLASH_END):
        if address not in memory:
            raise ValueError("Incomplete CCFG: cannot certify recovery settings")
    bl, erase, dap = (word(memory, a) for a in (0x57FD8, 0x57FDC, 0x57FE4))
    sp, reset, image = word(memory, 0), word(memory, 4), word(memory, 0x57FEC)
    checks = {
        "bootloader_enabled": bl >> 24 == 0xc5,
        "backdoor_enabled": bl & 255 == 0xc5,
        "backdoor_dio15": (bl >> 8) & 255 == 15,
        "backdoor_active_low": not bool(bl & 0x10000),
        "chip_erase_allowed": bool(erase & 0x100),
        "bank_erase_allowed": bool(erase & 1),
        "cpu_debug_enabled": (dap >> 16) & 255 == 0xc5,
        "vector_base_zero": image == 0,
        "stack_in_sram_aligned": 0x20000000 < sp <= 0x20014000 and sp % 8 == 0,
        "reset_thumb_in_application": bool(reset & 1) and (reset & ~1) < 0x50000 and (reset & ~1) in memory,
        "nv_region_not_in_image": not any(0x50000 <= a < 0x56000 for a in memory),
        "flash_sectors_unprotected": all(word(memory, a) == 0xffffffff for a in range(0x57ff0, 0x58000, 4)),
        "factory_ieee_not_overridden": word(memory, 0x57fc8) == 0xffffffff and word(memory, 0x57fcc) == 0xffffffff,
    }
    return {"profile": "CC2652P-352KiB-SONOFF-DIO15", "checks": checks,
            "passed": all(checks.values()), "hardware_validated": False,
            "bl_config": f"0x{bl:08x}", "bytes_in_image": len(memory)}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=["export", "check"])
    ap.add_argument("input", type=Path)
    ap.add_argument("output", nargs="?", type=Path)
    args = ap.parse_args()
    try:
        memory = read_elf(args.input) if args.input.suffix.lower() in (".elf", ".out") else read_hex(args.input)
        if args.action == "export":
            if not args.output:
                ap.error("export requires output")
            write_hex(memory, args.output)
        else:
            result = check(memory)
            print(json.dumps(result, indent=2))
            raise SystemExit(0 if result["passed"] else 1)
    except (ValueError, OSError, struct.error) as exc:
        raise SystemExit(f"Rejected image: {exc}")

if __name__ == "__main__":
    main()
