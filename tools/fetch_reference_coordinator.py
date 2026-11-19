"""Fetch and statically check the published Koenkk ZNP coordinator for SONOFF ZBDongle-P."""
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile
from image import read_hex, check

ROOT = Path(__file__).resolve().parents[1]
NAME = "CC1352P2_CC2652P_launchpad_coordinator_20250321"
URL = f"https://github.com/Koenkk/Z-Stack-firmware/releases/download/Z-Stack_3.x.0_coordinator_20250321/{NAME}.zip"
SHA256 = "66f5d3d20c2ca5375133da6a47b959525e39d707736a4d1b01b44bd95b04273a"

def main():
    folder = ROOT / "build/reference-coordinator"
    folder.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(URL, headers={"User-Agent": "sonoff-port-build/1.0"})
    with urllib.request.urlopen(request, timeout=45) as response:
        raw = response.read(2_000_001)
    if len(raw) > 2_000_000 or hashlib.sha256(raw).hexdigest() != SHA256:
        raise SystemExit("Published archive SHA-256 mismatch")
    archive = folder / (NAME + ".zip")
    archive.write_bytes(raw)
    with zipfile.ZipFile(io.BytesIO(raw)) as package:
        images = [name for name in package.namelist() if Path(name).name == NAME + ".hex"]
        if len(images) != 1:
            raise SystemExit("Expected exactly one coordinator HEX")
        hexfile = folder / (NAME + ".hex")
        hexfile.write_bytes(package.read(images[0]))
    result = check(read_hex(hexfile))
    result.update(source="Published Koenkk Z-Stack-firmware coordinator release", url=URL,
                  archive_sha256=SHA256, hex_sha256=hashlib.sha256(hexfile.read_bytes()).hexdigest(),
                  built_locally=False, hardware_tested_locally=False,
                  intended_board="SONOFF ZBDongle-P CC2652P, launchpad RF profile",
                  adapter="zstack", baudrate=115200, rtscts=False)
    (folder / "manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for source, name in [(ROOT / "upstream/Z-Stack-firmware/LICENSE", "KOENKK-LICENSE.txt"),
                         (ROOT / "upstream/ti-sdk/license_simplelink_cc13xx_cc26xx_sdk_8_30_01_01.txt", "TI-LICENSE.txt")]:
        if source.is_file(): (folder / name).write_bytes(source.read_bytes())
    print(json.dumps(result, indent=2))
    if not result["passed"]:
        raise SystemExit("Reference image did not pass the recovery configuration checks")

if __name__ == "__main__":
    main()
