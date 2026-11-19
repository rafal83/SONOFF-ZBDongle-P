"""Prepare/apply a minimal baudrate edit against an exact saved Z2M config."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import yaml

def sha(data): return hashlib.sha256(data).hexdigest()

def change_baud(raw, baudrate):
    if baudrate not in (115200, 1000000): raise ValueError("Unexpected baudrate")
    text = raw.decode("utf-8")
    root = yaml.compose(text)
    if not isinstance(root, yaml.MappingNode): raise ValueError("Expected YAML mapping")
    matches = [value for key, value in root.value if key.value == "serial"]
    if len(matches) != 1 or not isinstance(matches[0], yaml.MappingNode) or matches[0].flow_style:
        raise ValueError("Expected one block-style serial mapping")
    serial = matches[0]
    rates = [value for key, value in serial.value if key.value == "baudrate"]
    if len(rates) > 1: raise ValueError("Duplicate baudrate setting")
    if rates:
        node = rates[0]
        text = text[:node.start_mark.index] + str(baudrate) + text[node.end_mark.index:]
    else:
        if not serial.value: raise ValueError("Expected existing serial settings")
        first = serial.value[0][0].start_mark
        start = first.index - first.column
        newline = "\r\n" if "\r\n" in text else "\n"
        text = text[:start] + " " * first.column + f"baudrate: {baudrate}" + newline + text[start:]
    before, after = yaml.safe_load(raw), yaml.safe_load(text)
    if after["serial"]["baudrate"] != baudrate: raise ValueError("Baudrate edit was not applied")
    after["serial"].pop("baudrate")
    before["serial"].pop("baudrate", None)
    if before != after: raise ValueError("Edit changed another Z2M setting")
    return text.encode("utf-8")

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--saved-config", type=Path, required=True)
    ap.add_argument("--baudrate", type=int, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--apply-to", type=Path)
    args = ap.parse_args()
    original = args.saved_config.read_bytes()
    changed = change_baud(original, args.baudrate)
    if not args.output.resolve().is_relative_to((Path(__file__).resolve().parents[1] / "private").resolve()):
        raise ValueError("Prepared configuration must remain private")
    if args.output.exists():
        if args.output.read_bytes() != changed: raise ValueError("Prepared file differs")
    else:
        with args.output.open("xb") as stream: stream.write(changed)
    applied = False
    if args.apply_to:
        source = args.apply_to
        if source.name != "configuration.yaml": raise ValueError("Expected exact configuration filename")
        current = source.read_bytes()
        if current == changed:
            applied = True
        else:
            if current != original: raise ValueError("Remote config changed since backup; refusing overwrite")
            pending = source.with_name(source.name + ".codex-baud.pending")
            with pending.open("xb") as stream:
                stream.write(changed); stream.flush(); os.fsync(stream.fileno())
            if pending.read_bytes() != changed or source.read_bytes() != original:
                raise ValueError("Configuration changed during staging")
            os.replace(pending, source)
            if source.read_bytes() != changed: raise IOError("Remote configuration verification failed")
            applied = True
    report = dict(passed=True, baudrate=args.baudrate, original_sha256=sha(original),
        prepared_sha256=sha(changed), only_serial_baudrate_changed=True, applied=applied)
    Path(str(args.output) + ".report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))

if __name__ == "__main__": main()
