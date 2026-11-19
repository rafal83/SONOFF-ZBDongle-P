import importlib.util
from pathlib import Path
import sys
import unittest
import binascii
import hashlib
import json
import tempfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from capture_dongle import read_words
from dongle_probe import encode, extract
from run_dongle_session import program_main, boot, FLASH_SIZE, load_radio_nv

class FakeROM:
    def __init__(self, length): self.length = length; self.wire = None
    def _write(self, data): self.wire = data
    def _wait_for_ack(self, *args): return True
    def receivePacket(self): return bytes(self.length)
    def checkLastCmd(self): return True

class CaptureTests(unittest.TestCase):
    def test_radio_resume_rejects_production_and_changed_code(self):
        # Synthetic laboratory state only; no existing backup or port is read.
        (ROOT / "private").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / "private") as temporary:
            folder = Path(temporary)
            reference = folder / "experimental.hex"
            reference.write_text(":0400000001020304F2\n:00000001FF\n")
            data = bytearray([255]) * FLASH_SIZE
            data[:4] = bytes([1, 2, 3, 4]); data[0x50000:0x56000] = bytes([0x11]) * 0x6000
            original = bytearray(data); original[0x50000:0x56000] = bytes([0x22]) * 0x6000
            (folder / "original-a.bin").write_bytes(original)
            report = dict(experimental_hex_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
                production_network_loaded_into_experimental=False, radio={"adapter_start": True})
            def save():
                (folder / "experimental-final.bin").write_bytes(data)
                report["experimental_snapshot_sha256"] = hashlib.sha256(data).hexdigest()
                (folder / "hardware-session.json").write_text(json.dumps(report))
            save(); self.assertEqual(load_radio_nv(folder), bytes([0x11]) * 0x6000)
            report["production_network_loaded_into_experimental"] = True; save()
            with self.assertRaises(ValueError): load_radio_nv(folder)
            report["production_network_loaded_into_experimental"] = False
            data[0x50000:0x56000] = original[0x50000:0x56000]; save()
            with self.assertRaises(ValueError): load_radio_nv(folder)
            data[0x50000:0x56000] = bytes([0x11]) * 0x6000
            data[0x100] = 0; save()
            with self.assertRaises(ValueError): load_radio_nv(folder)
    def test_ROM_reset_before_modem_lines_change(self):
        events = []
        class Pins:
            def __setattr__(self, name, value): events.append((name, value))
        class Command:
            sp = Pins()
            def cmdReset(self):
                self_test.assertEqual(events, [])
                events.append("ROM_RESET"); return True
        self_test = self
        boot(Command())
        self.assertEqual(events, ["ROM_RESET", ("dtr", False), ("rts", False)])

    def test_program_refuses_unrecoverable_or_partial_images(self):
        class Device:
            erases = 0
            def erase(self): self.erases += 1; return True
        class Command:
            def writeMemory(self, address, data): self.data = data; return address == 0
            def cmdCRC32CC26xx(self, address, length): return binascii.crc32(self.data) & 0xffffffff
        device, command = Device(), Command()
        data = bytearray([255]) * FLASH_SIZE
        with self.assertRaises(ValueError): program_main(command, device, data[:-1])
        with self.assertRaises(ValueError): program_main(command, device, data)
        self.assertEqual(device.erases, 0)
        data[0x57fd8:0x57fdc] = bytes.fromhex("c50ffec5")
        self.assertEqual(program_main(command, device, data), f"{binascii.crc32(data) & 0xffffffff:08x}")
        self.assertEqual(device.erases, 1)

    def test_bounded_read_packet_only(self):
        fake = FakeROM(252)
        self.assertEqual(len(read_words(fake, 0x100, 63)), 252)
        self.assertEqual(fake.wire[2:], bytes.fromhex("2a00000100013f"))
        self.assertEqual(sum(fake.wire[2:]) & 255, fake.wire[1])
        for address, count in [(0, 0), (0, 64), (1, 1), (-4, 1), (0x58000, 1), (0x57ffc, 2)]:
            with self.assertRaises(ValueError): read_words(fake, address, count)
        with self.assertRaises(IOError): read_words(FakeROM(4), 0, 2)

    def test_MT_fragments_and_bad_fcs(self):
        good = encode(0x61, 1, b"\x59\x06")
        data = bytearray(b"noise" + good[:-1])
        self.assertEqual(extract(data), [])
        data.extend(good[-1:])
        self.assertEqual(extract(data), [(0x61, 1, b"\x59\x06")])
        data.extend(good[:-1] + b"\xff" + good)
        self.assertEqual(extract(data), [(0x61, 1, b"\x59\x06")])
