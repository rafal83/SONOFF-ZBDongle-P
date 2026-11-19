import binascii
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from migrate_production import FLASH_SIZE, NV_START, NV_END, CCFG, with_current_nv, same_program, program_application

def image(nv):
    data = bytearray(b"\xff" * FLASH_SIZE)
    for address, value in [(0, 0x20014000), (4, 0x101), (0x57fd8, 0xc5fe0fc5),
                           (0x57fe4, 0xffc5ffff), (0x57fec, 0)]:
        struct.pack_into("<I", data, address, value)
    data[0x100] = 0x70
    data[NV_START:NV_END] = bytes([nv]) * (NV_END - NV_START)
    return data

class ProductionMigrationTests(unittest.TestCase):
    def test_current_NV_and_CCFG_are_preserved_in_both_directions(self):
        old, new = image(0x11), image(0x22)
        new[0x104] = 0x73
        installed = with_current_nv(new, old)
        self.assertEqual(installed[NV_START:], old[NV_START:])
        self.assertEqual(installed[:NV_START], new[:NV_START])
        installed = bytearray(installed)
        installed[NV_START:NV_END] = b"\x33" * (NV_END - NV_START)
        restored = with_current_nv(old, installed)
        self.assertEqual(restored[NV_START:NV_END], installed[NV_START:NV_END])
        self.assertNotEqual(restored[NV_START:NV_END], old[NV_START:NV_END])
        self.assertEqual(restored[:NV_START], old[:NV_START])

    def test_empty_partial_or_unrecoverable_images_are_rejected(self):
        for current in [image(0xff), image(1)[:-1]]:
            with self.assertRaises(ValueError): with_current_nv(image(2), current)
        invalid = image(1); invalid[0x57fd8:0x57fdc] = b"\x00" * 4
        with self.assertRaises(ValueError): with_current_nv(image(2), invalid)

    def test_code_identity_allows_only_NV_evolution(self):
        a, b = image(1), image(2)
        self.assertTrue(same_program(a, b))
        for address in [0x100, NV_END, CCFG]:
            altered = bytearray(b); altered[address] ^= 1
            self.assertFalse(same_program(a, altered))

    def test_flash_never_erases_or_programs_NV_or_CCFG(self):
        current = image(1); target = with_current_nv(image(2), current)
        events = []
        class Device:
            page_size = 8192
            def eraseRange(self, address, length): events.append(("erase", address, length)); return True
        class Command:
            def writeMemory(self, address, data): events.append(("write", address, len(data))); return True
            def cmdCRC32CC26xx(self, address, length): return binascii.crc32(target) & 0xffffffff
        program_application(Command(), Device(), target, current)
        self.assertEqual(events, [("erase", 0, NV_START), ("write", 0, NV_START)])
        for address in [NV_START, NV_END, CCFG]:
            altered = bytearray(target); altered[address] ^= 1
            with self.assertRaises(ValueError): program_application(Command(), Device(), altered, current)
        self.assertEqual(len(events), 2)

if __name__ == "__main__": unittest.main()
