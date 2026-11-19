"""Compare original code and fixed-baud candidates, restoring original each time.

Explicit hardware session. Every profile runs on fresh LAB NV. The original
program code/CCFG are byte-identical to the user's dump; only lab NV is blank.
No proprietary binary is decompiled or distributed. Raw logs remain private.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from image import check, read_hex, write_hex

ROOT = Path(__file__).resolve().parents[1]

def sha(data): return hashlib.sha256(data).hexdigest()

def original_lab_image(folder):
    original = (folder / "original-a.bin").read_bytes()
    capture = json.loads((folder / "capture.json").read_text())
    if (len(original) != 0x58000 or original != (folder / "original-b.bin").read_bytes()
            or sha(original) != capture["flash_sha256"] or not capture["passed"]):
        raise ValueError("Original backups do not match their verified capture")
    memory = {i: byte for i, byte in enumerate(original) if not 0x50000 <= i < 0x56000}
    if not check(memory)["passed"]: raise ValueError("Original lab image failed recovery checks")
    destination = folder / "performance-original-20240710"
    destination.mkdir(exist_ok=True)
    image = destination / "sonoff-znp-coordinator.hex"
    write_hex(memory, image)
    actual = read_hex(image)
    if actual != memory: raise ValueError("Original code export mismatch")
    manifest = dict(hex_sha256=sha(image.read_bytes()), baudrate=115200, optimized=False,
        auto_baud=False, znp_version={"revision": 20240710}, private_original_code=True,
        original_code_and_CCFG_unchanged=True, production_NV_included=False)
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return image

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--usb-serial", required=True)
    ap.add_argument("--backup-dir", required=True, type=Path)
    ap.add_argument("--test-and-restore", required=True, action="store_true")
    ap.add_argument("--samples", type=int, default=40)
    ap.add_argument("--rounds", type=int, default=3)
    args = ap.parse_args()
    folder = args.backup_dir.resolve()
    if not folder.is_relative_to((ROOT / "private").resolve()): raise ValueError("Hardware results must remain private")
    original = original_lab_image(folder)
    profiles = [("original_115200_before", original)]
    for baud in (115200, 460800, 1000000):
        suffix = "" if baud == 115200 else "-" + str(baud)
        profiles.append((f"candidate_{baud}", ROOT / ("build/coordinator-optimized" + suffix) / "sonoff-znp-coordinator.hex"))
    profiles.append(("original_115200_after", original))
    result = {"date": "2026-11-17", "original_revision": 20240710, "profiles": [],
        "radio_tested": False, "Z2M_used": False, "production_NV_used": False,
        "method": "Same dongle, fresh synthetic NV per profile; randomized case order; original bracketed before/after"}
    for label, image in profiles:
        print("Starting profile:", label, flush=True)
        report_path = folder / "hardware-session.json"
        previous_report = report_path.read_bytes() if report_path.exists() else None
        with (folder / ("performance-" + label + ".log")).open("wb") as log:
            run = subprocess.run([sys.executable, str(ROOT / "tools/run_dongle_session.py"),
                "--port", args.port, "--usb-serial", args.usb_serial, "--backup-dir", str(folder),
                "--experimental", str(image), "--test-and-restore", "--transport-benchmark",
                "--benchmark-samples", str(args.samples), "--benchmark-rounds", str(args.rounds)],
                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        current_report = report_path.read_bytes()
        if current_report == previous_report:
            raise RuntimeError("No fresh session report; stopping rather than reusing stale evidence")
        report = json.loads(current_report)
        if report["experimental_hex_sha256"] != sha(image.read_bytes()):
            raise RuntimeError("Session report does not belong to the requested image")
        if not report["restore_verified"] or report.get("restore_error"):
            raise RuntimeError("Original restoration failed; stopping the entire matrix")
        entry = {"label": label, "hex_sha256": report["experimental_hex_sha256"],
                 "baudrate": report["baudrate"], "restore_verified": True,
                 "passed": run.returncode == 0 and report.get("performance", {}).get("passed", False),
                 "rollback_session": Path(report["rollback_directory"]).name}
        if "performance" in report: entry["measurements"] = report["performance"]
        if "test_error" in report: entry["error"] = report["test_error"]
        result["profiles"].append(entry)
        (folder / "performance-matrix.json").write_text(json.dumps(result, indent=2) + "\n")
        print("Finished:", label, "passed:", entry["passed"], "original restored:", True, flush=True)
        if label.startswith("original") and not entry["passed"]:
            raise RuntimeError("Original reference failed; no performance claim is possible")
    print("Matrix complete. Original firmware restored after every profile.", flush=True)

if __name__ == "__main__": main()
