"""Workloads of SMLIGHT's published ZeroStack benchmarks, run on OUR compiled
coordinator code (Cortex-M4F instructions in Unicorn, no peripheral, no RTOS).

Only our firmware is measured here. ZeroStack and Z-Stack figures quoted in
docs/firmware/benchmarks.md are the vendor's own, from its documentation; they
were not reproduced. Instruction counts are converted to estimated cycles with
the ratio measured on the dongle for the same code (DWT, see CALIBRATION).

    python tools/bench_freertos.py build/freertos/arm-test/freertos-portable-test-only.elf out.json
"""
import hashlib
import json
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_arm_freertos import Arm, ROOT  # noqa: E402

# Same code measured on the dongle (DWT cycles) and in Unicorn (instructions):
# software AES block 6 870 cycles / 4 693 instr, CCM* 22 B AAD + 40 B
# 74 769 cycles / 51 064 instr (docs/firmware/status.md).
CALIBRATION = {"aes_block": (6870, 4693), "ccm_frame": (74769, 51064)}
CPI = statistics.mean(c / i for c, i in CALIBRATION.values())
MHZ = 48


def summary(values):
    values = sorted(values)
    p95 = values[min(len(values) - 1, int(round(0.95 * (len(values) - 1))))]
    med = int(statistics.median(values))
    return {"n": len(values), "median_instructions": med, "p95_instructions": p95,
            "max_instructions": values[-1], "median_cycles_estimated": round(med * CPI),
            "median_us_estimated": round(med * CPI / MHZ, 1)}


def main(elf_path, out_path):
    arm = Arm(elf_path)
    arm.call("arm_perf_setup")
    r = {}

    # 1. Secured unicast received (NWK security + APS), table empty then full.
    def rx_round(n):
        counts = []
        for _ in range(n):
            assert arm.call("arm_perf_build_rx")[0] > 0
            _, c = arm.call("arm_perf_rx")
            counts.append(c)
        return counts
    assert arm.call("arm_perf_add_routers", 50)[0] == 50
    r["rx_secured_frame_51_devices_sw_aes"] = summary(rx_round(20))
    devices, _ = arm.call("arm_perf_fill_devices")
    assert devices == 200, devices
    r["rx_secured_frame_200_devices_sw_aes"] = summary(rx_round(20))

    # 2. Lookup of an unknown device in the full table.
    misses = []
    for salt in range(100):
        slot, c = arm.call("arm_perf_lookup_miss", salt * 7919 + 1)
        assert slot == 0xFFFF
        misses.append(c)
    r["device_lookup_miss_200_devices"] = summary(misses)

    # 3. Broadcast storm: 20 broadcasts x 50 routers = 1 000 echoes heard back.
    echoes, outcome = [], {}
    for _ in range(20):
        assert arm.call("arm_perf_broadcast")[0] > 0
        arm.call("arm_perf_tx_done")
        for router in range(1, 51):
            assert arm.call("arm_perf_build_echo", router)[0] > 16
            d, c = arm.call("arm_perf_echo")
            name = {1: "duplicate", 2: "auth_failure", 4: "malformed", 8: "delivered"}.get(d, f"other_{d}")
            outcome[name] = outcome.get(name, 0) + 1
            echoes.append(c)
    assert "delivered" not in outcome, outcome
    r["broadcast_echo_50_routers_x20"] = summary(echoes) | {
        "total_instructions": sum(echoes),
        "total_ms_estimated": round(sum(echoes) * CPI / MHZ / 1000, 2),
        "outcome": outcome}

    # 4. Replay, tampered MIC, truncated security header.
    replay, tamper, trunc = [], [], []
    for _ in range(50):
        arm.call("arm_perf_build_rx"); arm.call("arm_perf_rx")
        ok, c = arm.call("arm_perf_replay"); assert ok == 1; replay.append(c)
        arm.call("arm_perf_build_rx")
        ok, c = arm.call("arm_perf_tamper"); assert ok == 1; tamper.append(c)
        arm.call("arm_perf_build_rx")
        ok, c = arm.call("arm_perf_truncated", 9 + 8 + 6); assert ok == 1; trunc.append(c)
    r["replayed_frame_rejected"] = summary(replay)
    r["tampered_mic_rejected"] = summary(tamper)
    r["truncated_aux_header_rejected"] = summary(trunc)

    # 5. Store: identical 100 B record written 48 times; then changing
    #    100 B writes until the first compaction (empty store, then with a
    #    live set like a production network: 200 device records of 48 B).
    def sym(name):
        return int.from_bytes(arm.read(arm.sym[name], 4), "little")
    assert arm.call("arm_nv_setup")[0] == 1
    assert arm.call("arm_nv_write", 7, 100, 1)[0] == 1
    programmed = sym("arm_nv_programmed")
    same = [arm.call("arm_nv_write", 7, 100, 1)[1] for _ in range(48)]
    r["store_identical_write_100B_x48"] = summary(same) | {
        "flash_bytes_programmed": sym("arm_nv_programmed") - programmed}

    def until_compaction(prefill):
        assert arm.call("arm_nv_setup")[0] == 1
        for i in range(prefill):
            assert arm.call("arm_nv_write", 100 + i, 48, i)[0] == 1
        base_c, base_e = arm.call("arm_nv_compactions")[0], sym("arm_nv_erases")
        writes, last = 0, 0
        while arm.call("arm_nv_compactions")[0] == base_c:
            ok, last = arm.call("arm_nv_write", 7, 100, writes + 2)
            assert ok == 1
            writes += 1
        return {"writes_until_first_compaction": writes, "compacting_write_instructions": last,
                "compacting_write_ms_estimated_cpu_only": round(last * CPI / MHZ / 1000, 2),
                "sector_erases_in_compaction": sym("arm_nv_erases") - base_e}
    r["store_compaction_empty"] = until_compaction(0)
    r["store_compaction_200_live_records"] = until_compaction(200)

    report = {
        "level": "our code only: ARM Cortex-M4 instructions in Unicorn 2.1.3, no peripherals/RTOS/flash timing",
        "elf_sha256": hashlib.sha256(Path(elf_path).read_bytes()).hexdigest(),
        "cycles_per_instruction_calibration": {"ratio": round(CPI, 4), "from": CALIBRATION, "clock_mhz": MHZ},
        "aes": "software AES in this run; hardware CCM* measured on the dongle: 21 609 cycles per 40 B frame",
        "results": r,
        "hardware_tested": False,
        "other_firmwares_measured": False,
    }
    Path(out_path).write_text(json.dumps(report, indent=2) + "\n")
    for k, v in r.items():
        print(k, json.dumps(v))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
