"""FreeRTOS store parser and the guard of the Z-Stack byte restore.

The fixture is a store written by the real C code (znp_sim, herdsman
acceptance run): a network record and a v2 counters record."""
from pathlib import Path
import struct
import sys
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import freertos_store  # noqa: E402

FIXTURE = (ROOT / "tests/fixtures/freertos_store_sim.bin").read_bytes()
FLASH_SIZE = 0x58000


def full_image(store):
    flash = bytearray(b"\xff" * FLASH_SIZE)
    flash[freertos_store.STORE_BASE:freertos_store.STORE_BASE + len(store)] = store
    return bytes(flash)


class StoreParser(unittest.TestCase):
    def test_reads_the_c_written_store(self):
        recs = freertos_store.records(full_image(FIXTURE))
        self.assertTrue(freertos_store.has_network(recs))
        nwk, aps = freertos_store.counters(recs)
        self.assertGreater(nwk, 0)
        self.assertGreaterEqual(aps, 0)

    def test_blank_store_has_nothing(self):
        self.assertIsNone(freertos_store.records(b"\xff" * FLASH_SIZE))

    def test_corrupted_record_is_ignored(self):
        recs = freertos_store.records(full_image(FIXTURE))
        good = freertos_store.counters(recs)
        store = bytearray(FIXTURE)
        # Flip one bit in the payload of the LAST counters record: its CRC fails.
        offsets = []
        for bank in (0, 1):
            base = bank * freertos_store.BANK_SIZE
            off = 16
            while off + 8 <= freertos_store.BANK_SIZE:
                h = bytes(store[base + off: base + off + 8])
                if h == b"\xff" * 8:
                    break
                rid, ln, _ = struct.unpack_from("<HHH", h)
                plen = ln & 0x7FFF
                if rid == freertos_store.ID_COUNTERS:
                    offsets.append(base + off)
                off += 8 + ((plen + 3) & ~3) + 4
        store[offsets[-1] + 8 + 2] ^= 0x01
        recs = freertos_store.records(full_image(bytes(store)))
        self.assertNotEqual(freertos_store.counters(recs), good) if len(offsets) > 1 else \
            self.assertIsNone(freertos_store.counters(recs))


class RestoreGuard(unittest.TestCase):
    def setUp(self):
        sys.argv = ["x"]
        import freertos_lab_session
        self.lab = freertos_lab_session

    def test_refuses_byte_restore_over_a_freertos_network(self):
        args = types.SimpleNamespace(accept_counter_regression=False)
        report = {}
        with self.assertRaises(RuntimeError):
            self.lab.rollback_guard(full_image(FIXTURE), args, report)
        self.assertTrue(report["freertos_store_network"])

    def test_laboratory_override_and_empty_store(self):
        report = {}
        self.lab.rollback_guard(full_image(FIXTURE), types.SimpleNamespace(accept_counter_regression=True), report)
        self.lab.rollback_guard(b"\xff" * FLASH_SIZE, types.SimpleNamespace(accept_counter_regression=False), {})


if __name__ == "__main__":
    unittest.main()
