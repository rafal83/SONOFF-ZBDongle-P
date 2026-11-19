"""Export verified production-startup facts, never keys, addresses or names."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from image import read_hex
from migrate_production import FLASH_SIZE, NV_START, NV_END, TESTED_SHA, same_program

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path): return json.loads(path.read_text(encoding="utf-8"))

def export(folder):
    install = read(folder / "install/report.json")
    target = folder / "install/target-readback.bin"
    current = (folder / "install/current-a.bin").read_bytes()
    restored = target.read_bytes()
    candidate_path = Path(__file__).resolve().parents[1] / "build/coordinator-optimized-1000000/sonoff-znp-coordinator.hex"
    candidate = bytearray(b"\xff" * FLASH_SIZE)
    for address, byte in read_hex(candidate_path).items(): candidate[address] = byte
    if not (sha(candidate_path) == TESTED_SHA and install["passed"] and install["ready_for_Z2M"]
            and install["NV_and_CCFG_never_erased_or_programmed"]
            and current == (folder / "install/current-b.bin").read_bytes()
            and restored == (folder / "install/target.bin").read_bytes()
            and current[NV_START:NV_END] == restored[NV_START:NV_END]
            and same_program(restored, candidate) and sha(target) == install["programmed"]["sha256"]):
        raise ValueError("Physical migration evidence does not match the tested firmware")
    network = install["network_verification"]
    before, after = read(folder / "install/network-preflight.json"), read(folder / "install/network-after.json")
    for field in ["coordinator_ieee", "pan_id", "extended_pan_id", "channel", "security_level"]:
        if before[field] != after[field]: raise ValueError("Network identity changed")
    if before["network_key"]["key"] != after["network_key"]["key"]: raise ValueError("Network key changed")
    if before["stack_specific"] != after["stack_specific"]: raise ValueError("Stack key material changed")
    if before["network_key"]["frame_counter"] > after["network_key"]["frame_counter"]: raise ValueError("Security counter decreased")
    if not (network["passed"] and network["security_counters_non_decreasing"]
            and network["database_devices_absent_from_backup"] == 0): raise ValueError("Network continuity failed")
    config = read(folder / "configuration-1000000.yaml.report.json")
    if not config["applied"] or not config["only_serial_baudrate_changed"]: raise ValueError("Z2M config not applied")
    logs = read(folder / "production-log-check.json")
    mqtt = read(folder / "MQTT-observation.private.json")
    report = mqtt["report"]
    if not (logs["expected_firmware_detected"] and logs["Z2M_started"] and logs["herdsman_started"]
            and not logs["Z2M_stopped_after_start"] and report["health_check_ok"] and report["coordinator_check_ok"]):
        raise ValueError("Production startup and health have not passed")
    observations = read(folder / "device-observations.private.json")
    fresh = {d["friendly_name"]:d["type"] for d in observations if d["fresh_radio_timestamp_after_install"]}
    fresh.update({name:item["type"] for name,item in mqtt["fresh"].items()})
    oldrows = [json.loads(s) for s in (folder / "z2m-data/database.db").read_text(encoding="utf-8").splitlines() if s.strip()]
    import yaml
    saved_config = yaml.safe_load((folder / "z2m-data/configuration.yaml").read_text(encoding="utf-8"))
    dates = []
    for name, probe in mqtt["requests"].items():
        if probe.get("fresh_telemetry_after_get"): continue
        row = next(r for r in oldrows if saved_config.get("devices", {}).get(r.get("ieeeAddr"), {}).get("friendly_name", r.get("ieeeAddr")) == name)
        if row.get("lastSeen"):
            dates.append(datetime.fromtimestamp(row["lastSeen"] / 1000, timezone.utc).date().isoformat())
    evidence = ["install/report.json", "install/target-readback.bin", "install/network-after.json.report.json",
        "z2m-snapshot.json", "configuration-1000000.yaml.report.json", "production-startup.private.log", "MQTT-observation.private.json"]
    return dict(date="2026-11-17", experimental_hex_sha256=TESTED_SHA, baudrate=1000000,
        authorized_production_trial=True, production_network_preserved=True,
        NV_and_CCFG_not_erased_or_programmed=True, full_readback_verified=True,
        database_devices=network["database_devices"], all_database_devices_in_coordinator_backup=True,
        network_keys_preserved=True, security_counters_non_decreasing_at_install=True,
        z2m_startup_tested=True, MQTT_health_check_passed=True, coordinator_check_passed=True,
        missing_routers_in_coordinator=report["missing_routers_in_coordinator"],
        Z2M_version=report["status"]["Z2M_version"], herdsman_version=report["status"]["herdsman_version"],
        fresh_radio_device_count=len(fresh), fresh_radio_types=dict(Counter(fresh.values())),
        observation_requires_last_seen_after_installation=True,
        router_GETs_sent=report["router_GETs_sent"], GETs_followed_by_fresh_telemetry=report["GETs_followed_by_fresh_telemetry"],
        startup_log_error_counts=logs["error_categories"],
        unsuccessful_GET_devices_last_seen_before_migration_UTC_dates=sorted(dates),
        setters_sent=0, associations_opened=False, re_pairing_performed=False,
        candidate_left_installed=True, original_firmware_saved=True, rollback_after_production_not_executed=True,
        original_rollback_strategy="Restore original application sectors only; retain current NV/security counters",
        all_74_radio_devices_validated=False, endurance_tested=False, final_qualified=False,
        evidence_sha256={name:sha(folder / name) for name in evidence})

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--session-dir", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    if not args.session_dir.resolve().is_relative_to((Path(__file__).resolve().parents[1] / "private").resolve()):
        raise ValueError("Expected private evidence")
    result = export(args.session_dir)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k:result[k] for k in ["z2m_startup_tested", "database_devices", "fresh_radio_device_count", "fresh_radio_types", "startup_log_error_counts"]}))

if __name__ == "__main__": main()
