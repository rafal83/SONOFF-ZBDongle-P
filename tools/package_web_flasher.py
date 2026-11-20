"""Build a static, same-origin Web Serial flasher from the verified CI archive."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
import zipfile

from image import FLASH_END, CCFG, check, read_hex

ROOT = Path(__file__).resolve().parents[1]
APP_END = 0x34000   # storage layout 2: the nv_log store starts here


def package(archive, output, commit):
    archive, output = Path(archive), Path(output)
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('A full source commit SHA is required')
    expected = archive.with_suffix('.sha256').read_text(encoding='ascii').split()
    if len(expected) != 2 or expected[1] != archive.name or hashlib.sha256(archive.read_bytes()).hexdigest() != expected[0]:
        raise ValueError('CI archive SHA-256 mismatch')
    images, configuration = [], None
    # Validate both images before producing deployable output.
    with zipfile.ZipFile(archive) as z, tempfile.TemporaryDirectory() as temporary:
        if z.testzip() is not None:
            raise ValueError('CI archive CRC mismatch')
        licenses = []
        for name in ('PROJECT-LICENSE.txt', 'FreeRTOS-LICENSE.md', 'TI-LICENSE.txt', 'THIRD_PARTY_NOTICES.txt'):
            licenses.append(name.encode('ascii') + b'\n\n' + z.read('sonoff-freertos/licenses/' + name))
        for baud in (115200, 1000000):
            prefix = f'sonoff-freertos/diag-{baud}/'
            manifest = json.loads(z.read(prefix + 'manifest.json'))
            if manifest.get('hardware_aes') is not True:
                raise ValueError('Only hardware-accelerated firmware may be published')
            raw = z.read(prefix + 'sonoff-freertos-diag.hex')
            if hashlib.sha256(raw).hexdigest() != manifest['hex_sha256'] or manifest['uart_baud'] != baud:
                raise ValueError('Firmware manifest mismatch')
            if not (manifest['validation']['compiled_and_linked'] and manifest['validation']['static_image_checks']):
                raise ValueError('Firmware has not passed its image checks')
            path = Path(temporary) / f'{baud}.hex'
            path.write_bytes(raw)
            memory = read_hex(path)
            if not check(memory)['passed'] or any(APP_END <= a < CCFG for a in memory):
                raise ValueError('Firmware overlaps preserved storage or has invalid recovery settings')
            ccfg = bytes(memory[a] for a in range(CCFG, FLASH_END))
            if configuration is not None and configuration != ccfg:
                raise ValueError('The firmware variants have different CCFG')
            configuration = ccfg
            binary = bytearray(b'\xff' * APP_END)
            for address, byte in memory.items():
                if address < APP_END:
                    binary[address] = byte
            identity = binary[0x100:0x118]
            if (identity[:8] != b'ZBDP-FRT' or
                    tuple(int.from_bytes(identity[a:a + 4], 'little') for a in (8, 12, 16, 20)) !=
                    (1, 2, baud, manifest['build_revision'])):
                raise ValueError('Invalid FreeRTOS application identity / storage layout')
            images.append((baud, bytes(binary), raw, manifest))
    output.mkdir(parents=True, exist_ok=True)
    (output / 'firmware').mkdir(exist_ok=True)
    for name in ('index.html', 'styles.css', 'app.mjs', 'rom.mjs'):
        (output / name).write_bytes((ROOT / 'web-flasher' / name).read_bytes())
    (output / '.nojekyll').write_text('', encoding='ascii')
    (output / 'LICENSE.txt').write_bytes((ROOT / 'LICENSE').read_bytes())
    (output / 'FIRMWARE-LICENSES.txt').write_bytes(b'\n\n'.join(licenses) + b'\n')
    tag = f'freertos-{commit[:12]}'
    catalog = dict(schema=1, board='SONOFF ZBDongle-P', chip='CC2652P', commit=commit, tag=tag,
                   release=f'https://github.com/rafal83/SONOFF-ZBDongle-P/releases/tag/{tag}',
                   ccfg=list(configuration), images=[])
    for baud, binary, raw, manifest in images:
        name = f'firmware/sonoff-freertos-{baud}'
        (output / (name + '.bin')).write_bytes(binary)
        (output / (name + '.hex')).write_bytes(raw)
        catalog['images'].append(dict(baud=baud, path=name + '.bin', hex=name + '.hex',
                                      sha256=hashlib.sha256(binary).hexdigest(),
                                      hex_sha256=manifest['hex_sha256'], revision=manifest['build_revision']))
        catalog['images'][-1]['hardware_aes'] = manifest['hardware_aes']
    (output / 'firmware.json').write_text(json.dumps(catalog, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(site=str(output), commit=commit, images=len(images))))
    return catalog


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, default=ROOT / 'build/freertos/sonoff-freertos-experimental.zip')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/web-flasher')
    parser.add_argument('--commit', required=True)
    args = parser.parse_args()
    package(args.archive, args.output, args.commit)
