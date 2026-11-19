"""Recompute measured performance from raw transaction CSV, export no secrets."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
from benchmark import summarize

def radio_comparison(control_path, fast_path, profiles):
    """Compare completed lab peer trials; do not label this an original RF test."""
    trials = []
    for path, baudrate in [(control_path, 115200), (fast_path, 1000000)]:
        proof = json.loads(path.read_text(encoding="utf-8"))
        radio = proof["radio"]
        matching = [p for p in profiles if p["label"] == f"candidate_{baudrate}"
                    and p["hex_sha256"] == proof["experimental_hex_sha256"]]
        if not (len(matching) == 1 and proof["baudrate"] == baudrate
                and proof["restore_verified"] and proof["original_responded_after_restore"]
                and radio["passed"] and radio["radio_and_NV_load_test"]
                and radio["radio_exchange_after_coordinator_reset"]
                and radio["radio_exchange_after_backup_restore"]
                and not proof["Z2M_used"] and not proof["production_network_loaded_into_experimental"]
                and radio["load_test_changed_NV_writes"] == 80 and radio["load_test_radio_reads"] == 40):
            raise ValueError("Radio/load evidence does not match the measured candidate")
        phases = {}
        for phase, count in [("remote_repeated_attribute_reads", 30), ("radio_and_NV_load_test", 40)]:
            values = sorted(x["latency_ms"] for x in radio["application_read_latencies"] if x["phase"] == phase)
            if len(values) != count or any(not math.isfinite(v) or v <= 0 for v in values):
                raise ValueError("Incomplete or invalid radio latency observations")
            phases[phase] = dict(successful_reads=count, median_ms=statistics.median(values),
                p95_ms=values[math.ceil(.95 * count) - 1], max_ms=max(values))
        trials.append(dict(baudrate=baudrate, hex_sha256=matching[0]["hex_sha256"],
            proof_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            peer_model=radio["hardware_peer_model"], changed_NV_writes=80, phases=phases))
    if trials[0]["peer_model"] != trials[1]["peer_model"]:
        raise ValueError("Radio peer models differ")
    return dict(comparison="Candidate 115200 versus candidate 1000000, one HG06106C; not original firmware RF",
        trials=trials, original_firmware_RF_benchmarked=False, Z2M_service_tested=False,
        median_speedup={phase: trials[0]["phases"][phase]["median_ms"] / trials[1]["phases"][phase]["median_ms"]
                        for phase in trials[0]["phases"]},
        limits="Two sequential trials on one peer; host/USB and radio included; no mesh or endurance claim")

def verified_profile(profile, backup_dir):
    if not profile["passed"] or not profile["restore_verified"]:
        raise ValueError("A failed profile cannot support a performance claim")
    measurement = profile["measurements"]
    path = backup_dir / profile["rollback_session"] / "performance-raw.csv"
    with path.open(newline="", encoding="utf-8") as stream: rows = list(csv.DictReader(stream))
    cases = {"ping", "read_16", "read_64", "read_128", "read_240", "identical_write_240"}
    expected = {(case, str(r), str(i)) for case in cases for r in range(measurement["rounds"])
                for i in range(measurement["samples_per_round"])}
    actual = [(row["case"], row["round"], row["iteration"]) for row in rows]
    if len(actual) != len(set(actual)) or set(actual) != expected:
        raise ValueError("Raw CSV has missing, duplicate or unexpected observations")
    aggregate = {}
    for case in sorted(cases):
        selected = [row for row in rows if row["case"] == case]
        duration = sum(block["duration_seconds"] for block in measurement["cases"][case])
        aggregate[case] = summarize(selected, duration)
        if aggregate[case]["failed"] or aggregate[case] != measurement["aggregate"][case]:
            raise ValueError("Raw observations disagree with the claimed successful summary")
    return dict(label=profile["label"], revision=measurement["revision"], baudrate=profile["baudrate"],
        hex_sha256=profile["hex_sha256"], passed=True, original_restoration_verified=True,
        scratch_sha256=measurement["scratch_sha256"], raw_csv_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        measured_transactions=len(rows), cases=aggregate)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--matrix", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--radio-control", type=Path)
    ap.add_argument("--radio-fast", type=Path)
    args = ap.parse_args()
    if bool(args.radio_control) != bool(args.radio_fast):
        ap.error("Both completed radio trials are required")
    matrix = json.loads(args.matrix.read_text())
    required = ["original_115200_before", "candidate_115200", "candidate_460800", "candidate_1000000", "original_115200_after"]
    if [p["label"] for p in matrix["profiles"]] != required: raise ValueError("Full bracketed comparison required")
    profiles = [verified_profile(p, args.matrix.parent) for p in matrix["profiles"]]
    if len({p["scratch_sha256"] for p in profiles}) != 1: raise ValueError("Workload payloads differ")
    before, after = profiles[0], profiles[-1]
    metric = lambda profile, case, field="median": profile["cases"][case]["latency_ms"][field]
    drift = abs(metric(before, "read_240") - metric(after, "read_240")) / min(metric(before, "read_240"), metric(after, "read_240"))
    # Use the faster of the two original medians as the conservative reference.
    baseline_read = min(metric(before, "read_240"), metric(after, "read_240"))
    baseline_write = min(metric(before, "identical_write_240"), metric(after, "identical_write_240"))
    baseline_p99 = min(metric(before, "identical_write_240", "p99_nearest_rank"), metric(after, "identical_write_240", "p99_nearest_rank"))
    gains = []
    for p in profiles[1:-1]:
        gains.append(dict(label=p["label"], baudrate=p["baudrate"],
            read_240_median_speedup=baseline_read / metric(p, "read_240"),
            identical_write_240_median_speedup=baseline_write / metric(p, "identical_write_240"),
            identical_write_240_p99_speedup=baseline_p99 / metric(p, "identical_write_240", "p99_nearest_rank")))
    eligible = [g for g in gains if g["read_240_median_speedup"] >= 1.2]
    selected = max(eligible, key=lambda g: g["read_240_median_speedup"]) if eligible and drift < .1 else None
    result = dict(date="2026-11-17", hardware_tested=True, original_revision=20240710,
        method=matrix["method"], radio_benchmark_tested=False, Z2M_service_tested=False,
        production_NV_used=False, warmup_excluded_per_case_per_round=5,
        transactions=sum(p["measured_transactions"] for p in profiles), profiles=profiles,
        original_read_240_bracket_drift_fraction=drift, gains=gains,
        transport_gain_established=selected is not None,
        selected_transport_baudrate=selected["baudrate"] if selected else None,
        final_qualified=False,
        limits=["Standard SYS/NV workload on fresh synthetic laboratory NV",
                "Host USB/serial latency included; not a CPU or RF bitrate benchmark",
                "No claim that all Zigbee application commands have the same speedup",
                "One PC and one dongle; representative mesh/endurance remain untested"])
    if args.radio_control:
        result["radio_load_comparison"] = radio_comparison(args.radio_control, args.radio_fast, profiles)
        result["radio_benchmark_tested"] = True
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ["transactions", "original_read_240_bracket_drift_fraction", "gains", "selected_transport_baudrate"]}, indent=2))

if __name__ == "__main__": main()
