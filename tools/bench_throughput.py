"""Sustained throughput and sleepy-device benchmark of the FreeRTOS coordinator:
SIMULATION (firmware/freertos/sim/net_bench.c), never a hardware measurement.

The real portable stack (ZNP, zb_coord, MAC, NWK security) runs against a
timed 802.15.4 channel, simulated routers and sleepy end devices, and a
zigbee-herdsman host model. Nothing is sent on a serial port or on the air.

  python tools/bench_throughput.py [--quick] [--out build/freertos/throughput]
      [--costs build/freertos/arm-test/transaction-cpu.json] [--exe <net_bench>]

Suites (each run is repeated with --seeds seeds, the median is reported):
  levels   25, 50, 100, 150, 180 transactions/s, production-like tree
           (28 routers: 16 one hop, 9 two hops, 3 three hops), one collision
           domain, 1 % random frame loss;
  max      highest stable rate (success >= 99.9 %, p95 end-to-end <= 1 s):
           production-like tree, one-hop only, with spatial reuse;
  reports  incoming unsolicited attribute reports (most of a Zigbee2MQTT
           network's traffic, and what "messages per second" usually means):
           50 to 250 reports/s, tree and one-hop; Flash compaction stalls
           (CPU blocked 76 ms) with 4 and 8 RF receive buffers;
  sleepy   10, 20, 40 sleepy end devices (children of the coordinator and of
           routers), spread or simultaneous wake-ups, a congested router,
           a parent lost during the run, a child moving to
           a router without a rejoin we hear (herdsman's assocRemove recovery);
  bursts   tools/znp_prod_bench.cjs on the simulator: one read to each of the
           28 routers at once every 12 s (no 16-transaction window), 120 s,
           with no faulty relay, then with a relay losing 30 % of what it
           relays (a router that "does not always react"): first-attempt
           failures are what the production bench counts.
"""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

LEVELS = [25, 50, 100, 150, 180]


def build(exe_path):
    import build_freertos as b
    zig, _ = b.zig()
    flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-fsanitize=undefined", "-fsanitize-trap=undefined"] + [f"-I{p}" for p in b.INCLUDES]
    exe_path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([zig, "cc", *flags, *b.PORTABLE, "firmware/freertos/sim/net_bench.c", "-o", str(exe_path)],
                   cwd=ROOT, check=True)


def cost_args(path):
    if not path or not Path(path).exists():
        return []
    steps = json.loads(Path(path).read_text())["steps"]
    m = {"af_request": "--i-af", "tx_confirm": "--i-confirm", "rx_unicast": "--i-rx", "rx_bcast_dup": "--i-dup",
         "poll": "--i-poll", "tick": "--i-tick"}
    args = []
    for k, opt in m.items():
        if k in steps:
            args += [opt, str(steps[k]["instructions_without_ccm"])]
    return args


class Runner:
    def __init__(self, exe, out, seeds, costs, duration):
        self.exe, self.out, self.seeds, self.costs, self.duration = exe, out, seeds, costs, duration
        self.n = 0

    def run(self, label, **opts):
        results = []
        opts = dict(opts)
        duration = opts.pop("duration", self.duration)
        for seed in range(1, self.seeds + 1):
            self.n += 1
            js = self.out / "runs" / f"{label}-s{seed}.json"
            js.parent.mkdir(parents=True, exist_ok=True)
            args = [str(self.exe), "--seed", str(seed), "--label", label, "--json", str(js), "--quiet", "1",
                    "--duration", str(duration)] + self.costs
            for k, v in opts.items():
                args += ["--" + k.replace("_", "-"), str(v)]
            subprocess.run(args, check=True)
            results.append(json.loads(js.read_text()))
        return merge(results)


def merge(results):
    """Median of each numeric field over the seeds (structure of the first)."""
    def med(values):
        if all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in values):
            return round(statistics.median(values), 3)
        if all(isinstance(v, dict) for v in values):
            return {k: med([v.get(k) for v in values]) for k in values[0]}
        return values[0]
    out = med(results)
    out["seeds"] = len(results)
    out["success_pct_min"] = min(r["success_pct"] for r in results)
    return out


def stable(r):
    return r["success_pct_min"] >= 99.9 and r["latency_ms"]["p95"] <= 1000.0


