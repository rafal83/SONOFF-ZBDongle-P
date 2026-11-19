"""Explicit installation/rollback preserving the CURRENT production NV.

Never calls a lab harness, ZDO startup, permitJoin or scratch-NV tests.
Rollback combines original program/CCFG with freshly captured CURRENT NV;
an old full-flash snapshot must not rewind security counters after RF use.
"""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from capture_dongle import FLASH_SIZE, capture
from image import CCFG, read_hex, check
from run_dongle_session import open_rom, boot

ROOT = Path(__file__).resolve().parents[1]
NV_START, NV_END = 0x50000, 0x56000
ORIGINAL_SHA = "97bf857c98344d8ce82cd2ef186f7152727aad58e4a9f8f57e89b344bd46f417"
TESTED_SHA = "cb2b5e4f5cae02a5601c6c1c8d3cc9d7be7a009cfa2fb7398d323f39eb31dee5"

def sha(data): return hashlib.sha256(data).hexdigest()

def same_program(a, b):
    return len(a) == len(b) == FLASH_SIZE and a[:NV_START] == b[:NV_START] and a[NV_END:] == b[NV_END:]

def with_current_nv(program, current):
    if len(program) != FLASH_SIZE or len(current) != FLASH_SIZE:
        raise ValueError("Expected complete CC2652P images")
    nv = current[NV_START:NV_END]
    if nv == b"\xff" * len(nv): raise ValueError("Refusing an empty network")
    result = bytearray(program)
    result[NV_START:NV_END] = nv
    result[CCFG:] = current[CCFG:]
    sparse = {i: byte for i, byte in enumerate(result) if not NV_START <= i < NV_END}
    if not check(sparse)["passed"]: raise ValueError("Recovery configuration check failed")
    return bytes(result)

def capture_twice(command, folder, prefix):
    a, b = folder / (prefix + "-a.bin"), folder / (prefix + "-b.bin")
    capture(command, a); capture(command, b)
    if a.read_bytes() != b.read_bytes(): raise ValueError("Two ROM captures differ; refusing erase")
    return a.read_bytes()

def program_application(command, device, data, current):
    """Erase/program only 0..0x4ffff. Never touch NV or the CCFG sector."""
    if len(data) != FLASH_SIZE or len(current) != FLASH_SIZE or device.page_size != 8192:
        raise ValueError("Unexpected image or sector geometry")
    if data[NV_START:] != current[NV_START:]:
        raise ValueError("Application-only flashing cannot alter NV, reserved sectors or CCFG")
    sparse = {i: byte for i, byte in enumerate(data) if not NV_START <= i < NV_END}
    if not check(sparse)["passed"]: raise ValueError("Refusing unrecoverable application")
    if not device.eraseRange(0, NV_START): raise IOError("Application sector erase failed")
    if not command.writeMemory(0, bytearray(data[:NV_START])): raise IOError("Application programming failed")
    crc = binascii.crc32(data) & 0xffffffff
    if command.cmdCRC32CC26xx(0, FLASH_SIZE) != crc: raise IOError("Full Flash CRC mismatch")
    return f"{crc:08x}"

def write_verified(command, device, data, current, folder, prefix):
    crc = program_application(command, device, data, current)
    output = folder / (prefix + "-readback.bin")
    capture(command, output)
    if output.read_bytes() != data: raise ValueError("Full programmed-image readback mismatch")
    return dict(crc32=crc, full_readback_verified=True, sha256=sha(data))

