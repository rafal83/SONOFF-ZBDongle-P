"""Download TI SDK / CCS official Windows packages into this workspace and verify published hashes."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def device_support_source(name, sdk_name):
    """Select source files used by FreeRTOS, never precompiled stack libraries."""
    if '\\' in name or '..' in PurePosixPath(name).parts or PurePosixPath(name).is_absolute():
        return False
    prefix = sdk_name + '/'
    if not name.startswith(prefix):
        return False
    relative = name[len(prefix):]
    if relative in (f'license_{sdk_name}.txt', f'manifest_{sdk_name}.html',
                    'source/ti/devices/DeviceFamily.h'):
        return True
    return (relative.startswith('source/ti/devices/cc13x2_cc26x2/')
            and Path(relative).suffix.lower() in ('.c', '.h', '.inc'))
PACKAGES = {
    "sdk833": {
        "url": "https://dr-download.ti.com/software-development/software-development-kit-sdk/MD-BPlR3djvTV/8.33.00.16/simplelink_cc13xx_cc26xx_sdk_8_33_00_16__win.zip",
        "sha256": "f61ccd1727bc54819506b4acc3b3a2891d0dd802df1c421700e93c1b8f5d3688",
    },
    "sdk": {
        "url": "https://dr-download.ti.com/software-development/software-development-kit-sdk/MD-BPlR3djvTV/8.30.01.01/simplelink_cc13xx_cc26xx_sdk_8_30_01_01__win.zip",
        "sha256": "2531e2fb7f1b2bd0384d3adb2717b621b0218706e1bc2063728517c195006fe4",
    },
    "ccs": {
        "url": "https://dr-download.ti.com/software-development/ide-configuration-compiler-or-debugger/MD-J1VdearkvK/20.1.0/CCS_20.1.0.00006_win.zip",
        "sha256": "883ec6f29efb8d226b573ecbd91c8f8c6af0c6ed6b4ada01b9cbc380708699f1",
    },
    "compiler": {
        "url": "https://dr-download.ti.com/software-development/ide-configuration-compiler-or-debugger/MD-ayxs93eZNN/4.0.2.LTS/ti_cgt_armllvm_4.0.2.LTS_windows-x64_installer.exe",
        "sha256": "8e9d809ee37348be9b1f573babd1b51eb95bf09b7bd38818503be1d93a5dd5f2",
    },
    "sysconfig": {
        "url": "https://dr-download.ti.com/software-development/ide-configuration-compiler-or-debugger/MD-nsUM6f7Vvb/1.23.0.4000/sysconfig-1.23.0_4000-setup.exe",
        "sha256": "de8387fc7c980299ebceb7531d39baf697480cdaf376b64bc3e52ad2966bce35",
    },
}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("component", choices=PACKAGES)
    ap.add_argument("--extract", action="store_true")
    ap.add_argument("--device-support-only", action="store_true",
                    help="Extract only TI device-support sources and licences, no closed stack binaries")
    args = ap.parse_args()
    package = PACKAGES[args.component]
    is_sdk = args.component in ("sdk", "sdk833")
    if args.device_support_only and (not is_sdk or not args.extract):
        ap.error('--device-support-only requires an SDK component and --extract')
    sdk_name = "simplelink_cc13xx_cc26xx_sdk_" + ("8_33_00_16" if args.component == "sdk833" else "8_30_01_01")
    cache = ROOT / ".tooling/downloads"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / package["url"].rsplit("/", 1)[1]
    if not archive.exists():
        request = urllib.request.Request(package["url"], headers={"User-Agent": "sonoff-port-build/1.0"})
        print("Downloading", args.component, flush=True)
        with urllib.request.urlopen(request, timeout=60) as response, archive.open("wb") as stream:
            total, next_report = 0, 128 * 1024 * 1024
            while chunk := response.read(1024 * 1024):
                stream.write(chunk)
                total += len(chunk)
                if total >= next_report:
                    print(args.component, total // (1024 * 1024), "MiB", flush=True)
                    next_report += 128 * 1024 * 1024
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != package["sha256"]:
        raise SystemExit("TI archive checksum mismatch; retained for inspection; no extraction")
    print("Verified", archive.name, digest, flush=True)
    if archive.suffix != ".zip":
        if args.extract:
            raise SystemExit("Use the verified TI standalone installer in unattended mode")
        return
    with zipfile.ZipFile(archive) as package_zip:
        names = package_zip.namelist()
        print("Archive entries:", len(names), "roots:", sorted({name.split('/')[0] for name in names})[:15], flush=True)
        if args.extract:
            destination = (ROOT / ("vendor-sdk" if is_sdk else ".tooling/ccs-installer")).resolve()
            if is_sdk and (destination / sdk_name / "source/preinclude.h").exists():
                raise SystemExit("SDK already patched: refusing to overwrite the prepared coordinator sources")
            destination.mkdir(parents=True, exist_ok=True)
            for item in package_zip.infolist():
                if not (destination / item.filename).resolve().is_relative_to(destination):
                    raise SystemExit("Archive path escapes destination")
                if (item.external_attr >> 16) & 0o170000 == 0o120000:
                    raise SystemExit("Unexpected symbolic link in Windows archive")
            print("Extracting to", destination, flush=True)
            selected = [item for item in package_zip.infolist()
                        if not args.device_support_only or device_support_source(item.filename, sdk_name)]
            package_zip.extractall(destination, members=selected)
            if is_sdk:
                (destination / sdk_name / ".extraction-verified.json").write_text(
                    json.dumps({"archive_sha256": digest, "entries": len(selected),
                                "selection": "device-support-sources" if args.device_support_only else "complete-sdk"}) + "\n", encoding="utf-8")
            print("Extracted", args.component, flush=True)

if __name__ == "__main__":
    main()
