"""Authorized 20261114 <-> 20261117 application-only production trial.

Pins both images. Never erase/program NV or CCFG, form a network, or open
association. Failed upgrade falls back to 20261114 with the CURRENT NV.
"""
import argparse
import json
from pathlib import Path
from migrate_production import (ROOT, FLASH_SIZE, NV_START, NV_END, sha, same_program,
    with_current_nv, capture_twice, write_verified, network_snapshot)
from run_dongle_session import open_rom, boot
from image import check, read_hex

PROFILES = {
    20261114: ('coordinator-optimized-1000000', 'cb2b5e4f5cae02a5601c6c1c8d3cc9d7be7a009cfa2fb7398d323f39eb31dee5'),
    20261117: ('coordinator-optimized-1000000-next', '94a3717da4204523a601371a96b13b0a532c666c9a161f12adbd78e00d1bbf25'),
    2026111801: ('coordinator-optimized-1000000-cores-sdk833', 'b79a5308aa926aecf49b7464c829753242a16d49d4d7e7cddc697353e6c3581a'),
    2026111802: ('coordinator-optimized-1000000-cores-sdk833-r2', 'cb400551e38ca84491dc920b647cac1ed839c5b20fb34aed5290c243870b093b'),
}

def load_profile(revision):
    name, expected = PROFILES[revision]
    folder = ROOT / 'build' / name
    path = folder / 'sonoff-znp-coordinator.hex'
    manifest = json.loads((folder / 'manifest.json').read_text())
    if not (sha(path.read_bytes()) == expected == manifest['hex_sha256']
            and manifest['znp_version']['revision'] == revision and manifest['baudrate'] == 1000000
            and not manifest['auto_baud']):
        raise ValueError('Firmware pin/profile mismatch')
    memory = read_hex(path)
    if not check(memory)['passed']: raise ValueError('Image recovery check failed')
    image = bytearray(b'\xff' * FLASH_SIZE)
    for address, value in memory.items(): image[address] = value
    return bytes(image), manifest

def upgrade_target(current, expected_program, requested_program):
    if not same_program(current, expected_program): raise ValueError('Current application differs from the pinned source version')
    target = with_current_nv(requested_program, current)
    if target[NV_START:] != current[NV_START:]: raise ValueError('NV/CCFG/reserved sectors would change')
    return target

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=['upgrade', 'rollback'])
    ap.add_argument('--port', required=True)
    ap.add_argument('--usb-serial', required=True)
    ap.add_argument('--expect-ieee', required=True)
    ap.add_argument('--session-dir', required=True, type=Path)
    ap.add_argument('--z2m-stopped', required=True, action='store_true')
    args = ap.parse_args()
    folder = args.session_dir.resolve()
    if not folder.is_relative_to((ROOT / 'private').resolve()): raise ValueError('Session must remain private')
    data = json.loads((folder / 'z2m-snapshot.json').read_text())
    if data['ieee'].lower() != args.expect_ieee.lower() or data['configured_baudrate'] != 1000000:
        raise ValueError('Z2M backup identity or baud mismatch')
    for name, digest in data['sha256'].items():
        if sha((folder / 'z2m-data' / name).read_bytes()) != digest: raise ValueError('Z2M snapshot changed')
    stable, _ = load_profile(20261114)
    candidate, manifest = load_profile(20261117)
    proof_dir = ROOT / 'build' / PROFILES[20261117][0]
    native = json.loads((proof_dir / 'optimization-tests.json').read_text())
    if not native['passed'] or 'nine restore failure classes' not in native['security_persistence']:
        raise ValueError('Candidate software qualification missing')
    for name, digest in native['input_sha256'].items():
        if sha((ROOT / name).read_bytes()) != digest: raise ValueError('Candidate source inputs changed')
    for name in ['arm-radio-tests.json', 'arm-autobaud-tests.json']:
        proof = json.loads((proof_dir / name).read_text())
        if not proof['passed'] or proof['elf_sha256'] != manifest['elf_sha256']: raise ValueError('ARM proof mismatch')
    before = folder / 'network-before.json'
    baseline = json.loads(Path(str(before) + '.report.json').read_text())
    if not baseline['passed'] or baseline['database_devices_absent_from_backup']:
        raise ValueError('Fresh production backup is incomplete')
    source_revision, destination_revision = (20261114, 20261117) if args.action == 'upgrade' else (20261117, 20261114)
    source, destination = (stable, candidate) if args.action == 'upgrade' else (candidate, stable)
    work = folder / args.action
    work.mkdir()
    report = dict(action=args.action, passed=False, source_revision=source_revision,
        destination_revision=destination_revision, baudrate=1000000,
        source_hex_sha256=PROFILES[source_revision][1], destination_hex_sha256=PROFILES[destination_revision][1],
        NV_and_CCFG_not_erased_or_programmed=True, commissioning_commands_sent=False,
        Z2M_started=False, production_network_used=True)
    command = None; touched = False
    try:
        preflight = work / 'network-preflight.json'
        network_snapshot(args, folder, preflight, source_revision, 1000000, before)
        command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
        current = capture_twice(command, work, 'current')
        target = upgrade_target(current, source, destination)
        (work / 'target.bin').write_bytes(target)
        report['NV_bytes_identical_before_boot'] = target[NV_START:NV_END] == current[NV_START:NV_END]
        touched = True
        report['programmed'] = write_verified(command, device, target, current, work, 'target')
        boot(command); command.close(); command = None
        report['network_verification'] = network_snapshot(args, folder, work / 'network-after.json', destination_revision, 1000000, preflight)
        report.update(passed=True, ready_for_Z2M=True, required_Z2M_baudrate=1000000)
    except BaseException:
        report['failed'] = True
        if touched:
            try:
                if command: command.close(); command = None
                command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
                latest = capture_twice(command, work, 'recovery-current')
                recovery = with_current_nv(stable, latest)
                report['automatic_20261114_recovery'] = write_verified(command, device, recovery, latest, work, 'recovery')
                boot(command); command.close(); command = None
                report['recovered_network'] = network_snapshot(args, folder, work / 'network-recovered.json', 20261114, 1000000, preflight)
            except BaseException:
                report['automatic_recovery_failed'] = True
        raise
    finally:
        if command:
            if not touched:
                try: boot(command)
                except Exception: pass
            command.close()
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))

if __name__ == '__main__': main()