def network_snapshot(args, folder, output, revision, baud, compare):
    command = ["node", str(ROOT / "tools/production_nv_snapshot.cjs"), "--port", args.port,
        "--baudrate", str(baud), "--expected-revision", str(revision), "--expect-ieee", args.expect_ieee,
        "--database", str(folder / "z2m-data/database.db"),
        "--old-backup", str(folder / "z2m-data/coordinator_backup.json"),
        "--compare", str(compare), "--output", str(output)]
    with output.with_suffix(".process.log").open("xb") as log:
        run = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    if run.returncode: raise RuntimeError("Network verification failed; diagnostics retained privately")
    report = json.loads(Path(str(output) + ".report.json").read_text())
    if not report["passed"] or report["database_devices_absent_from_backup"]:
        raise ValueError("Not all Z2M devices are represented in the network backup")
    return report

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=["install", "rollback"])
    ap.add_argument("--port", required=True)
    ap.add_argument("--usb-serial", required=True)
    ap.add_argument("--expect-ieee", required=True)
    ap.add_argument("--session-dir", required=True, type=Path)
    ap.add_argument("--z2m-stopped", required=True, action="store_true")
    args = ap.parse_args()
    folder = args.session_dir.resolve()
    if not folder.is_relative_to((ROOT / "private").resolve()): raise ValueError("Session must remain private")
    snapshot = json.loads((folder / "z2m-snapshot.json").read_text())
    for name, expected in snapshot["sha256"].items():
        if sha((folder / "z2m-data" / name).read_bytes()) != expected:
            raise ValueError("Z2M snapshot changed")
    if snapshot["ieee"].lower() != args.expect_ieee.lower(): raise ValueError("Snapshot identity mismatch")
    original = (ROOT / "private/dongle-20261117/original-a.bin").read_bytes()
    if sha(original) != ORIGINAL_SHA: raise ValueError("Known original image changed")
    image = ROOT / "build/coordinator-optimized-1000000/sonoff-znp-coordinator.hex"
    if sha(image.read_bytes()) != TESTED_SHA: raise ValueError("Candidate differs from physically tested image")
    memory = read_hex(image)
    if not check(memory)["passed"]: raise ValueError("Candidate recovery checks failed")
    candidate = bytearray(b"\xff" * FLASH_SIZE)
    for address, value in memory.items(): candidate[address] = value
    proof = json.loads((image.parent / "hardware-tests.json").read_text())
    if not (proof["experimental_hex_sha256"] == TESTED_SHA and proof["baudrate"] == 1000000
            and proof["radio"]["radio_and_NV_load_test"] and proof["restore_verified"]):
        raise ValueError("Candidate hardware evidence is missing")
    before = folder / "network-before.json"
    baseline = json.loads(Path(str(before) + ".report.json").read_text())
    if not baseline["passed"] or baseline["database_devices_absent_from_backup"]:
        raise ValueError("Original network backup is incomplete")
    if args.action == "rollback":
        installed = json.loads((folder / "install/report.json").read_text())
        if not installed.get("ready_for_Z2M") or installed["candidate_hex_sha256"] != TESTED_SHA:
            raise ValueError("No verified installation in this session")
    work = folder / args.action
    work.mkdir()  # A session/action can never overwrite prior recovery evidence.
    report = dict(action=args.action, passed=False, candidate_hex_sha256=TESTED_SHA,
        production_NV_preserved=True, commissioning_commands_sent=False, Z2M_started=False,
        NV_and_CCFG_never_erased_or_programmed=True)
    command = None
    touched = False
    try:
        pre = work / "network-preflight.json"
        network_snapshot(args, folder, pre, 20240710 if args.action == "install" else 20261114,
                         115200 if args.action == "install" else 1000000, before)
        command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
        current = capture_twice(command, work, "current")
        if args.action == "install":
            if not same_program(current, original): raise ValueError("Current program is not the saved original")
            program = bytes(candidate)
        else:
            installed_program = (folder / "install/target.bin").read_bytes()
            if sha(installed_program) != installed["programmed"]["sha256"] or not same_program(current, installed_program):
                raise ValueError("Current program no longer matches the installed candidate")
            program = original
        target = with_current_nv(program, current)
        (work / "target.bin").write_bytes(target)
        report["NV_bytes_preserved_exactly_before_boot"] = target[NV_START:NV_END] == current[NV_START:NV_END]
        report["CCFG_preserved_exactly"] = target[CCFG:] == current[CCFG:]
        touched = True
        report["programmed"] = write_verified(command, device, target, current, work, "target")
        boot(command); command.close(); command = None
        report["network_verification"] = network_snapshot(args, folder, work / "network-after.json",
            20261114 if args.action == "install" else 20240710,
            1000000 if args.action == "install" else 115200, pre)
        report.update(passed=True, ready_for_Z2M=True, required_Z2M_baudrate=1000000 if args.action == "install" else 115200)
    except BaseException:
        report["failed"] = True
        if touched and args.action == "install":
            # Boot or verification may already have advanced durable counters.
            # Always capture them again; never roll back to an older NV snapshot.
            try:
                if command: command.close(); command = None
                command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
                latest = capture_twice(command, work, "failure-current")
                recovery = with_current_nv(original, latest)
                report["automatic_recovery"] = write_verified(command, device, recovery, latest, work, "recovery")
                boot(command); command.close(); command = None
                report["recovered_original_network"] = network_snapshot(args, folder, work / "network-recovered.json", 20240710, 115200, before)
            except BaseException:
                report["automatic_recovery_failed"] = True
        raise
    finally:
        if command:
            # Never leave an untouched original parked in ROM after a preflight failure.
            if not touched:
                try: boot(command)
                except Exception: pass
            command.close()
        (work / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))

if __name__ == "__main__": main()
