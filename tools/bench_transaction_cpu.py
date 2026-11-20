"""CPU cost of each step of a host transaction on OUR compiled coordinator
code (Cortex-M4F instructions in Unicorn), CCM* excluded, for the network
benchmark firmware/freertos/sim/net_bench.c (which adds the CCM* operations
it counts, at the cost measured on the dongle).

Steps measured (firmware/freertos/tests/arm_perf.c):
  af_request   ZNP AF_DATA_REQUEST (ZCL read) -> APS, NWK, seal, MAC queue, radio
  tx_confirm   MAC confirm -> route outcome -> ZNP AF_DATA_CONFIRM
  rx_unicast   secured unicast received -> MAC, NWK open, APS -> AF_INCOMING_MSG
  rx_bcast_dup relayed copy of a broadcast, dropped before decryption
  poll         MAC data request of a sleepy child -> indirect frame to the radio
  tick         radio task tick with nothing due

    python tools/bench_transaction_cpu.py <arm-test ELF> <out.json>
"""
import hashlib
import json
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_arm_freertos import Arm  # noqa: E402
from bench_freertos import CPI, MHZ, CALIBRATION  # noqa: E402


def med(values):
    return int(statistics.median(values))


def main(elf_path, out_path):
    arm = Arm(elf_path)
    arm.call("arm_perf_setup")
    steps = {}

    def measure(null):
        arm.call("arm_perf_null_ccm", 1 if null else 0)
        af, conf, rx, tick, poll = [], [], [], [], []
        for _ in range(16):
            status, c = arm.call("arm_perf_znp_af")
            af.append(c)
            out, c = arm.call("arm_perf_tx_done_confirm")
            assert out >= 1, out
            conf.append(c)
            assert arm.call("arm_perf_build_rx")[0] > 0
            out, c = arm.call("arm_perf_rx_znp")
            rx.append(c)
            tick.append(arm.call("arm_perf_tick_idle")[1])
            assert arm.call("arm_perf_poll_setup")[0] == 1
            sent, c = arm.call("arm_perf_poll")
            assert sent == 1, sent
            poll.append(c)
            arm.call("arm_perf_tx_done")
        return {"af_request": med(af), "tx_confirm": med(conf), "rx_unicast": med(rx),
                "tick": med(tick), "poll": med(poll)}

    without = measure(True)
    with_sw = measure(False)
    arm.call("arm_perf_null_ccm", 0)
    # Broadcast echo (no CCM*: recognised before decryption), as bench_freertos.
    assert arm.call("arm_perf_add_routers", 8)[0] == 8
    echoes = []
    for _ in range(4):
        assert arm.call("arm_perf_broadcast")[0] > 0
        arm.call("arm_perf_tx_done")
        for router in range(1, 9):
            assert arm.call("arm_perf_build_echo", router)[0] > 16
            d, c = arm.call("arm_perf_echo")
            assert d == 1, d
            echoes.append(c)
    without["rx_bcast_dup"] = med(echoes)
    for k, v in without.items():
        steps[k] = {"instructions_without_ccm": v, "cycles_estimated": round(v * CPI),
                    "us_estimated": round(v * CPI / MHZ, 1)}
        if k in with_sw:
            steps[k]["instructions_with_sw_ccm"] = with_sw[k]
    report = {
        "level": "our code only: ARM Cortex-M4 instructions in Unicorn, CCM* replaced by a pass-through",
        "elf_sha256": hashlib.sha256(Path(elf_path).read_bytes()).hexdigest(),
        "cycles_per_instruction": round(CPI, 4), "calibration": CALIBRATION, "clock_mhz": MHZ,
        "steps": steps,
    }
    Path(out_path).write_text(json.dumps(report, indent=2) + "\n")
    for k, v in steps.items():
        print(k, json.dumps(v))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