def search_max(runner, label, lo=25, hi=400, step=5, **opts):
    """Highest stable rate, by bisection between a stable and an unstable rate."""
    best = None
    r = runner.run(f"{label}-{lo}", rate=lo, **opts)
    if not stable(r):
        return {"max_stable_rate": None, "at_lowest": r}
    best = r
    while hi - lo > step:
        mid = (lo + hi) // 2
        r = runner.run(f"{label}-{mid}", rate=mid, **opts)
        if stable(r):
            lo, best = mid, r
        else:
            hi = mid
    return {"max_stable_rate": lo, "at_max": best}


def row(name, r):
    lat, svc = r["latency_ms"], r["service_ms"]
    return (f"| {name} | {r['rate_offered']:.0f} | {r['throughput']:.1f} | {r['success_pct']:.2f} | "
            f"{svc['p50']:.0f} / {svc['p95']:.0f} / {svc['p99']:.0f} | {lat['p50']:.0f} / {lat['p95']:.0f} / {lat['p99']:.0f} | "
            f"{r['retransmissions']['mac_retries']:.0f} / {r['retransmissions']['zcl_retx']:.0f} / "
            f"{r['retransmissions']['host_retries']:.0f} | {r['radio']['air_load_pct']:.0f} | "
            f"{r['cpu']['radio_task_load_pct']:.1f} | {r['queues']['mac_direct_max']:.0f}/{r['queues']['mac_direct_cap']:.0f} |")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build/freertos/throughput"))
    ap.add_argument("--exe", help="prebuilt net_bench (default: built into --out)")
    ap.add_argument("--costs", default=str(ROOT / "build/freertos/arm-test/transaction-cpu.json"),
                    help="tools/bench_transaction_cpu.py output (default instruction counts otherwise)")
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--duration", type=float, default=30)
    ap.add_argument("--suites", default="levels,max,reports,sleepy,bursts")
    ap.add_argument("--quick", action="store_true", help="1 seed, 15 s runs")
    ap.add_argument("--extra", default="", help="options appended to every net_bench run (e.g. '--tick-every-wake 1')")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    if args.quick:
        args.seeds, args.duration = 1, 15
    exe = Path(args.exe) if args.exe else out / ("net_bench" + (".exe" if os.name == "nt" else ""))
    if not args.exe:
        build(exe)
    runner = Runner(exe, out, args.seeds, cost_args(args.costs) + args.extra.split(), args.duration)
    suites = args.suites.split(",")
    report = {"level": "SIMULATION of the portable stack (net_bench), not a hardware measurement",
              "seeds": args.seeds, "duration_s": args.duration, "costs": args.costs if cost_args(args.costs) else "defaults"}
    lines = ["| Scénario | Offert (/s) | Obtenu (/s) | Réussite % | Service p50/p95/p99 (ms) | "
             "Bout en bout p50/p95/p99 (ms) | Réémissions MAC / ZCL / hôte | Air % | CPU % | File MAC |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    if "levels" in suites:
        report["levels"] = {}
        for rate in LEVELS:
            r = runner.run(f"levels-{rate}", rate=rate)
            report["levels"][rate] = r
            lines.append(row(f"arbre 28 routeurs, {rate}/s", r))
    if "max" in suites:
        report["max"] = {
            "tree_single_domain": search_max(runner, "max-tree"),
            "one_hop_single_domain": search_max(runner, "max-onehop", hop1=28, hop2=0, hop3=0),
            "tree_spatial_reuse": search_max(runner, "max-reuse", spatial_reuse=1),
            "tree_115200": search_max(runner, "max-115200", baud=115200),
        }
        for k, v in report["max"].items():
            r = v.get("at_max") or v.get("at_lowest")
            lines.append(row(f"max stable ({k}) = {v['max_stable_rate']}", r))
    if "reports" in suites:
        report["reports"] = {}
        rlines = ["| Rapports entrants | Offerts (/s) | Livrés % | Latence p50/p95/p99 (ms) | Air % | CPU % | "
                  "Tampons RX max | Pertes faute de tampon |", "|---|---|---|---|---|---|---|---|"]
        for topo, topts in (("arbre", {}), ("1 saut", dict(hop1=28, hop2=0, hop3=0))):
            for rep in (50, 100, 150, 180, 250):
                r = runner.run(f"reports-{'tree' if topo == 'arbre' else 'onehop'}-{rep}", rate=0, reports=rep, **topts)
                report["reports"][f"{topo}-{rep}"] = r
                rp = r["reports"]
                rlines.append(f"| {topo} | {rep} | {rp['delivered_pct']:.2f} | {rp['p50_ms']:.0f} / {rp['p95_ms']:.0f} / "
                              f"{rp['p99_ms']:.0f} | {r['radio']['air_load_pct']:.0f} | {r['cpu']['radio_task_load_pct']:.1f} | "
                              f"{r['radio']['rx_entries_max']:.0f} | {r['radio']['rx_buffer_drops']:.0f} |")
        for rx in (4, 8):
            name = f"flash-stall-rx{rx}"
            r = runner.run(name, rate=20, reports=100, hop1=28, hop2=0, hop3=0, rx_entries=rx, stall_at=6000, stall_ms=76,
                           stall_count=15)
            report["reports"][name] = r
            rp = r["reports"]
            rlines.append(f"| 1 saut, 15 blocages Flash de 76 ms, {rx} tampons RX | 100 (+20 transactions) | "
                          f"{rp['delivered_pct']:.2f} | {rp['p50_ms']:.0f} / {rp['p95_ms']:.0f} / {rp['p99_ms']:.0f} | "
                          f"{r['radio']['air_load_pct']:.0f} | {r['cpu']['radio_task_load_pct']:.1f} | "
                          f"{r['radio']['rx_entries_max']:.0f} | {r['radio']['rx_buffer_drops']:.0f} |")
        lines += [""] + rlines
    if "sleepy" in suites:
        report["sleepy"] = {}
        for n in (10, 20, 40):
            half = n // 2
            for name, opts in [
                (f"sed{n}-spread", dict(seds_coord=half, seds_router=n - half, sed_share=0.3, rate=20)),
                (f"sed{n}-sync", dict(seds_coord=half, seds_router=n - half, sed_share=0.3, rate=20, sed_sync=50)),
            ]:
                r = runner.run(name, **opts)
                report["sleepy"][name] = r
                lines.append(row(name, r))
        for name, opts in [
            ("sed20-congested-router", dict(seds_coord=0, seds_router=20, sed_parent=16, sed_share=0.5, rate=20,
                                            congest_router=16, congest_queue=2, router_indirect=4)),
            ("sed10-moved-silently", dict(seds_coord=10, sed_share=0.5, rate=10, duration=40, sed_move_node=29,
                                          sed_move_to=3, sed_move_at=8, reports=3)),
            ("sed20-parent-lost", dict(seds_coord=0, seds_router=20, sed_parent=16, sed_share=0.5, rate=10,
                                       kill_router=16, kill_at=8, duration=40, router_indirect=16)),
        ]:
            r = runner.run(name, **opts)
            report["sleepy"][name] = r
            lines.append(row(name, r))
    if "bursts" in suites:
        report["bursts"] = {}
        lines += ["", "| Rafales (28 lectures ensemble, toutes les 12 s) | Lectures | Échecs au 1er essai | Réussite finale % | "
                  "Bout en bout p95 (ms) | Trames perdues par le relais |", "|---|---|---|---|---|---|"]
        for relay in (0, 1, 2):
            opts = dict(rate=0, burst_size=28, burst_every_ms=12000, concurrency=32, duration=120)
            if relay:
                opts.update(lossy_relay=relay, lossy_pct=30)
            name = f"bursts-relay{relay}"
            r = runner.run(name, **opts)
            report["bursts"][name] = r
            label = "aucun relais défaillant" if not relay else f"relais {relay} perdant 30 %"
            lines.append(f"| {label} | {r['offered']:.0f} | {r['retransmissions']['host_retries']:.0f} | "
                         f"{r['success_pct']:.2f} | {r['latency_ms']['p95']:.0f} | {r['radio'].get('lossy_relay_drops', 0):.0f} |")
    (out / "summary.json").write_text(json.dumps(report, indent=1) + "\n")
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    if "sleepy" in suites:
        print("\n| Scénario endormis | Transactions / réussies | Polls | Bit pending sans trame | Réassociations | "
              "Expirées (coord / routeur) | File pleine (coord / routeur) |")
        print("|---|---|---|---|---|---|---|")
        for k, r in report["sleepy"].items():
            s = r["sed"]
            print(f"| {k} | {s['transactions']:.0f} / {s['ok']:.0f} | {s['polls']:.0f} | {s['fp_miss']:.0f} | "
                  f"{s['rejoins']:.0f} | {s['coord_indirect_expired']:.0f} / {s['router_child_expired']:.0f} | "
                  f"{s['coord_poll_queue_full']:.0f} / {s['router_child_queue_full']:.0f} |")
    print(f"\n{runner.n} simulations; results in {out}")


if __name__ == "__main__":
    main()
