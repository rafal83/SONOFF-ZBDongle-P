"""Reject sensitive files, retired reverse-engineering tools and missing notices.

This checks the current source tree and release contents, not Git history and
not legal ownership or patent rights. Findings never print credential values.
"""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_ROOTS = {'private', 'upstream', 'vendor-sdk', '.tooling', '.cache', 'build', 'lab-data'}
RETIRED = {
    'tools/decompile_cc2652p.py',
    'tools/unpack_slzb.py', 'tools/install_ghidra.py', 'tools/core_integration.py',
    'tools/build_coordinator.py', 'tools/audit_core_image.py', 'tools/test_arm_core_integration.py',
    'tools/finalize_core_candidate.py',
}
PRIVATE_NAMES = {'configuration.yaml', 'configuration.yml', 'coordinator_backup.json', 'database.db'}
BINARY_SUFFIXES = {'.a', '.lib', '.o', '.obj', '.elf', '.out', '.hex', '.exe', '.dll', '.so', '.slzb', '.gpr'}
SECRETS = {
    'private key': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    'GitHub token': re.compile(r'\b(?:ghp_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b'),
    'AWS access key': re.compile(r'\bAKIA[A-Z0-9]{16}\b'),
}
REQUIRED_LICENSES = ['LICENSE', 'NOTICE.md', 'LICENSES/Koenkk-MIT.txt',
                     'LICENSES/zigbee-herdsman-MIT.txt', 'LICENSES/TI-BSD-3-Clause.txt']


def check_source(name, data):
    path = PurePosixPath(name)
    problems = []
    if path.is_absolute() or '..' in path.parts or not path.parts:
        problems.append('unsafe path')
    elif path.parts[0] in EXCLUDED_ROOTS:
        problems.append('private or generated directory')
    if name in RETIRED or name.startswith('tools/ghidra/') or name.startswith('firmware/platform/ti_znp/port_core_'):
        problems.append('retired binary-analysis or binary-rewriting component')
    if path.name.lower() in PRIVATE_NAMES or path.suffix.lower() in {'.pem', '.key', '.nvram', '.dump'}:
        problems.append('backup, configuration or credential file')
    if path.suffix.lower() in BINARY_SUFFIXES or (path.suffix.lower() == '.bin' and name != 'tests/fixtures/freertos_store_sim.bin'):
        problems.append('binary artifact in source distribution')
    text = data.decode('utf-8', errors='replace')
    for kind, pattern in SECRETS.items():
        if pattern.search(text):
            problems.append(kind)
    return [f'{name}: {problem}' for problem in problems]


def audit_sources(root=ROOT):
    # Include new source files; omit ignored local backups and generated assets.
    output = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=root)
    names = set(output.decode().split('\0')) - {''}
    findings = []
    checked = 0
    for name in sorted(names):
        path = root / name
        if not path.is_file():
            continue  # files deliberately removed from the current working tree
        findings.extend(check_source(name, path.read_bytes()))
        checked += 1
    for name in REQUIRED_LICENSES:
        if not (root / name).is_file():
            findings.append(f'{name}: required licence or attribution missing')
    lock = json.loads((root / 'sources.lock.json').read_text(encoding='utf-8'))
    supported = {'FreeRTOS-Kernel', 'Z-Stack-firmware', 'zigbee-herdsman', 'zigbee2mqtt',
                 'ti-sdk', 'cc2538-bsl', 'tlsf'}
    if lock.keys() - supported:
        findings.append('sources.lock.json: dependency outside the supported source list')
    return {'checked_source_files': checked, 'findings': findings}


def audit_sdk(root=ROOT):
    # Compile only sources supplied under a permissive BSD-style licence.
    from build_freertos import TI_SOURCES, SDK
    sources = [*TI_SOURCES, SDK / 'ti/devices/DeviceFamily.h',
               SDK / 'ti/devices/cc13x2_cc26x2/startup_files/ccfg.c']
    findings = []
    for path in sources:
        if not path.is_file():
            findings.append(f'{path.name}: TI source missing')
            continue
        text = path.read_text(encoding='utf-8')
        notice = text[:text.find('*/') + 2]
        if not all(word in notice for word in ('Copyright', 'Redistribution', 'binary', 'disclaimer')):
            findings.append(f'{path.name}: expected TI source licence notice missing')
    return {'checked_ti_sources': len(sources), 'findings': findings}


def audit_archive(archive):
    findings = []
    required = {'sonoff-freertos/licenses/' + name for name in (
        'PROJECT-LICENSE.txt', 'FreeRTOS-LICENSE.md', 'TI-LICENSE.txt', 'TI-SDK-MANIFEST.html',
        'THIRD_PARTY_NOTICES.txt', 'Koenkk-MIT.txt', 'zigbee-herdsman-MIT.txt', 'TI-BSD-3-Clause.txt', 'ATTRIBUTIONS.md')}
    allowed = {
        'README.txt', 'host/host-tests.json', 'arm-test/arm-tests.json', 'arm-test/freertos-portable-test-only.elf',
        *[f'diag-{baud}/{name}' for baud in (115200, 1000000)
          for name in ('sonoff-freertos-diag.elf', 'sonoff-freertos-diag.hex', 'manifest.json')],
    }
    with zipfile.ZipFile(archive) as z:
        if z.testzip() is not None:
            findings.append('archive: CRC failure')
        names = set(z.namelist())
        for name in sorted(required - names):
            findings.append(f'{name}: licence missing from release')
        for member in z.infolist():
            name = member.filename
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts or not name.startswith('sonoff-freertos/'):
                findings.append(f'{name}: unsafe release path')
                continue
            relative = name[len('sonoff-freertos/'):]
            permitted = (relative in allowed or (relative.startswith('docs/') and path.suffix == '.md')
                         or name in required)
            if not permitted:
                findings.append(f'{name}: unexpected release member')
            if member.external_attr >> 16 & 0o170000 == 0o120000:
                findings.append(f'{name}: symbolic link in release')
            if path.suffix in ('.md', '.txt', '.json'):
                text = z.read(member).decode('utf-8', errors='replace')
                for kind, pattern in SECRETS.items():
                    if pattern.search(text):
                        findings.append(f'{name}: {kind}')
    return {'checked_release_members': len(names), 'findings': findings}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-sdk', action='store_true')
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    reports = [audit_sources()]
    if args.check_sdk:
        reports.append(audit_sdk())
    if args.archive:
        reports.append(audit_archive(args.archive))
    print(json.dumps(reports, indent=2))
    raise SystemExit(bool(any(report['findings'] for report in reports)))
