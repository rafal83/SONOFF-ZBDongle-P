"""Build and test the FreeRTOS target for the SONOFF ZBDongle-P (CC2652P).

  python tools/build_freertos.py host                 # native unit tests
  python tools/build_freertos.py arm                  # same vectors on ARM code (Unicorn)
  python tools/build_freertos.py firmware [--baud N]  # ARM ELF/HEX + checks
  python tools/build_freertos.py all                  # both, both baud rates

Outputs go to build/freertos/ only; the Z-Stack images under build/ are never
touched. No serial port, flasher or network is used. Every artifact is
labelled with its validation level in its manifest.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "freertos"
FRT = ROOT / "firmware" / "freertos"
KERNEL = ROOT / "upstream" / "FreeRTOS-Kernel"
SDK = ROOT / "vendor-sdk" / "simplelink_cc13xx_cc26xx_sdk_8_33_00_16" / "source"
DEVICE = SDK / "ti" / "devices" / "cc13x2_cc26x2"
PRODUCTION_HEX = ROOT / "build" / "coordinator-optimized-1000000-cores-sdk833-r2" / "sonoff-znp-coordinator.hex"

# Portable modules: compiled for the host tests AND for the MCU image.
PORTABLE = [
    "firmware/src/npi.c",
    "firmware/src/core_index.c",
    "firmware/src/core_security.c",
    "firmware/src/core_indirect.c",
    "firmware/freertos/transport/znp/mt.c",
    "firmware/freertos/diagnostics/wd_quorum.c",
    "firmware/freertos/drivers/radio/rf_logic.c",
    "firmware/freertos/ieee802154/mac/mac_frame.c",
    "firmware/freertos/ieee802154/mac/mac_core.c",
    "firmware/freertos/zigbee/security/aes128.c",
    "firmware/freertos/zigbee/security/ccm_star.c",
    "firmware/freertos/zigbee/security/zb_mmo.c",
    "firmware/freertos/zigbee/security/zb_ccm_backend.c",
    "firmware/freertos/zigbee/nwk/nwk_frame.c",
    "firmware/freertos/zigbee/aps/aps_frame.c",
    "firmware/freertos/zigbee/aps/aps_security.c",
    "firmware/freertos/zigbee/nwk/nwk_devices.c",
    "firmware/freertos/zigbee/nwk/nwk_route.c",
    "firmware/freertos/zigbee/nwk/zb_coord.c",
    "firmware/freertos/storage/nv_log.c",
    "firmware/freertos/zigbee/tc/trust_center.c",
    "firmware/freertos/transport/znp/znp.c",
    "firmware/freertos/zigbee/gp/gp_proxy.c",
]
INCLUDES = [
    "firmware/src",
    "firmware/freertos/config",
    "firmware/freertos/board",
    "firmware/freertos/common",
    "firmware/freertos/diagnostics",
    "firmware/freertos/hal",
    "firmware/freertos/app",
    "firmware/freertos/transport/znp",
    "firmware/freertos/drivers/radio",
    "firmware/freertos/ieee802154/mac",
    "firmware/freertos/zigbee/security",
    "firmware/freertos/zigbee/nwk",
    "firmware/freertos/zigbee/aps",
    "firmware/freertos/zigbee/tc",
    "firmware/freertos/zigbee/gp",
    "firmware/freertos/storage",
]
MCU_SOURCES = [
    "firmware/freertos/startup/startup.c",
    "firmware/freertos/runtime/runtime.c",
    "firmware/freertos/board/ccfg.c",
    "firmware/freertos/diagnostics/crash_log.c",
    "firmware/freertos/hal/hal_sys.c",
    "firmware/freertos/hal/hal_uart.c",
    "firmware/freertos/hal/hal_wdt.c",
    "firmware/freertos/hal/hal_trng.c",
    "firmware/freertos/hal/hal_flash.c",
    "firmware/freertos/hal/hal_aes.c",
    "firmware/freertos/kernel/rtos_hooks.c",
    "firmware/freertos/drivers/radio/rf_settings.c",
    "firmware/freertos/drivers/radio/rf_ieee.c",
    "firmware/freertos/app/main.c",
    "firmware/freertos/app/host_task.c",
    "firmware/freertos/app/radio_task.c",
    "firmware/freertos/app/supervisor_task.c",
]
KERNEL_SOURCES = ["tasks.c", "queue.c", "list.c", "timers.c", "portable/GCC/ARM_CM4F/port.c"]
# BSD-licensed TI device support only (no TI-RTOS, no DPL, no RF.a, no Z-Stack).
TI_SOURCES = [DEVICE / "driverlib" / f"{n}.c" for n in [
    "setup", "setup_rom", "chipinfo", "prcm", "osc", "ddi", "adi", "sys_ctrl", "ioc", "uart",
    "interrupt", "cpu", "aon_rtc", "aux_sysif", "rfc", "watchdog", "aon_pmctl", "vims", "trng", "flash", "aes"]] + [
    DEVICE / "rf_patches" / "rf_patch_cpe_ieee_802_15_4.c"]
HOST_TESTS = {
    "test_frt_mt": ["firmware/freertos/tests/test_mt.c"],
    "test_frt_uart_noise": ["firmware/freertos/tests/test_uart_noise.c"],
    "test_frt_radio_logic": ["firmware/freertos/tests/test_radio_logic.c"],
    "test_frt_quorum": ["firmware/freertos/tests/test_quorum.c"],
    "test_frt_spsc": ["firmware/freertos/tests/test_spsc_ring.c"],
    "test_frt_mac_frame": ["firmware/freertos/tests/test_mac_frame.c"],
    "test_frt_mac_core": ["firmware/freertos/tests/test_mac_core.c"],
    "test_frt_crypto": ["firmware/freertos/tests/test_crypto.c"],
    "test_frt_hal_aes": ["firmware/freertos/tests/test_hal_aes.c"],
    "test_frt_nwk_aps": ["firmware/freertos/tests/test_nwk_aps.c"],
    "test_frt_nwk_security": ["firmware/freertos/tests/test_nwk_security.c"],
    "test_frt_nwk_join": ["firmware/freertos/tests/test_nwk_join.c"],
    "test_frt_join_e2e": ["firmware/freertos/tests/test_join_e2e.c"],
    "test_frt_nv_log": ["firmware/freertos/tests/test_nv_log.c"],
    "test_frt_mesh": ["firmware/freertos/tests/test_mesh.c"],
    "test_frt_store_integrity": ["firmware/freertos/tests/test_store_integrity.c"],
    "test_frt_mesh_scale": ["firmware/freertos/tests/test_mesh_scale.c"],
    "test_frt_sleepy": ["firmware/freertos/tests/test_sleepy.c"],
    "test_frt_gp": ["firmware/freertos/tests/test_gp.c"],
}


def run(args, **kw):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True, **kw)


def zig():
    os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / ".cache/zig"))
    local = ROOT / ".tooling/ziglang" / ("zig.exe" if os.name == "nt" else "zig")
    exe = str(local) if local.exists() else shutil.which("zig")
    if not exe:
        raise SystemExit("Install: python -m pip install --target .tooling ziglang==0.13.0")
    version = subprocess.check_output([exe, "version"], text=True).strip()
    if version != "0.13.0":
        raise SystemExit(f"Expected Zig 0.13.0, got {version}")
    return exe, version


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_kernel():
    lock = json.loads((ROOT / "sources.lock.json").read_text())["FreeRTOS-Kernel"]
    if not KERNEL.exists():
        raise SystemExit("Run: python tools/fetch_sources.py FreeRTOS-Kernel")
    head = subprocess.check_output(["git", "-C", str(KERNEL), "rev-parse", "HEAD"], text=True).strip()
    if head != lock["commit"]:
        raise SystemExit(f"FreeRTOS-Kernel at {head}, expected {lock['commit']}")
    if subprocess.run(["git", "-C", str(KERNEL), "diff", "--quiet", "HEAD"]).returncode:
        raise SystemExit("FreeRTOS-Kernel checkout is modified")
    return lock


def host(exe):
    out = OUT / "host"
    out.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-O1", "-g", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-fsanitize=undefined", "-fsanitize-trap=undefined"]
    flags += [f"-I{p}" for p in INCLUDES]
    flags += [f"-I{DEVICE}"]   # register definitions only (hal_aes model test)
    results = {}
    for name, sources in HOST_TESTS.items():
        if name == "test_frt_hal_aes" and not (DEVICE / "inc" / "hw_crypto.h").exists():
            # Needs the TI register definitions (SDK fetched by the firmware
            # workflow only): reported as skipped, never as passed.
            print(f"{name}: SKIPPED (TI SDK absent: python tools/download_ti_toolchain.py sdk833 --extract)")
            results[name] = {"status": "skipped", "reason": "TI SDK register headers absent"}
            continue
        binary = out / (name + (".exe" if os.name == "nt" else ""))
        run([exe, "cc", *flags, *PORTABLE, *sources, "-o", binary])
        proc = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
        sys.stdout.write(proc.stdout)
        if proc.returncode:
            sys.stdout.write(proc.stderr)
            raise SystemExit(f"{name} failed with {proc.returncode}")
        results[name] = {"status": "pass", "output": proc.stdout.strip().splitlines()[-1:]}
    report = {"level": "unit tests on host (x86-64), not on the MCU", "compiler": "Zig cc 0.13.0",
              "tests": results,
              "sources_sha256": {s: sha256(ROOT / s) for s in PORTABLE + sum(HOST_TESTS.values(), [])}}
    (out / "host-tests.json").write_text(json.dumps(report, indent=2) + "\n")
    passed = sum(1 for r in results.values() if r["status"] == "pass")
    skipped = len(results) - passed
    print(f"host: {passed} test programs passed" + (f", {skipped} skipped" if skipped else ""))
    return report


def elf_report(elf):
    sys.path.insert(0, str(ROOT / ".tooling/arm-tests"))
    from elftools.elf.elffile import ELFFile
    with open(elf, "rb") as f:
        e = ELFFile(f)
        sections = {s.name: {"addr": s["sh_addr"], "size": s["sh_size"]}
                    for s in e.iter_sections() if s["sh_flags"] & 2}
        symtab = e.get_section_by_name(".symtab")
        symbols = {s.name: s["st_value"] for s in symtab.iter_symbols() if s.name}
        sizes = {s.name: s["st_size"] for s in symtab.iter_symbols() if s.name and s["st_info"]["type"] == "STT_OBJECT"}
    return sections, symbols, sizes


def arm_flags(baud, revision):
    return ["-std=c11", "-Os", "-g", "-target", "thumb-freestanding-eabihf", "-mcpu=cortex_m4+vfp4d16sp",
            "-mfloat-abi=hard", "-ffreestanding", "-fno-stack-protector", "-ffunction-sections",
            "-fdata-sections", "-fno-common", "-DNDEBUG", "-DDeviceFamily_CC26X2",
            f"-DBOARD_UART_BAUD={baud}u", f"-DFRT_BUILD_REVISION={revision}u",
            "-Ifirmware/freertos/runtime/include", f"-I{DEVICE}", f"-I{SDK}", f"-I{KERNEL / 'include'}",
            f"-I{KERNEL / 'portable/GCC/ARM_CM4F'}"] + [f"-I{p}" for p in INCLUDES]


def compile_arm(exe, flags, sources, obj_dir):
    objects = []
    for index, source in enumerate(sources):
        obj = obj_dir / f"{index:03d}_{Path(source).stem}.o"
        extra = []
        if str(source).startswith("firmware/freertos/") or str(source).startswith("firmware/src/"):
            extra = ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
        if Path(source).name == "runtime.c":
            extra.append("-fno-builtin")
        run([exe, "cc", *flags, *extra, "-c", source, "-o", obj])
        objects.append(obj)
    return objects


def arm_tests(exe):
    """Portable modules compiled for Cortex-M4F, executed in Unicorn."""
    out = OUT / "arm-test"
    obj_dir = out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objects = compile_arm(exe, arm_flags(115200, 0), PORTABLE + ["firmware/freertos/runtime/runtime.c",
                                                                      "firmware/freertos/tests/arm_sim.c",
                                                                      "firmware/freertos/tests/arm_perf.c"], obj_dir)
    elf = out / "freertos-portable-test-only.elf"
    run([exe, "cc", *arm_flags(115200, 0), "-nostdlib", "-Wl,--no-gc-sections", "-Wl,--entry=aes128_init",
         "-Wl,-T,firmware/freertos/tests/arm_test.ld",
         *objects, "-o", elf])
    _, symbols, _ = elf_report(elf)
    if symbols.get("_ebss", 0) > 0x20040000:
        raise SystemExit("test ELF .bss overlaps the Unicorn test heap")
    run([sys.executable, "tools/test_arm_freertos.py", elf, out / "arm-tests.json"])


def firmware(exe, baud, hw_aes=False):
    lock = check_kernel()
    out = OUT / (f"diag-{baud}-hwaes" if hw_aes else f"diag-{baud}")
    obj_dir = out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    revision = int(datetime.date.today().strftime("%Y%m%d"))
    flags = arm_flags(baud, revision) + [f"-DFRT_HW_AES={1 if hw_aes else 0}"]
    own = PORTABLE + MCU_SOURCES
    objects = compile_arm(exe, flags, own + [KERNEL / s for s in KERNEL_SOURCES] + TI_SOURCES, obj_dir)
    elf = out / "sonoff-freertos-diag.elf"
    hexfile = out / "sonoff-freertos-diag.hex"
    run([exe, "cc", *flags, "-nostdlib", "-Wl,--gc-sections", "-Wl,-e,Reset_Handler",
         "-Wl,-T,firmware/freertos/startup/cc2652p_freertos.ld",
         *objects, "-o", elf])
    run([sys.executable, "tools/image.py", "export", elf, hexfile])
    check = subprocess.run([sys.executable, "tools/image.py", "check", hexfile], cwd=ROOT,
                           capture_output=True, text=True)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit("tools/image.py check failed")

    sections, symbols, sizes = elf_report(elf)
    flash_used = symbols["_flash_used"]
    sram_used = symbols["_sram_used"]
    required = ["Reset_Handler", "vector_table", "SVC_Handler", "PendSV_Handler", "SysTick_Handler",
                "UART0_IRQHandler", "RFC_CPE0_IRQHandler", "WDT_IRQHandler", "mt_dispatch",
                "vTaskStartScheduler", "rf_patch_cpe_ieee_802_15_4"]
    missing = [s for s in required if s not in symbols]
    if missing:
        raise SystemExit(f"Missing symbols: {missing}")
    if hw_aes:
        required_hw = ["hal_aes_selftest", "hal_aes_ccm_selftest", "hal_aes_ccm",
                       "zb_ccm_backend_set_native", "zb_ccm_backend_hw"]
        if any(name not in symbols for name in required_hw):
            raise SystemExit('Hardware AES/CCM* backend or startup self-tests missing from ELF')
    forbidden = [s for s in symbols if s in ("malloc", "free", "pvPortMalloc", "Power_init", "RF_open",
                                               "BIOS_start", "osal_init_system", "NVOCMP_initNvApi")]
    if forbidden:
        raise SystemExit(f"Forbidden symbols linked: {forbidden}")
    # Only hal_flash.c may name a TI Flash write function (bounded to the store).
    writers = [s for s in own if s != "firmware/freertos/hal/hal_flash.c" and
               any(t in (ROOT / s).read_text(encoding="utf-8")
                   for t in ("FlashProgram", "FlashSectorErase", "FlashProgram4X", "HapiProgramFlash"))]
    if writers:
        raise SystemExit(f"Flash write outside hal_flash.c: {writers}")

    sys.path.insert(0, str(ROOT / "tools"))
    import image
    memory = image.read_hex(hexfile)
    identity = bytes(memory.get(a, 255) for a in range(0x100, 0x118))
    if (identity[:8] != b'ZBDP-FRT' or
            tuple(int.from_bytes(identity[a:a + 4], 'little') for a in (8, 12, 16, 20)) != (1, 1, baud, revision)):
        raise SystemExit('Missing or invalid FreeRTOS application identity / storage layout')
    ccfg_match = None
    if PRODUCTION_HEX.exists():
        prod = image.read_hex(PRODUCTION_HEX)
        ccfg_match = all(image.word(memory, a) == image.word(prod, a) for a in range(0x57FA8, 0x58000, 4))
        if not ccfg_match:
            raise SystemExit("CCFG differs from the installed production image")
    import stack_analysis
    stacks = stack_analysis.analyse(elf)
    thin = {t: v["margin_bytes"] for t, v in stacks["tasks"].items() if v["margin_bytes"] < 256}
    if stacks["interrupts"]["msp_allocated_bytes"] - stacks["interrupts"]["nested_worst_bytes"] < 256:
        thin["interrupts"] = stacks["interrupts"]["msp_allocated_bytes"] - stacks["interrupts"]["nested_worst_bytes"]
    if thin:
        raise SystemExit(f"Stack margin below 256 bytes (static analysis): {thin}")
    print("stacks: " + stack_analysis.summary(stacks))
    biggest = sorted(sizes.items(), key=lambda kv: -kv[1])[:15]
    manifest = {
        "target": "SONOFF ZBDongle-P CC2652P - FreeRTOS Zigbee coordinator (ZNP), laboratory release",
        "validation": {
            "compiled_and_linked": True,
            "static_image_checks": True,
            "host_unit_tests": "see build/freertos/host/host-tests.json",
            "arm_instruction_tests": "portable modules only, see build/freertos/arm-test/arm-tests.json",
            "executed_on_mcu": "this exact image: not recorded here; previous images of the branch: see "
                               "docs/firmware/validation.md",
            "zigbee_stack": "native coordinator: join, Trust Center, unique TCLK, mesh routing (concentrator), "
                            "persistence; limitations in docs/firmware/znp-compat.md",
            "zigbee2mqtt_compatible": "laboratory: zigbee-herdsman 11.2.1 acceptance (simulator and dongle) and "
                                      "one real device paired and controlled; not a Z-Stack replacement for "
                                      "every feature",
            "production_use": "experimental; review hardware validation and compatibility before use; see "
                              "docs/firmware/installation.md and docs/firmware/restauration.md",
        },
        "uart_baud": baud,
        "hardware_aes": hw_aes,
        "image_identity": {"address": 0x100, "version": 1, "storage_layout": 1},
        "build_revision": revision,
        "compiler": "Zig cc 0.13.0 (clang/LLD)",
        "freertos_kernel": {"tag": lock["tag"], "commit": lock["commit"]},
        "ti_sources": [str(Path(p).relative_to(SDK)).replace("\\", "/") for p in TI_SOURCES],
        "elf_sha256": sha256(elf),
        "hex_sha256": sha256(hexfile),
        "flash_used_bytes": flash_used,
        "flash_app_limit_bytes": 0x50000,
        "sram_static_bytes_including_msp_stack": sram_used,
        "sram_total_bytes": 0x14000,
        "stack_static_analysis": {"tool": "tools/stack_analysis.py (upper bound under its indirect-call rules)",
                                  "tasks": {t: {k: v[k] for k in ("allocated_bytes", "static_worst_bytes",
                                                                    "margin_bytes")}
                                            for t, v in stacks["tasks"].items()},
                                  "interrupts": {k: stacks["interrupts"][k]
                                                 for k in ("msp_allocated_bytes", "nested_worst_bytes")}},
        "sections": sections,
        "largest_objects": dict(biggest),
        "nv_region_untouched": True,
        "flash_writes": "only hal_flash.c, bounded to 0x48000-0x4FFFF (nv_log store)",
        "ccfg_identical_to_installed_2026111802": ccfg_match,
        "image_check": check.stdout.strip().splitlines()[-1:] if check.stdout else [],
        "sources_sha256": {s: sha256(ROOT / s) for s in own},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"firmware {baud}: flash {flash_used} B, static SRAM {sram_used} B -> {hexfile.relative_to(ROOT)}")
    return manifest


def sim(exe):
    """Host ZNP simulator (portable stack over TCP) for the herdsman acceptance test."""
    out = OUT / "sim"
    out.mkdir(parents=True, exist_ok=True)
    binary = out / ("znp_sim" + (".exe" if os.name == "nt" else ""))
    flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-fsanitize=undefined", "-fsanitize-trap=undefined"]
    flags += [f"-I{p}" for p in INCLUDES]
    libs = ["-lws2_32"] if os.name == "nt" else []
    run([exe, "cc", *flags, *PORTABLE, "firmware/freertos/sim/znp_sim.c", "-o", binary, *libs])
    print(f"sim: {binary.relative_to(ROOT)}")
    return binary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", choices=["host", "arm", "firmware", "sim", "all"])
    ap.add_argument("--baud", type=int, choices=[115200, 1000000], default=115200)
    ap.add_argument("--hw-aes", action="store_true",
                    help="firmware: variant using the AES accelerator after its boot self-test (not hardware-validated)")
    args = ap.parse_args()
    exe, _ = zig()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.target in ("host", "all"):
        host(exe)
    if args.target in ("sim", "all"):
        sim(exe)
    if args.target in ("arm", "all"):
        arm_tests(exe)
    if args.target == "firmware":
        firmware(exe, args.baud, args.hw_aes)
    if args.target == "all":
        for baud in (115200, 1000000):
            firmware(exe, baud, args.hw_aes)


if __name__ == "__main__":
    main()
