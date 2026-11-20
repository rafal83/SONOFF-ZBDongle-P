"""PRODUCTION comparison with the published Koenkk Z-Stack coordinator
(CC1352P2_CC2652P_launchpad_coordinator_20250321, tools/fetch_reference_coordinator.py).
Explicit operator consent required (--confirm-production-compare).

FreeRTOS -> Koenkk, without counter regression:
  1. double full capture of the dongle (same ROM session as the programming);
  2. the fresh backup (tools/production_nv_snapshot.cjs, < 1 h) NWK counter
     + 2500 (herdsman restore increment) must exceed every counter the
     FreeRTOS store has reserved;
  3. Koenkk application programmed in 0..0x47FFF; the FreeRTOS store
     (0x48000..0x4FFFF), the Z-Stack NV and the CCFG are kept byte for byte
     (program_application refuses otherwise); full read-back compared;
  4. Koenkk identity checked (revision 20250321, IEEE) at 115200 baud;
  5. Z-Stack network state cleared (STARTUP_OPTION), so that herdsman then
     RESTORES the fresh backup (tools/znp_restore_backup.cjs).
The way back is the FreeRTOS flash (store kept) + DIAG_STORE_FORMAT "WIPE"
(counters kept as floors) + restore of a fresh backup read under Koenkk.
"""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from image import read_hex  # noqa: E402

KOENKK_HEX = ROOT / "build/reference-coordinator/CC1352P2_CC2652P_launchpad_coordinator_20250321.hex"
KOENKK_REVISION = 20250321
KOENKK_BAUD = 115200
STORE_START, NV_START, FLASH_SIZE, CCFG = 0x48000, 0x50000, 0x58000, 0x57FA8


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--usb-serial", required=True)
    ap.add_argument("--expect-ieee", required=True)
    ap.add_argument("--fresh-backup", type=Path, required=True)
    ap.add_argument("--session-dir", type=Path, required=True)
    ap.add_argument("--confirm-production-compare", action="store_true")
    args = ap.parse_args()
    if not args.confirm_production_compare:
        ap.error("--confirm-production-compare is required (explicit consent of the network owner)")
    import freertos_lab_session as fls
    import freertos_store
    from migrate_production import capture_twice, write_verified
    from run_dongle_session import open_rom, boot, wait_mt

    folder = args.session_dir.resolve()
    if not folder.is_relative_to((ROOT / "private").resolve()):
        raise ValueError("--session-dir must be under private/")
    folder.mkdir(parents=True, exist_ok=False)
    fresh_path = args.fresh_backup.resolve()
    if time.time() - fresh_path.stat().st_mtime > 3600:
        raise ValueError("--fresh-backup is older than 1 h: read it again")
    fresh = json.loads(fresh_path.read_text(encoding="utf-8"))
    if "0x" + fresh["coordinator_ieee"].lower() != args.expect_ieee.lower():
        raise ValueError("The fresh backup is not this coordinator's")
    fc = int(fresh["network_key"]["frame_counter"])
    koenkk = read_hex(KOENKK_HEX)
    report = {"action": "koenkk-compare", "koenkk_hex": str(KOENKK_HEX.relative_to(ROOT)),
              "koenkk_hex_sha256": fls.sha(KOENKK_HEX.read_bytes()), "fresh_frame_counter": fc}

    command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
    try:
        current = capture_twice(command, folder, "before-koenkk")
        reserved = freertos_store.counters(freertos_store.records(current))
        report["freertos_reserved_counters"] = reserved
        if reserved is None:
            raise RuntimeError("No FreeRTOS counters record found in the store")
        if fc + 2500 <= reserved[0]:
            raise RuntimeError(f"Backup counter {fc} + 2500 does not exceed the reserved {reserved[0]}")
        for a in range(CCFG, FLASH_SIZE):
            if a in koenkk and koenkk[a] != current[a]:
                raise RuntimeError("Koenkk CCFG differs from the dongle CCFG")
        if any(STORE_START <= a < CCFG for a in koenkk):
            raise RuntimeError("Koenkk image overlaps the store / NV")
        target = bytearray(current)
        target[:STORE_START] = b"\xff" * STORE_START
        for a, b in koenkk.items():
            if a < STORE_START:
                target[a] = b
        report["programmed"] = write_verified(command, device, bytes(target), current, folder, "koenkk")
        boot(command)
    finally:
        command.close()

    mt, capability = wait_mt(args.port, timeout=30, baud=KOENKK_BAUD)
    try:
        version, _ = mt.request(0x21, 2)
        ieee, _ = mt.request(0x21, 4)
        report["koenkk"] = {"product": version[1], "revision": int.from_bytes(version[5:9], "little"),
                            "ieee_matches": int.from_bytes(ieee, "little") == int(args.expect_ieee, 16)}
    finally:
        mt.close()
    if report["koenkk"]["revision"] != KOENKK_REVISION or not report["koenkk"]["ieee_matches"]:
        raise RuntimeError("The running firmware is not the expected Koenkk image")
    fls.INSTALLED_BAUD = KOENKK_BAUD
    fls.clear_zstack_state(args.port, report)
    (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({**report, "next": "node tools/znp_restore_backup.cjs --baudrate 115200 --backup <fresh>"}, indent=2))


if __name__ == "__main__":
    main()
