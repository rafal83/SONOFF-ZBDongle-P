"""Build host tests or the real UART-only CC2652P laboratory image. No device I/O."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"

def run(args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)

def compiler():
    if os.environ.get("ZIG"):
        return [os.environ["ZIG"], "cc"]
    local = ROOT / ".tooling/ziglang" / ("zig.exe" if os.name == "nt" else "zig")
    if local.exists():
        return [str(local), "cc"]
    if shutil.which("zig"):
        return ["zig", "cc"]
    raise SystemExit("Install: python -m pip install --target .tooling ziglang==0.13.0")

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("target", choices=["host", "diagnostic"])
    args = ap.parse_args()
    BUILD.mkdir(exist_ok=True)
    # Keep compiler caches inside this project, including in restricted environments.
    os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / ".cache/zig"))
    cc = compiler()
    version = subprocess.check_output([cc[0], "version"], text=True).strip()
    if version != "0.13.0":
        raise SystemExit(f"Expected Zig 0.13.0, got {version}")
    common = ["-std=c11", "-O2", "-g", "-Ifirmware/src"]
    core = ["firmware/src/npi.c", "firmware/src/diagnostic.c"]
    if args.target == "host":
        output = BUILD / ("test_npi.exe" if os.name == "nt" else "test_npi")
        run(cc + common + ["-UNDEBUG", "-Wall", "-Wextra", "-Werror"] + core + ["tests/test_npi.c", "-o", output])
        run([output])
        fixture = BUILD / ("npi_stdio.exe" if os.name == "nt" else "npi_stdio")
        run(cc + common + ["-Wall", "-Wextra", "-Werror"] + core + ["tests/npi_stdio.c", "-o", fixture])
    else:
        sdk = ROOT / "upstream/ti-sdk"
        expected = json.loads((ROOT / "sources.lock.json").read_text())["ti-sdk"]["commit"]
        actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
        if actual != expected:
            raise SystemExit("TI SDK revision differs from sources.lock.json")
        subprocess.run(["git", "-C", str(sdk), "diff", "--exit-code", "HEAD", "--",
                        "source/ti/devices/cc13x2_cc26x2"], check=True)
        device = sdk / "source/ti/devices/cc13x2_cc26x2"
        board = "firmware/platform/sonoff_cc2652p"
        flags = common + ["-target", "thumb-freestanding-eabi", "-mcpu=cortex_m4",
            "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-ffunction-sections",
            "-fdata-sections", "-DNDEBUG", "-I" + str(device), "-I" + board,
            "-Ifirmware/platform/freestanding"]
        # Compile only these BSD-licensed TI device support files.
        ti_sources = [device / "startup_files/startup_gcc.c"] + [
            device / "driverlib" / (name + ".c") for name in
            ["setup", "setup_rom", "prcm", "ioc", "uart", "osc", "ddi", "sys_ctrl", "chipinfo", "interrupt", "cpu", "aon_rtc", "aux_sysif"]]
        sources = core + [f"{board}/{name}.c" for name in ["main", "ccfg", "runtime"]] + ti_sources
        objects = []
        for index, source in enumerate(sources):
            obj = BUILD / f"arm_{index}.o"
            run(cc + flags + ["-c", source, "-o", obj])
            objects.append(obj)
        output = BUILD / "sonoff-uart-diagnostic.elf"
        run(cc + flags + ["-nostdlib", "-Wl,--gc-sections", "-Wl,-e,ResetISR", "-Wl,-T," + board + "/memory.ld"]
            + objects + ["-o", output])
        run([sys.executable, "tools/image.py", "export", output, BUILD / "sonoff-uart-diagnostic.hex"])
        run([sys.executable, "tools/image.py", "check", BUILD / "sonoff-uart-diagnostic.hex"])
        shutil.copyfile(device / "startup_files/startup_gcc.c", BUILD / "TI-startup-source-and-license.txt")
        notices = ["TI source notices for the linked UART diagnostic. Original source revisions: sources.lock.json.\n"]
        for source in ti_sources:
            original = Path(source).read_text(encoding="utf-8")
            notices.append(str(Path(source).relative_to(sdk)) + "\n" + original[:original.index("*/") + 2])
        (BUILD / "THIRD_PARTY_NOTICES.txt").write_text("\n\n".join(notices), encoding="utf-8")
    manifest = {"target": args.target, "compiler": "Zig " + version,
                "artifact": output.name, "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                "sources_lock_sha256": hashlib.sha256((ROOT / "sources.lock.json").read_bytes()).hexdigest(),
                "hardware_tested": False}
    if args.target == "diagnostic":
        manifest["hex_sha256"] = hashlib.sha256((BUILD / "sonoff-uart-diagnostic.hex").read_bytes()).hexdigest()
    (BUILD / f"{args.target}-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))

if __name__ == "__main__":
    main()
