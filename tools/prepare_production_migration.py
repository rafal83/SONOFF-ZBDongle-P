"""Copy stopped Z2M data to private storage and inspect without disclosing secrets."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import shutil
import yaml

ROOT = Path(__file__).resolve().parents[1]

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def inspect_data(folder):
    config = yaml.safe_load((folder / "configuration.yaml").read_text(encoding="utf-8"))
    rows = [json.loads(line) for line in (folder / "database.db").read_text(encoding="utf-8").splitlines() if line.strip()]
    backup = json.loads((folder / "coordinator_backup.json").read_text(encoding="utf-8"))
    serial = config.get("serial", {})
    if serial.get("adapter", "zstack") != "zstack": raise ValueError("Expected zstack adapter")
    if backup.get("metadata", {}).get("format") != "zigpy/open-coordinator-backup":
        raise ValueError("Expected unified coordinator backup")
    if len([r for r in rows if r.get("type") == "Coordinator"]) != 1:
        raise ValueError("Expected one coordinator in Z2M database")
    coordinator = next(r["ieeeAddr"] for r in rows if r.get("type") == "Coordinator")
    if coordinator.removeprefix("0x").lower() != backup["coordinator_ieee"].lower():
        raise ValueError("Z2M database and coordinator backup identity differ")
    return dict(database_types=dict(collections.Counter(r.get("type") for r in rows)),
        database_devices=sum(r.get("type") not in ("Coordinator", "Group") for r in rows),
        backup_devices=len(backup["devices"]), configured_baudrate=serial.get("baudrate", 115200),
        backup_format=backup["metadata"]["format"], ieee=coordinator)

def snapshot(source, destination):
    source, destination = source.resolve(), destination.resolve()
    if not destination.is_relative_to((ROOT / "private").resolve()): raise ValueError("Backups must remain private")
    if destination.exists(): raise FileExistsError("Never overwrite a migration snapshot")
    required = ["configuration.yaml", "database.db", "coordinator_backup.json"]
    initial = {name: sha(source / name) for name in required}
    # Copy all data, converters, extensions, icons, state and historical files.
    # Do not follow a link outside the explicitly named source directory.
    for item in source.rglob("*"):
        if item.is_symlink(): raise ValueError("Source contains a link; resolve backup scope first")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination)
    if any(sha(source / name) != initial[name] or sha(destination / name) != initial[name] for name in required):
        raise ValueError("Critical Z2M files changed during capture")
    manifest = {p.relative_to(destination).as_posix(): sha(p) for p in destination.rglob("*") if p.is_file()}
    for name, expected in manifest.items():
        if sha(source / name) != expected: raise ValueError("Source changed during copy")
    report = dict(passed=True, source=str(source), files=len(manifest), sha256=manifest,
        **inspect_data(destination))
    (destination.parent / "z2m-snapshot.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--destination", type=Path, required=True)
    args = ap.parse_args()
    report = snapshot(args.source, args.destination)
    print(json.dumps({k: report[k] for k in ["passed", "files", "database_types", "database_devices", "backup_devices", "configured_baudrate"]}))

if __name__ == "__main__": main()
