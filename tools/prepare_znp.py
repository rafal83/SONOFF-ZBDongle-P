"""Extract the pinned coordinator patch. Optional SDK patch/build; never flash or serial access."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROFILE = json.loads((ROOT / "firmware/config/znp-profile.json").read_text())

def extract_patch(text):
    sections = re.split(r"(?=^diff --git )", text, flags=re.MULTILINE)[1:]
    selected = []
    for section in sections:
        path = section.splitlines()[0].split(" b/", 1)[1]
        if path.startswith("source/") or path.startswith("workspace/" + PROFILE["project"] + "/"):
            selected.append(section)
    if not selected:
        raise ValueError("No patch sections found")
    return "".join(selected)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk", type=Path, help="Complete SDK 8.30.01.01 with CCS project imported into sdk/workspace")
    ap.add_argument("--apply", action="store_true", help="Apply checked patch to a dedicated SDK copy inside this project")
    ap.add_argument("--ccs-cli", type=Path, help="Build with the installed CCS 20.1.0 ccs-server-cli after --apply")
    args = ap.parse_args()
    upstream = ROOT / "upstream/Z-Stack-firmware"
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    commit = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
    if commit != lock["Z-Stack-firmware"]["commit"]:
        raise SystemExit("Z-Stack-firmware revision mismatch")
    raw = (upstream / "coordinator/Z-Stack_3.x.0/firmware.patch").read_text(encoding="utf-8")
    patch = extract_patch(raw)
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    destination = build / "znp-sonoff-baseline.patch"
    destination.write_text(patch, encoding="utf-8", newline="\n")
    (build / "ZNP-PATCH-LICENSE.txt").write_bytes((upstream / "LICENSE").read_bytes())
    report = {"profile": PROFILE, "patch_sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
              "built": False, "hardware_tested": False}
    if args.sdk:
        sdk = args.sdk.resolve()
        project = sdk / "workspace" / PROFILE["project"]
        required = [sdk / "license_simplelink_cc13xx_cc26xx_sdk_8_30_01_01.txt",
                    sdk / "source/ti/zstack/npi/npi_tl_uart.c", project / ".project", project / "znp.syscfg"]
        missing = [str(p) for p in required if not p.is_file()]
        if missing:
            report["missing"] = missing
            (build / "znp-prerequisites.json").write_text(json.dumps(report, indent=2) + "\n")
            raise SystemExit("Complete SDK / imported CCS project missing; see build/znp-prerequisites.json")
        # No implicit 'ignore-space-change': a source mismatch must be reviewed.
        if not sdk.is_relative_to(ROOT):
            raise SystemExit("SDK must be a dedicated copy below this workspace")
        gitapply = ["git", "apply", "--directory=" + sdk.relative_to(ROOT).as_posix()]
        subprocess.run([*gitapply, "--check", str(destination)], cwd=ROOT, check=True)
        if args.apply:
            if not sdk.is_relative_to(ROOT) or sdk == ROOT or sdk == ROOT / "upstream/ti-sdk":
                raise SystemExit("--apply requires a dedicated SDK copy below this project; audited upstream/ti-sdk is read-only")
            subprocess.run([*gitapply, str(destination)], cwd=ROOT, check=True)
            header = (sdk / "source/preinclude.h").read_text(encoding="utf-8")
            if not re.search(r"#define LAUNCHPAD_CONFIG\s+1\b", header):
                raise SystemExit("Unexpected RF profile")
            report["applied"] = True
        if args.ccs_cli:
            if not args.apply:
                raise SystemExit("--ccs-cli requires --apply on a fresh SDK copy")
            subprocess.run([str(args.ccs_cli.resolve()), "-noSplash", "-workspace", str(sdk / "workspace"),
                "-application", "com.ti.ccs.apps.projectBuild", "-ccs.projects", PROFILE["project"],
                "-ccs.configuration", "default", "-ccs.buildType", "full", "-ccs.listErrors"], check=True)
            image = project / "default" / (PROFILE["project"] + ".hex")
            if not image.is_file():
                raise SystemExit("CCS did not produce the expected HEX")
            subprocess.run([sys.executable, str(ROOT / "tools/image.py"), "check", str(image)], check=True)
            report.update(built=True, hex_sha256=hashlib.sha256(image.read_bytes()).hexdigest())
    elif args.apply or args.ccs_cli:
        ap.error("--sdk is required")
    (build / "znp-preparation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    main()
