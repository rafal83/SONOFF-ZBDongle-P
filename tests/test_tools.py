import importlib.util
from pathlib import Path
import sys
import tempfile
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

image = module("image")
prepare = module("prepare_znp")
benchmark = module("benchmark")
coordinator = module("audit_coordinator")

def fixture():
    memory = {address: 255 for address in range(image.CCFG, image.FLASH_END)}
    def put(address, value):
        memory.update({address + i: b for i, b in enumerate(value.to_bytes(4, "little"))})
    put(0, 0x20014000)
    put(4, 0x101)
    memory[0x100] = 0x70
    put(0x57fd8, 0xc5fe0fc5)
    put(0x57fe4, 0xffc5ffff)
    put(0x57fec, 0)
    return memory

class ImageTests(unittest.TestCase):
    def test_allocated_ram_sections_include_nobits(self):
        def elf_ram(sections):
            raw = bytearray(52 + 40 * len(sections))
            struct.pack_into("<I", raw, 32, 52)
            struct.pack_into("<HH", raw, 46, 40, len(sections))
            for i, (address, size) in enumerate(sections):
                struct.pack_into("<10I", raw, 52 + i * 40, 0, 8, 3, address, 0, size, 0, 0, 4, 0)
            return raw
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ram.elf"
            valid = [(0x2000010c, 16), (0x20001830, 155), (0x20003000, 256), (0x20013000, 4096)]
            path.write_bytes(elf_ram(valid))
            self.assertEqual(coordinator.audit_ram_sections(path, 0x20004000, 0x20013000, True), 4)
            for extra in [(0x20005000, 16), (0x20001700, 16), (0x20001840, 16), (0x20013ff0, 32)]:
                path.write_bytes(elf_ram(valid + [extra]))
                with self.assertRaises(ValueError):
                    coordinator.audit_ram_sections(path, 0x20004000, 0x20013000, True)

    def test_good_recovery_profile(self):
        self.assertTrue(image.check(fixture())["passed"])

    def test_bootloader_and_backdoor_fail_closed(self):
        for address, value in ((0x57fd8, 0), (0x57fdb, 0), (0x57fda, 1), (0x57fd9, 0), (0x57fe6, 0)):
            memory = fixture()
            memory[address] = value
            self.assertFalse(image.check(memory)["passed"], hex(address))

    def test_nv_and_ieee_rejected(self):
        memory = fixture()
        memory[0x50000] = 0
        self.assertFalse(image.check(memory)["passed"])
        memory = fixture()
        memory[0x57fc8] = 0
        self.assertFalse(image.check(memory)["passed"])

    def test_missing_ccfg_rejected(self):
        memory = fixture()
        del memory[0x57ff0]
        with self.assertRaises(ValueError):
            image.check(memory)

    def test_hex_sparse_roundtrip(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.hex"
            image.write_hex(fixture(), path)
            self.assertEqual(image.read_hex(path), fixture())

    def test_malformed_hex_rejected(self):
        cases = [":00000001FE\n", ":00000001FF\n:00000001FF\n", "",
                 image.record(0, 4, b"\x00\x06") + "\n" + image.record(0, 0, [1]) + "\n:00000001FF\n",
                 image.record(0, 0, [1]) + "\n" + image.record(0, 0, [1]) + "\n:00000001FF\n"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.hex"
            for case in cases:
                path.write_text(case)
                with self.assertRaises(ValueError):
                    image.read_hex(path)

    def test_real_arm_image_when_available(self):
        elf = ROOT / "build/sonoff-uart-diagnostic.elf"
        if not elf.exists():
            self.skipTest("Build diagnostic first")
        memory = image.read_elf(elf)
        self.assertTrue(image.check(memory)["passed"])
        self.assertEqual(memory, image.read_hex(elf.with_suffix(".hex")))

    def test_patch_selection(self):
        raw = (ROOT / "upstream/Z-Stack-firmware/coordinator/Z-Stack_3.x.0/firmware.patch").read_text(encoding="utf-8")
        patch = prepare.extract_patch(raw)
        self.assertIn("source/preinclude.h", patch)
        self.assertIn("znp_CC1352P_2_LAUNCHXL_tirtos7_ticlang/znp.syscfg", patch)
        self.assertNotIn("diff --git a/workspace/zr_", patch)
        self.assertNotIn("diff --git a/workspace/znp_LP_", patch)

    def test_real_coordinator_when_available(self):
        folder = ROOT / "build/coordinator"
        if not (folder / "sonoff-znp-coordinator.hex").is_file():
            self.skipTest("Build the full coordinator first")
        result = coordinator.audit(folder / "sonoff-znp-coordinator.hex", folder / "sonoff-znp-coordinator.out",
                                   folder / "sonoff-znp-coordinator.map")
        self.assertTrue(result["elf_hex_sections_match"])
        self.assertEqual(result["znp_version"]["product"], 1)
        self.assertEqual(result["znp_version"]["revision"], 20250321)

    def test_real_optimized_coordinator_when_available(self):
        folder = ROOT / "build/coordinator-optimized"
        if not (folder / "sonoff-znp-coordinator.hex").is_file():
            self.skipTest("Build the optimized coordinator first")
        result = coordinator.audit(folder / "sonoff-znp-coordinator.hex", folder / "sonoff-znp-coordinator.out",
                                   folder / "sonoff-znp-coordinator.map", revision=20261114, optimized=True)
        self.assertTrue(result["elf_hex_sections_match"])
        self.assertGreaterEqual(result["runtime_heap_span_bytes"], 32768)
        self.assertGreaterEqual(result["heap_capacity_before_os_allocations"], 32768)

    def test_benchmark_counts_failures(self):
        result = benchmark.summarize([
            {"status": "ok", "latency_ms": "10"},
            {"status": "ok", "latency_ms": "20"},
            {"status": "timeout", "latency_ms": ""}], 10)
        self.assertEqual(result["error_rate"], 1 / 3)
        self.assertEqual(result["success_per_second"], .2)
        self.assertEqual(result["latency_ms"]["p95_nearest_rank"], 20)

    def test_benchmark_empty_or_invalid(self):
        self.assertIsNone(benchmark.summarize([], 10)["latency_ms"]["median"])
        with self.assertRaises(ValueError):
            benchmark.summarize([], 0)
        with self.assertRaises(ValueError):
            benchmark.summarize([{"status": "ok", "latency_ms": "nan"}], 1)

if __name__ == "__main__":
    unittest.main()
