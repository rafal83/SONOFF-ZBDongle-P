"""Read Z2M shared logs; publish only secret-free startup and error counters."""
import argparse
from collections import Counter
from datetime import datetime
import json
from pathlib import Path
import re
import yaml

def last_seen(value):
    try:
        if isinstance(value, (float, int)): return value / 1000 if value > 1e11 else value
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except (ValueError, TypeError, AttributeError): return None

def firmware_revisions(lines):
    firmware=[line for line in lines if 'Coordinator firmware version' in line]
    # Firmware revisions are uint32 values; newer candidates have ten digits.
    return re.findall(r'''["']revision["']\s*:\s*(\d+)''', ' '.join(firmware))

def inspect(source, session, expected_revision=20261114, output_tag=''):
    if output_tag and not output_tag.replace('-', '').isalnum(): raise ValueError('Invalid output tag')
    suffix = '-' + output_tag if output_tag else ''
    logs = list((source / "log").rglob("*.log"))
    if not logs: raise ValueError("No Z2M log file found")
    newest = max(logs, key=lambda p: p.stat().st_mtime)
    raw = newest.read_bytes()
    (session / ("production-startup" + suffix + ".private.log")).write_bytes(raw)
    lines = raw.decode("utf-8", errors="replace").splitlines()
    revisions = firmware_revisions(lines)
    joined = re.findall(r"Currently (\d+) devices are joined", "\n".join(lines))
    errors = [line for line in lines if re.search(r"\berror\s*:", line, re.I)]
    categories = Counter()
    for line in errors:
        lower = line.lower()
        if 'mem_error' in lower: categories['MEM_ERROR'] += 1
        elif "nwk_no_route" in lower: categories["NWK_NO_ROUTE"] += 1
        elif "timeout -" in lower or "timed out" in lower: categories["timeout"] += 1
        elif "configuration" in lower or "mismatch" in lower: categories["configuration"] += 1
        elif "mqtt" in lower: categories["MQTT"] += 1
        elif "serial" in lower or "adapter" in lower or "herdsman" in lower: categories["adapter_or_serial"] += 1
        else: categories["other"] += 1
    result = dict(log_file=newest.name, log_bytes=len(raw), firmware_revisions=revisions,
        expected_revision=expected_revision, expected_firmware_detected=str(expected_revision) in revisions,
        Z2M_started=any("Zigbee2MQTT started!" in line for line in lines),
        herdsman_started=any("zigbee-herdsman started" in line for line in lines),
        joined_device_count=int(joined[-1]) if joined else None,
        error_lines=len(errors), error_categories=dict(categories),
        network_recovery_error=any("configuration is not consistent" in line.lower() for line in lines),
        Z2M_stopped_after_start=any("Stopped Zigbee2MQTT" in line for line in lines))
    old = json.loads((session / "z2m-data/state.json").read_text(encoding="utf-8"))
    current = json.loads((source / "state.json").read_text(encoding="utf-8"))
    config = yaml.safe_load((session / "z2m-data/configuration.yaml").read_text(encoding="utf-8"))
    base = config.get("mqtt", {}).get("base_topic", "zigbee2mqtt") + "/"
    observed = {}
    for line in lines:
        match = re.search(r"MQTT publish: topic '([^']+)', payload '(.*)'\s*$", line)
        if not match or not match[1].startswith(base): continue
        try: payload = json.loads(match[2])
        except ValueError: continue
        if isinstance(payload, dict) and "last_seen" in payload:
            observed[match[1][len(base):]] = payload
    # Cached startup publications may be newer than state.json yet months old.
    # Count a device only if its radio timestamp is after this installation.
    migration_report = session / "install/report.json"
    if not migration_report.is_file(): migration_report = session / "upgrade/report.json"
    boundary = migration_report.stat().st_mtime
    devices = [json.loads(line) for line in (session / "z2m-data/database.db").read_text(encoding="utf-8").splitlines() if line.strip()]
    persisted_seen = 0
    observed_types = Counter()
    remaining_types = Counter()
    device_results = []
    for device in devices:
        if device.get("type") in ("Coordinator", "Group"): continue
        ieee = device["ieeeAddr"]
        name = config.get("devices", {}).get(ieee, {}).get("friendly_name", ieee)
        before = last_seen(old.get(name, old.get(ieee, {})).get("last_seen"))
        after = last_seen(current.get(name, current.get(ieee, {})).get("last_seen"))
        if after is not None and after >= boundary and (before is None or after > before): persisted_seen += 1
        event_time = last_seen(observed.get(name, {}).get("last_seen"))
        latest = max([v for v in [after, event_time] if v is not None], default=0)
        fresh = latest >= boundary and (before is None or latest > before)
        (observed_types if fresh else remaining_types)[device.get("type")] += 1
        device_results.append(dict(ieee=ieee, friendly_name=name, type=device.get("type"),
                                   fresh_radio_timestamp_after_install=fresh))
    (session / ("device-observations" + suffix + ".private.json")).write_text(json.dumps(device_results, indent=2) + "\n", encoding="utf-8")
    result["devices_with_advanced_persisted_last_seen"] = persisted_seen
    result["devices_observed_after_installation"] = sum(observed_types.values())
    result["observed_types"] = dict(observed_types)
    result["awaiting_fresh_observation_types"] = dict(remaining_types)
    (session / ("production-log-check" + suffix + ".json")).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, type=Path)
    ap.add_argument("--session-dir", required=True, type=Path)
    ap.add_argument("--expected-revision", type=int, choices=[20261114, 20261117, 2026111801, 2026111802], default=20261114)
    ap.add_argument('--output-tag', default='')
    args = ap.parse_args()
    if not args.session_dir.resolve().is_relative_to((Path(__file__).resolve().parents[1] / "private").resolve()):
        raise ValueError("Results must stay private")
    print(json.dumps(inspect(args.source, args.session_dir, args.expected_revision, args.output_tag)))

if __name__ == "__main__": main()
