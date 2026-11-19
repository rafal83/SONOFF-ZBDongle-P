"""Summarize explicitly supplied laboratory CSV; never opens serial/network interfaces."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics

def summarize(rows, duration_seconds):
    if not math.isfinite(duration_seconds) or duration_seconds <= 0:
        raise ValueError("A positive finite observation duration is required")
    latencies, total, failed = [], 0, 0
    for row in rows:
        total += 1
        if row["status"] not in ("ok", "error", "timeout"):
            raise ValueError("status must be ok, error or timeout")
        if row["status"] != "ok":
            failed += 1
            continue
        latency = float(row["latency_ms"])
        if not math.isfinite(latency) or latency < 0:
            raise ValueError("Successful transaction requires finite non-negative latency_ms")
        latencies.append(latency)
    latencies.sort()
    def percentile(p):
        return latencies[max(0, math.ceil(p * len(latencies)) - 1)] if latencies else None
    return {"transactions": total, "successful": len(latencies), "failed": failed,
            "error_rate": failed / total if total else None,
            "success_per_second": len(latencies) / duration_seconds,
            "latency_ms": {"median": statistics.median(latencies) if latencies else None,
                           "p95_nearest_rank": percentile(.95), "p99_nearest_rank": percentile(.99),
                           "max": max(latencies) if latencies else None},
            "duration_seconds": duration_seconds}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv", type=Path)
    ap.add_argument("--duration-seconds", type=float, required=True)
    ap.add_argument("--label", required=True, help="Firmware and scenario identifier; no keys")
    args = ap.parse_args()
    with args.csv.open(newline="", encoding="utf-8") as stream:
        result = summarize(csv.DictReader(stream), args.duration_seconds)
    result["label"] = args.label
    print(json.dumps(result, indent=2, allow_nan=False))

if __name__ == "__main__":
    main()
