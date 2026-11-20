import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import image
from package_web_flasher import package, APP_END


class WebFlasherPackageTests(unittest.TestCase):
    def archive(self, folder, overlap=False, wrong_manifest=False, different_ccfg=False):
        memory = {address: 255 for address in range(image.CCFG, image.FLASH_END)}
        for address, value in [(0, 0x20010000), (4, 0x141), (0x57fd8, 0xc5fe0fc5), (0x57fe4, 0xffc5ffff), (0x57fec, 0)]:
            memory.update({address + i: b for i, b in enumerate(value.to_bytes(4, 'little'))})
        memory[0x140] = 0x70
        memory.update(dict(enumerate(b'ZBDP-FRT', 0x100)))
        for address, value in [(0x108, 1), (0x10c, 1), (0x114, 20261119)]:
            memory.update({address + i: b for i, b in enumerate(value.to_bytes(4, 'little'))})
        if overlap:
            memory[APP_END] = 1
        archive = folder / 'ci.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            for name in ('PROJECT-LICENSE.txt', 'FreeRTOS-LICENSE.md', 'TI-LICENSE.txt', 'THIRD_PARTY_NOTICES.txt'):
                z.writestr('sonoff-freertos/licenses/' + name, 'License fixture: ' + name)
            for baud in (115200, 1000000):
                memory.update({0x110 + i: b for i, b in enumerate(baud.to_bytes(4, 'little'))})
                if different_ccfg and baud == 1000000:
                    memory[image.CCFG] ^= 1
                path = folder / 'image.hex'
                image.write_hex(memory, path)
                raw = path.read_bytes()
                manifest = dict(uart_baud=baud, build_revision=20261119,
                                hex_sha256='0' * 64 if wrong_manifest else hashlib.sha256(raw).hexdigest(),
                                validation=dict(compiled_and_linked=True, static_image_checks=True))
                prefix = f'sonoff-freertos/diag-{baud}/'
                z.writestr(prefix + 'sonoff-freertos-diag.hex', raw)
                z.writestr(prefix + 'manifest.json', json.dumps(manifest))
        archive.with_suffix('.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ci.zip\n')
        return archive

    def test_site_contains_only_application_binaries_and_both_verified_variants(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            output = folder / 'site'
            data = package(self.archive(folder), output, 'a' * 40)
            self.assertEqual(len(data['images']), 2)
            for firmware in data['images']:
                binary = (output / firmware['path']).read_bytes()
                self.assertEqual(len(binary), APP_END)
                self.assertEqual(hashlib.sha256(binary).hexdigest(), firmware['sha256'])
                self.assertEqual(binary[0x140], 0x70)
                self.assertEqual(binary[0x100:0x108], b'ZBDP-FRT')
                self.assertEqual(binary[-1], 255)
            self.assertTrue((output / 'index.html').is_file())
            self.assertIn('THIRD_PARTY_NOTICES.txt', (output / 'FIRMWARE-LICENSES.txt').read_text())
            self.assertEqual(data['tag'], 'freertos-' + 'a' * 12)

    def test_rejects_changed_archive_before_writing_site(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            archive = self.archive(folder)
            archive.write_bytes(archive.read_bytes() + b'changed')
            with self.assertRaisesRegex(ValueError, 'SHA-256'):
                package(archive, folder / 'site', 'a' * 40)
            self.assertFalse((folder / 'site').exists())

    def test_rejects_preserved_storage_overlap_or_mismatched_manifest(self):
        for option in ('overlap', 'wrong_manifest', 'different_ccfg'):
            with self.subTest(option=option), tempfile.TemporaryDirectory() as temporary:
                folder = Path(temporary)
                with self.assertRaises(ValueError):
                    package(self.archive(folder, **{option: True}), folder / 'site', 'a' * 40)
                self.assertFalse((folder / 'site').exists())

    def test_requires_full_commit_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            with self.assertRaises(ValueError):
                package(self.archive(folder), folder / 'site', 'short')
