"""Read the performance counters of the FreeRTOS firmware (DIAG 0x1B), its
mesh counters (DIAG 0x18) and the store status (DIAG 0x19). Read-only: no
flashing, no NV write, no radio command.

  python tools/freertos_perf.py --port COM8 [--baud 1000000]
  python tools/freertos_perf.py --port COM8 --baud 1000000 --interval 60 --samples 1440 \
      --out private/qualif/perf.jsonl        # prolonged qualification: one JSON line per sample

The serial port must be free (Zigbee2MQTT stopped, or the laboratory
herdsman tool not running): DIAG frames share the ZNP UART.
Cycle counts are DWT cycles at 48 MHz, measured on the MCU.
"""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

PERF_NAMES = ["aes_sw_block_cycles", "ccm_sw_frame_cycles", "ccm_hw_frame_cycles", "hw_aes_state",
              "znp_last_cycles", "znp_max_cycles", "rx_last_cycles", "rx_max_cycles", "tick_max_cycles",
              "rx_latency_last_us", "rx_latency_max_us", "rx_measured", "cpu_load_permille",
              "cpu_load_peak_permille", "radio_stack_free_words",
              "host_stack_free_words", "supervisor_stack_free_words", "idle_stack_free_words",
              "timer_stack_free_words_unused",
              "ccm_hw_ops", "ccm_hw_faults", "ccm_hw_mismatches", "ccm_sw_redo", "ccm_busy", "ccm_hw_audits",
              "ccm_hw_audit_failures", "ccm_hw_disabled",
              "aes_timeouts", "aes_dma_errors", "aes_key_errors", "aes_resets", "aes_reset_timeouts",
              "ccm_hw_block_frame_cycles", "ccm_native_ops", "ccm_tag_suspect", "aes_ccm_ops",
              "aes_key_check_failures",
              "gp_tx_started", "gp_tx_ok", "gp_tx_failed", "gp_tx_missed", "gp_coord_tx_deferred",
              "gp_channel_moves", "gp_channel_failed",
              "ccm_prof_setup_cycles_sum", "ccm_prof_aad_cycles_sum", "ccm_prof_data_cycles_sum",
              "ccm_prof_tail_cycles_sum"]
STORE_STATUS = {0: "mounted", 1: "formatted (was blank)", 2: "CORRUPT (nothing erased)", 3: "I/O error",
                4: "bad parameters", 5: "not mounted", 6: "migrated (layout 1 -> 2) at this boot"}
RESTORE_STATUS = {0: "no network", 1: "restored", 2: "network record invalid", 3: "counters missing",
                  4: "counters invalid", 5: "counters of another key", 6: "counters incoherent"}
# Allocated stacks (words), firmware/freertos/app/app.h and FreeRTOSConfig.h.
STACK_WORDS = {"radio": 1280, "host": 512, "supervisor": 256, "idle": 128}


def sample(mt):
    from freertos_lab_session import decode_counters, MESH_NAMES
    perf, _ = mt.request(0x3F, 0x1B)
    mesh, _ = mt.request(0x3F, 0x18)
    store, _ = mt.request(0x3F, 0x19)
    p = decode_counters(perf, PERF_NAMES)
    us = lambda cycles: round(cycles / 48.0, 1)
    report = {
        "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "perf": p,
        "derived_us_at_48MHz": {k.replace("_cycles", "_us"): us(v) for k, v in p.items() if k.endswith("_cycles")},
        "stack_used_percent": {t: round(100 * (1 - p.get(f"{t}_stack_free_words", w) / w), 1)
                               for t, w in STACK_WORDS.items() if f"{t}_stack_free_words" in p},
        "mesh": decode_counters(mesh, MESH_NAMES),
        "store": {"mount": STORE_STATUS.get(store[1], store[1]), "mounted": bool(store[2]),
                  "restore": RESTORE_STATUS.get(store[3], store[3]),
                  "generation": int.from_bytes(store[4:8], "little"),
                  "write_offset": int.from_bytes(store[8:12], "little"),
                  "compactions": int.from_bytes(store[20:24], "little")},
    }
    if store[0] >= 2 and len(store) >= 49:
        report["store"].update({"counter_floor_from_store": bool(store[40]),
                                "recovery_floor_nwk": int.from_bytes(store[41:45], "little"),
                                "recovery_floor_aps": int.from_bytes(store[45:49], "little")})
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--interval", type=float, default=0, help="seconds between samples (0: one sample)")
    ap.add_argument("--samples", type=int, default=1)
    ap.add_argument("--out", type=Path, help="append one JSON line per sample (must be under private/)")
    args = ap.parse_args()
    if args.out and not args.out.resolve().is_relative_to((ROOT / "private").resolve()):
        ap.error("--out must be under private/ (not versioned)")
    from run_dongle_session import wait_mt
    mt, _ = wait_mt(args.port, timeout=8, baud=args.baud)
    try:
        for n in range(max(1, args.samples)):
            report = sample(mt)
            if args.out:
                args.out.parent.mkdir(parents=True, exist_ok=True)
                with args.out.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(report) + "\n")
            print(json.dumps(report, indent=None if args.out else 2), flush=True)
            if n + 1 < args.samples:
                time.sleep(args.interval)
    finally:
        mt.close()


if __name__ == "__main__":
    main()
