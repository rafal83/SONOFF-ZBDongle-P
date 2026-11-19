"""Execute native tests of actual TI-source overlays with simulated peripherals."""
import os
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from build import compiler
from coordinator_overlays import generate, prepare_tlsf

ROOT = Path(__file__).resolve().parents[1]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk-version',choices=['8.30.01.01','8.33.00.16'],default='8.30.01.01')
    args=ap.parse_args()
    suffix='-sdk833-r2' if args.sdk_version=='8.33.00.16' else ''
    sdk = ROOT / ('vendor-sdk/simplelink_cc13xx_cc26xx_sdk_'+args.sdk_version.replace('.','_'))
    generated = ROOT / ('build/coordinator-overlay'+suffix)
    generate(sdk, generated)
    prepare_tlsf(generated)
    os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / ".cache/zig"))
    out = ROOT / "build/test_nv_znp.exe"
    command = compiler() + ["-std=c11", "-O1", "-g", "-UNDEBUG", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-unused-function", "-DNV_LINUX", "-DNVOCMP_NVPAGES=3",
        "-DNVOCMP_RECOVER_FROM_COMPACT_FAILURE", "-Itests/ti_stubs", "-Ifirmware/src",
        "-I" + str(generated), "-I" + str(sdk / "source/ti/common/nv"), "-I" + str(sdk / "source"),
        "-Ifirmware/platform/ti_znp", "tests/test_nv_znp.c", "firmware/src/nv_cache.c", "firmware/src/nv_index.c",
        str(sdk / "source/ti/common/nv/crc.c"), "-o", str(out)]
    subprocess.run(command, cwd=ROOT, check=True)
    nv_output = subprocess.check_output([str(out)], cwd=ROOT, text=True)
    print(nv_output, end="")
    out = ROOT / "build/test_port_npi.exe"
    command = compiler() + ["-std=c11", "-O2", "-g", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
        "-Itests/ti_stubs", "-Ifirmware/src", "-Ifirmware/platform/ti_znp", "tests/test_port_npi.c",
        "firmware/src/npi.c", "firmware/platform/ti_znp/port_npi.c", "-o", str(out)]
    subprocess.run(command, cwd=ROOT, check=True)
    npi_output = subprocess.check_output([str(out)], cwd=ROOT, text=True)
    print(npi_output, end="")
    extra = {}
    configurations = [
        ("heap", ["tests/test_port_heap.c", "firmware/src/port_heap.c", str(generated / "tlsf.c")],
         ["-DPORT_HEAP_HOST_TEST", "-Ifirmware/src", "-I" + str(generated)]),
        ("timers", ["tests/test_port_timers.c", "firmware/platform/ti_znp/port_timers.c"],
         ["-Itests/ti_stubs", "-Ifirmware/platform/ti_znp"]),
        ("watchdog", ["tests/test_port_watchdog.c", "firmware/platform/ti_znp/port_watchdog.c"],
         ["-Itests/ti_stubs", "-Ifirmware/platform/ti_znp"]),
        ("zmac", ["tests/test_zmac_datareq.c"], ["-I" + str(generated)]),
        ("radio_guard", ["tests/test_radio_guard.c", "firmware/src/radio_guard.c"],
         ["-Ifirmware/src", "-I" + str(generated)]),
        ("nv_index", ["tests/test_nv_index.c", "firmware/src/nv_index.c"], ["-Ifirmware/src"]),
        ("autobaud", ["tests/test_autobaud.c", "firmware/src/autobaud.c", "firmware/platform/ti_znp/port_autobaud.c"],
         ["-Ifirmware/src", "-Ifirmware/platform/ti_znp", "-Itests/ti_stubs"]),
        ("autobaud_fixed", ["tests/test_autobaud.c", "firmware/src/autobaud.c", "firmware/platform/ti_znp/port_autobaud.c"],
         ["-DPORT_AUTOBAUD_ENABLE=0", "-Ifirmware/src", "-Ifirmware/platform/ti_znp", "-Itests/ti_stubs"]),
        ("npi_uart", ["tests/test_npi_uart.c"], ["-I" + str(generated), "-Wno-unused-parameter"])]
    configurations.append(("broadcast", ["tests/test_broadcast.c", "firmware/platform/ti_znp/port_broadcast.c"],
        ["-Ifirmware/platform/ti_znp", "-Itests/broadcast_stubs", "-Itests/ti_stubs"]))
    configurations.append(("security_persistence", ["tests/test_security_persistence.c"], ["-I" + str(generated)]))
    configurations.append(("security_persistence205", ["tests/test_security_persistence.c"], ["-DTEST_SECURITY_CAPACITY=205", "-I" + str(generated)]))
    configurations.append(("task_queue", ["tests/test_task_queue.c"],
        ["-Ifirmware/platform/ti_znp", "-I" + str(generated)]))
    if os.name == "nt":
        configurations.append(("heap32", ["tests/test_port_heap.c", "firmware/src/port_heap.c", str(generated / "tlsf.c")],
            ["-target", "x86-windows-gnu", "-DPORT_HEAP_HOST_TEST", "-Ifirmware/src", "-I" + str(generated)]))
        configurations.append(("task_queue32", ["tests/test_task_queue.c"],
            ["-target", "x86-windows-gnu", "-Ifirmware/platform/ti_znp", "-I" + str(generated)]))
    for name, files, includes in configurations:
        target = ROOT / "build" / ("test_port_" + name + ".exe")
        subprocess.run(compiler() + ["-std=c11", "-O2", "-g", "-UNDEBUG", "-Wall", "-Wextra", "-Werror"]
                       + includes + files + ["-o", str(target)], cwd=ROOT, check=True)
        extra[name] = subprocess.check_output([str(target)], cwd=ROOT, text=True).strip()
        print(extra[name])
    inputs = [ROOT / name for name in ["firmware/src/npi.c", "firmware/src/npi.h", "firmware/src/nv_cache.c",
        "firmware/src/nv_cache.h", "firmware/platform/ti_znp/port_npi.c", "firmware/platform/ti_znp/port_npi.h",
        "tests/test_nv_znp.c", "tests/test_port_npi.c", "tools/coordinator_overlays.py",
        "firmware/src/port_heap.c", "firmware/src/port_heap.h", "firmware/platform/ti_znp/port_heap_ti.h",
        "firmware/platform/ti_znp/port_timers.c", "firmware/platform/ti_znp/port_timers.h",
        "tests/test_port_heap.c", "tests/test_port_timers.c", "tests/test_port_watchdog.c",
        "firmware/platform/ti_znp/port_watchdog.c", "firmware/platform/ti_znp/port_watchdog.h",
        "tests/test_zmac_datareq.c", "firmware/config/znp_optimized.h",
        "tools/build_coordinator.py", "tools/audit_coordinator.py", "tests/test_radio_guard.c",
        "firmware/src/radio_guard.c", "firmware/src/radio_guard.h", "tools/test_arm_radio.py",
        "tools/arm-tests-requirements.txt", "firmware/src/nv_index.c", "firmware/src/nv_index.h",
        "firmware/platform/ti_znp/nv_index_ti.inc", "firmware/platform/ti_znp/nv_index_transaction.h",
        "tests/test_nv_index.c"]]
    inputs += [generated / "tlsf.c", generated / "tlsf.h"]
    inputs += [ROOT / name for name in ["firmware/src/autobaud.c", "firmware/src/autobaud.h",
        "firmware/platform/ti_znp/port_autobaud.c", "firmware/platform/ti_znp/port_autobaud.h",
        "firmware/platform/ti_znp/port_baud_capture.c", "firmware/platform/ti_znp/port_baud_capture.h",
        "tests/test_autobaud.c", "tests/test_npi_uart.c"]]
    inputs.append(ROOT / "tools/test_arm_autobaud.py")
    inputs += [ROOT / name for name in ["firmware/platform/ti_znp/port_broadcast.c", "firmware/platform/ti_znp/port_broadcast.h", "tests/test_broadcast.c"]]
    inputs += sorted((ROOT / "tests/broadcast_stubs").glob("*.h"))
    inputs += [ROOT / "firmware/platform/ti_znp/task_queue_ti.inc", ROOT / "tests/test_task_queue.c", generated / "osal_queue_under_test.inc"]
    inputs.append(generated / "npi_uart_under_test.inc")
    inputs.append(generated / "zmac_datareq_under_test.inc")
    inputs.append(generated / "zmac_rx_guard_under_test.inc")
    inputs += [ROOT / "tools/security_overlays.py", ROOT / "tests/test_security_persistence.c", generated / "security_under_test.inc"]
    inputs += sorted((ROOT / "tests/ti_stubs").rglob("*.h"))
    inputs.append(generated / "overlays.json")
    report = {"passed": True, "sdk_version":args.sdk_version,"hardware_tested": False, "backend": "simulated NOR and TI API peripheral mocks",
              "nvocmp": nv_output.strip(), "npi_adapter": npi_output.strip(), **extra,
              "input_sha256": {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}}
    (ROOT / ('build/coordinator-optimization-tests'+suffix+'.json')).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

if __name__ == "__main__":
    main()
