"""Fuzz the FreeRTOS firmware's decoders and coordinator on the PC.

libFuzzer + AddressSanitizer + UBSan (clang 18) in a disposable Docker
container (tools/fuzz/Dockerfile); zig cc does not provide them on Windows.
Only the portable C of the firmware is compiled; no hardware, no network
access from the fuzzed code.

  python tools/fuzz_freertos.py --seconds 600            # every harness, 10 min each
  python tools/fuzz_freertos.py --only coord --seconds 3600

Harnesses (firmware/freertos/tests/fuzz/):
  parsers  MAC / beacon / NWK / NWK aux / APS decoders, MT-NPI serial parser
  coord    whole coordinator + ZNP: raw air frames, frames secured with the
           real network key (paths behind decryption), host requests, time
  nvlog    nv_log mount / read / salvage / write / compaction and the
           coordinator restore, on arbitrary Flash contents

Results in build/freertos/fuzz/: corpus per harness, crash-* files with the
input that triggered a sanitizer report, and fuzz-report.json.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_freertos  # noqa: E402

IMAGE = "zb-fuzz:clang18"
HARNESSES = ("parsers", "coord", "nvlog")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=300, help="fuzzing time per harness")
    ap.add_argument("--only", choices=HARNESSES)
    ap.add_argument("--jobs", type=int, default=4, help="parallel fuzzing workers per harness")
    args = ap.parse_args()
    subprocess.run(["docker", "build", "-q", "-t", IMAGE, str(ROOT / "tools/fuzz")], check=True)
    out = ROOT / "build/freertos/fuzz"
    out.mkdir(parents=True, exist_ok=True)
    sources = " ".join(build_freertos.PORTABLE)
    includes = " ".join(f"-I{p}" for p in build_freertos.INCLUDES)
    flags = "-std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
    report = {"level": "libFuzzer + ASan + UBSan on the host (x86-64 Linux container), not on the MCU",
              "seconds_per_harness": args.seconds, "harnesses": {}}
    for name in HARNESSES if not args.only else (args.only,):
        corpus = f"build/freertos/fuzz/corpus-{name}"
        (ROOT / corpus).mkdir(parents=True, exist_ok=True)
        script = (
            f"set -e; clang-18 {flags} {includes} {sources} firmware/freertos/tests/fuzz/fuzz_{name}.c "
            f"-o /tmp/fuzz_{name} && "
            f"/tmp/fuzz_{name} {corpus} -max_total_time={args.seconds} -jobs={args.jobs} -workers={args.jobs} "
            f"-max_len=256 -artifact_prefix=build/freertos/fuzz/crash-{name}- -print_final_stats=1 "
            f"> build/freertos/fuzz/{name}.log 2>&1; echo EXIT=$?")
        proc = subprocess.run(["docker", "run", "--rm", "-e", "ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-18/bin/llvm-symbolizer",
                               "-v", f"{ROOT}:/src", "-w", "/src", IMAGE,
                               "bash", "-c", script], capture_output=True, text=True)
        crashes = sorted(p.name for p in out.glob(f"crash-{name}-*"))
        logs = sorted(out.glob(f"fuzz-*.log")) + [out / f"{name}.log"]
        text = "\n".join(p.read_text(errors="replace") for p in logs if p.exists())
        runs = [int(l.split()[-1]) for l in text.splitlines() if l.startswith("stat::number_of_executed_units")]
        report["harnesses"][name] = {
            "compiled": "EXIT=" in proc.stdout or bool(runs),
            "executions": sum(runs), "corpus_files": len(list((ROOT / corpus).iterdir())),
            "crashes": crashes, "stderr_tail": proc.stderr[-400:] if proc.returncode else "",
        }
        for p in out.glob("fuzz-*.log"):
            p.unlink()
        print(f"{name}: {sum(runs)} executions, {len(crashes)} crash file(s)")
    (out / "fuzz-report.json").write_text(json.dumps(report, indent=2) + "\n")
    if any(h["crashes"] for h in report["harnesses"].values()):
        raise SystemExit("sanitizer findings: see build/freertos/fuzz/crash-*")


if __name__ == "__main__":
    main()
