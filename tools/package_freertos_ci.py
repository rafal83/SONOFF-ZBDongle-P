"""Package verified FreeRTOS build outputs with their reports and notices."""
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/freertos'
SDK = ROOT / 'vendor-sdk/simplelink_cc13xx_cc26xx_sdk_8_33_00_16'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    members = {}
    ti_sources = set()
    manifests = []
    for baud in [115200, 1000000]:
        folder = BUILD / f'diag-{baud}'
        manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8'))
        assert manifest['uart_baud'] == baud
        assert manifest['validation']['compiled_and_linked']
        assert manifest['validation']['static_image_checks']
        manifests.append(manifest)
        for suffix in ['hex', 'elf']:
            name = f'sonoff-freertos-diag.{suffix}'
            data = (folder / name).read_bytes()
            assert sha(data) == manifest[f'{suffix}_sha256'], f'Changed {name}'
            members[f'diag-{baud}/{name}'] = data
        members[f'diag-{baud}/manifest.json'] = (folder / 'manifest.json').read_bytes()
        ti_sources.update(manifest['ti_sources'])
    host = json.loads((BUILD / 'host/host-tests.json').read_text(encoding='utf-8'))
    assert host['tests'] and all(r['status'] == 'pass' for r in host['tests'].values())
    for manifest in manifests:
        for name, digest in manifest['sources_sha256'].items():
            assert sha((ROOT / name).read_bytes()) == digest, f'Changed source: {name}'
            if name in host['sources_sha256']:
                assert host['sources_sha256'][name] == digest, f'Stale host test: {name}'
    arm = json.loads((BUILD / 'arm-test/arm-tests.json').read_text(encoding='utf-8'))
    assert arm['checks'] > 0
    test_elf = BUILD / 'arm-test/freertos-portable-test-only.elf'
    assert sha(test_elf.read_bytes()) == arm['elf_sha256'], 'Changed ARM test ELF'
    members['arm-test/freertos-portable-test-only.elf'] = test_elf.read_bytes()
    for name in ['host/host-tests.json', 'arm-test/arm-tests.json']:
        members[name] = (BUILD / name).read_bytes()
    for source, name in [
        (ROOT / 'LICENSE', 'licenses/PROJECT-LICENSE.txt'),
        (ROOT / 'NOTICE.md', 'licenses/ATTRIBUTIONS.md'),
        (ROOT / 'upstream/FreeRTOS-Kernel/LICENSE.md', 'licenses/FreeRTOS-LICENSE.md'),
        (SDK / 'license_simplelink_cc13xx_cc26xx_sdk_8_33_00_16.txt', 'licenses/TI-LICENSE.txt'),
        (SDK / 'manifest_simplelink_cc13xx_cc26xx_sdk_8_33_00_16.html', 'licenses/TI-SDK-MANIFEST.html'),
    ]:
        members[name] = source.read_bytes()
    notices = [(ROOT / 'NOTICE.md').read_text(encoding='utf-8')]
    for name in ('Koenkk-MIT.txt', 'zigbee-herdsman-MIT.txt', 'TI-BSD-3-Clause.txt'):
        data = (ROOT / 'LICENSES' / name).read_bytes()
        members['licenses/' + name] = data
        notices.append(name + '\n' + data.decode('utf-8'))
    notice_sources = ti_sources | {
        'ti/devices/DeviceFamily.h',
        'ti/devices/cc13x2_cc26x2/startup_files/ccfg.c',
    }
    # TI inline helpers and register definitions may be compiled from headers.
    # Carry their exact copyright/licence notices with the binary as well.
    device = SDK / 'source/ti/devices/cc13x2_cc26x2'
    for header in device.rglob('*.h'):
        text = header.read_text(encoding='utf-8')
        notice = text[:text.find('*/') + 2]
        if 'Copyright' in notice and 'Redistribution' in notice:
            notice_sources.add(header.relative_to(SDK / 'source').as_posix())
    for relative in sorted(notice_sources):
        source = (SDK / 'source' / relative).resolve()
        assert source.is_relative_to((SDK / 'source').resolve())
        text = source.read_text(encoding='utf-8')
        end = text.find('*/')
        assert end >= 0, f'TI notice missing: {relative}'
        notices.append(relative + '\n' + text[:end + 2])
    members['licenses/THIRD_PARTY_NOTICES.txt'] = ('\n\n'.join(notices) + '\n').encode('utf-8')
    for source in sorted((ROOT / 'docs/freertos-migration').glob('*.md')):
        members['docs/' + source.name] = source.read_bytes()
    members['README.txt'] = (
        'SONOFF ZBDongle-P CC2652P - experimental FreeRTOS build\n'
        'Separate images: diag-115200/ and diag-1000000/.\n'
        'Compilation and host/ARM tests do not qualify the build on hardware.\n'
        'Read each manifest for its actual validation and compatibility limits.\n'
        'Preserve the included TI, FreeRTOS and project licenses.\n'
    ).encode('utf-8')
    archive = BUILD / 'sonoff-freertos-experimental.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(members.items()):
            z.writestr('sonoff-freertos/' + name, data)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
    from audit_distribution import audit_archive
    audit = audit_archive(archive)
    if audit['findings']:
        raise ValueError('Distribution audit failed: ' + '; '.join(audit['findings']))
    archive.with_suffix('.sha256').write_text(sha(archive.read_bytes()) + '  ' + archive.name + '\n', encoding='ascii')
    print(json.dumps({'package': str(archive.relative_to(ROOT)), 'members': len(members), 'verified': True}))


if __name__ == '__main__':
    main()
