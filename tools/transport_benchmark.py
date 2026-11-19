"""Real MT benchmark on freshly erased LAB NV only, under rollback supervision.

Uses standard SYS commands shared by the original and candidate. Never reads
production items, commissions a network, or emits synthetic successful replies.
"""
import csv
import hashlib
import random
import struct
import time
from benchmark import summarize
from dongle_probe import MT

def measure(port, baud, expected_revision, expected_ieee, output, samples=40, rounds=3):
    if samples < 10 or rounds < 2: raise ValueError("Benchmark requires >=10 samples and >=2 rounds")
    mt = MT(port, baud)
    rows = []
    report = {"passed": False, "baudrate": baud, "revision": expected_revision,
              "samples_per_round": samples, "rounds": rounds, "radio_tested": False,
              "production_NV_used": False, "serial_flow_control": False, "cases": {}}
    try:
        version, _ = mt.request(0x21, 2)
        ieee, _ = mt.request(0x21, 4)
        if int.from_bytes(version[5:9], "little") != expected_revision or int.from_bytes(ieee, "little") != int(expected_ieee, 16):
            raise ValueError("Benchmark target identity/revision mismatch")
        capability, _ = mt.request(0x21, 1)
        key = struct.pack("<BHH", 13, 43, 1)
        data = bytes((i * 73 + 19) & 255 for i in range(240))
        created, _ = mt.request(0x21, 0x30, key + struct.pack("<I", len(data)))
        if created != b"\x00": raise RuntimeError("Scratch item must be new on blank experimental NV")
        written, _ = mt.request(0x21, 0x34, key + struct.pack("<HB", 0, len(data)) + data)
        if written != b"\x00": raise RuntimeError("Scratch initialization failed")
        report["scratch_bytes"] = len(data)
        report["scratch_sha256"] = hashlib.sha256(data).hexdigest()
        cases = {"ping": (1, b"", capability, 0)}
        for length in (16, 64, 128, 240):
            cases[f"read_{length}"] = (0x33, key + struct.pack("<HB", 0, length), bytes([0, length]) + data[:length], length)
        cases["identical_write_240"] = (0x34, key + struct.pack("<HB", 0, len(data)) + data, b"\x00", len(data))
        randomizer = random.Random(20261117)  # Same order for every firmware.
        for round_index in range(rounds):
            order = list(cases); randomizer.shuffle(order)
            for name in order:
                command, payload, expected, useful_bytes = cases[name]
                # Explicit warm-up, excluded identically in all profiles.
                for _ in range(5):
                    response, _ = mt.request(0x21, command, payload)
                    if response != expected: raise RuntimeError("Benchmark warm-up response mismatch")
                block = []
                start = time.perf_counter()
                for iteration in range(samples):
                    transaction_start = time.perf_counter()
                    try:
                        response, _ = mt.request(0x21, command, payload)
                        if response != expected: raise RuntimeError("Benchmark response mismatch")
                        status = "ok"
                    except Exception:
                        # Keep the failed observation. Stop rather than allow
                        # a late SRSP to be attributed to the next transaction.
                        elapsed = (time.perf_counter() - transaction_start) * 1000
                        rows.append(dict(case=name, round=round_index, iteration=iteration, status="error", latency_ms=elapsed))
                        raise
                    elapsed = (time.perf_counter() - transaction_start) * 1000
                    row = dict(case=name, round=round_index, iteration=iteration, status=status, latency_ms=elapsed)
                    rows.append(row); block.append(row)
                duration = time.perf_counter() - start
                summary = summarize(block, duration)
                summary["payload_bytes_per_second"] = summary["success_per_second"] * useful_bytes
                report["cases"].setdefault(name, []).append(summary)
                print(f"Benchmark {baud}: round {round_index + 1}/{rounds}, {name}: "
                      f"p50={summary['latency_ms']['median']:.3f} ms, errors={summary['failed']}", flush=True)
        for name in cases:
            chosen = [row for row in rows if row["case"] == name]
            duration = sum(block["duration_seconds"] for block in report["cases"][name])
            report.setdefault("aggregate", {})[name] = summarize(chosen, duration)
        report["passed"] = True
        return report
    finally:
        mt.close()
        with output.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=["case", "round", "iteration", "status", "latency_ms"])
            writer.writeheader(); writer.writerows(rows)
