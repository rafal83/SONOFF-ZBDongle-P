"""Capture two identical main-Flash images through TI ROM, without programming.

Resets into BSL, leaves the MCU in BSL after success. Uses the pinned public
cc2538-bsl transport. No erase/download/write/CCFG command is issued here.
"""
import argparse
import binascii
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import time
from dongle_probe import identify_port

ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 0x58000
BSL_COMMIT = "28cce9749bfb73c50363ded01bd8001b1639d9fb"

def read_words(command, address, count):
    # TI SWCU185G 10.2.3.10: <=63 accesses of 32 bits per read packet.
    if address % 4 or not 1 <= count <= 63 or address < 0 or address + count * 4 > FLASH_SIZE:
        raise ValueError("Read outside the confirmed CC2652P main Flash")
    payload = bytes([0x2a]) + address.to_bytes(4, "big") + bytes([1, count])
    command._write(bytes([9, sum(payload) & 255]) + payload)
    if not command._wait_for_ack("main-Flash read", 2): raise IOError("ROM rejected read")
    data = bytes(command.receivePacket())
    if len(data) != count * 4 or not command.checkLastCmd(): raise IOError("ROM read length/status failure")
    return data

def capture(command, output):
    path = output.with_suffix(output.suffix + ".part")
    crc = 0
    previous_progress = -1
    with path.open("xb") as stream:
        for address in range(0, FLASH_SIZE, 252):
            data = read_words(command, address, min(63, (FLASH_SIZE - address) // 4))
            stream.write(data); crc = binascii.crc32(data, crc)
            progress = (address + len(data)) // 65536
            if progress != previous_progress:
                print(f"{output.name}: {address + len(data)}/{FLASH_SIZE} bytes", flush=True)
                previous_progress = progress
        stream.flush(); os.fsync(stream.fileno())
    crc &= 0xffffffff
    target_crc = command.cmdCRC32CC26xx(0, FLASH_SIZE)
    if crc != target_crc: raise IOError("ROM CRC32 does not match captured image")
    if output.exists(): raise FileExistsError(output)
    path.rename(output)
    return crc

def load_bsl():
    source = ROOT / "upstream/cc2538-bsl"
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if commit != BSL_COMMIT: raise ValueError("BSL source commit mismatch")
    subprocess.run(["git", "-C", str(source), "diff", "--exit-code", "HEAD", "--", "cc2538_bsl/cc2538_bsl.py"], check=True)
    path = source / "cc2538_bsl/cc2538_bsl.py"
    spec = importlib.util.spec_from_file_location("pinned_cc_bsl", path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    module.QUIET = 0
    return module

def main():
    import serial
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--usb-serial", required=True)
    ap.add_argument("--expect-ieee", required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    identity = identify_port(args.port, args.usb_serial)
    destination = args.output.resolve()
    private = (ROOT / "private").resolve()
    if not destination.is_relative_to(private): raise ValueError("Backups must remain in private/")
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("original-a.bin", "original-b.bin", "capture.json"):
        if (destination / name).exists(): raise FileExistsError("Refusing to overwrite an existing backup")
    bsl = load_bsl()
    command = bsl.CommandInterface()
    command.sp = serial.serial_for_url(args.port, do_not_open=True, baudrate=500000, timeout=2, write_timeout=2)
    command.sp.dtr = False; command.sp.rts = False
    command.sp.open()
    try:
        command.invoke_bootloader(sonoff_usb=True)
        if not command.sendSynch(): raise IOError("ROM synchronization failed; Flash was not modified")
        chip_id = command.cmdGetChipId()
        device = bsl.CC26xx(command)
        if device.size != FLASH_SIZE or device.page_size != 8192: raise ValueError("Unexpected Flash geometry")
        primary = bytes(command.cmdMemReadCC26xx(0x500012f0)) + bytes(command.cmdMemReadCC26xx(0x500012f4))
        ieee = "0x" + primary[::-1].hex()
        if int(ieee, 16) != int(args.expect_ieee, 16): raise ValueError("ROM IEEE does not match the authorized dongle")
        print(f"Confirmed CC26x2-class target: {device.size} bytes main Flash; ROM read access works", flush=True)
        started = time.monotonic()
        first, second = destination / "original-a.bin", destination / "original-b.bin"
        crc_a, crc_b = capture(command, first), capture(command, second)
        a, b = first.read_bytes(), second.read_bytes()
        if a != b or crc_a != crc_b: raise IOError("Two main-Flash captures differ; no programming is permitted")
        sp, reset = struct.unpack_from("<II", a)
        bl = struct.unpack_from("<I", a, 0x57fd8)[0]
        if not (0x20000000 < sp <= 0x20014000 and reset & 1 and reset < 0x50000): raise ValueError("Invalid original vectors")
        if bl >> 24 != 0xc5 or bl & 255 != 0xc5 or (bl >> 8) & 255 != 15 or bl & 0x10000:
            raise ValueError("Original CCFG does not retain the expected ROM recovery path")
        nv = a[0x50000:0x56000]
        if all(byte == 255 for byte in nv): raise ValueError("Expected production NV is empty")
        result = dict(identity, passed=True, ieee=ieee, chip_id=chip_id, flash_size=FLASH_SIZE,
            flash_sha256=hashlib.sha256(a).hexdigest(), nv_sha256=hashlib.sha256(nv).hexdigest(),
            crc32=f"{crc_a:08x}", two_captures_identical=True, both_captures_match_ROM_CRC32=True,
            bl_config=f"{bl:08x}", programming_performed=False, ROM_at_last_command=True,
            serial_close_may_reboot_application=True,
            duration_seconds=round(time.monotonic()-started, 2), bsl_commit=BSL_COMMIT)
        (destination / "capture.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2), flush=True)
    finally: command.close()

if __name__ == "__main__": main()
