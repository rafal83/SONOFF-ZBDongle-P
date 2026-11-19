"""Explicit 20261117 <-> 2026111801 trial on the user's identified dongle.

Application sectors only. Preserve CURRENT NV/CCFG in both directions.
No commissioning, permitJoin, scratch NV or old full-image restoration.
"""
import argparse
import json
from pathlib import Path

from upgrade_production import load_profile, upgrade_target, PROFILES
from migrate_production import (ROOT, NV_START, NV_END, sha, with_current_nv,
    capture_twice, write_verified, network_snapshot)
from run_dongle_session import open_rom, boot

STABLE, CANDIDATE = 20261117, 2026111801

def qualify_candidate(revision=CANDIDATE):
    _, manifest=load_profile(revision)
    folder=ROOT/'build'/PROFILES[revision][0]
    if manifest.get('sdk_version')!='8.33.00.16' or manifest['final_qualified']:
        raise ValueError('Unexpected SDK/candidate scope')
    elf_digest=sha((folder/'sonoff-znp-coordinator.out').read_bytes())
    if elf_digest!=manifest['elf_sha256']: raise ValueError('Candidate ELF changed')
    for name in ['optimization-tests.json','core-engine-tests.json','core-integration-tests.json',
                 'arm-radio-tests.json','arm-autobaud-tests.json']:
        report=json.loads((folder/name).read_text(encoding='utf-8'))
        if not report['passed']: raise ValueError('Candidate software qualification failed')
        if name.startswith('arm-') or name=='core-integration-tests.json':
            if report['elf_sha256']!=elf_digest: raise ValueError('ARM proof does not match candidate')
        if name=='core-integration-tests.json':
            if report['revision']!=revision or not report['firmware_integration']:
                raise ValueError('Core firmware integration proof missing')
            inputs=report['link_audit']['source_sha256']
        else: inputs=report.get('input_sha256',{})
        for relative,digest in inputs.items():
            if sha((ROOT/relative).read_bytes())!=digest: raise ValueError('Candidate source changed: '+relative)
    return manifest

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action',choices=['install','rollback'])
    ap.add_argument('--candidate-revision',type=int,choices=[2026111801,2026111802],default=CANDIDATE)
    ap.add_argument('--source-revision',type=int,choices=[STABLE,2026111801],default=STABLE)
    ap.add_argument('--port',required=True); ap.add_argument('--usb-serial',required=True)
    ap.add_argument('--expect-ieee',required=True)
    ap.add_argument('--session-dir',required=True,type=Path)
    ap.add_argument('--z2m-stopped',required=True,action='store_true')
    args=ap.parse_args(); folder=args.session_dir.resolve()
    if not folder.is_relative_to((ROOT/'private').resolve()): raise ValueError('Session must remain private')
    data=json.loads((folder/'z2m-snapshot.json').read_text(encoding='utf-8'))
    if data['ieee'].lower()!=args.expect_ieee.lower() or data['configured_baudrate']!=1000000:
        raise ValueError('Z2M snapshot identity/baud mismatch')
    for relative,digest in data['sha256'].items():
        if sha((folder/'z2m-data'/relative).read_bytes())!=digest: raise ValueError('Z2M snapshot changed')
    candidate_revision=args.candidate_revision; baseline_source=args.source_revision
    stable,_=load_profile(STABLE); candidate,_=load_profile(candidate_revision)
    if args.action=='install': qualify_candidate(candidate_revision)
    else:
        installed=json.loads((folder/'install/report.json').read_text(encoding='utf-8'))
        if not installed.get('passed') or installed['destination_hex_sha256']!=PROFILES[candidate_revision][1]:
            raise ValueError('No verified candidate installation in this session')
        baseline_source=installed.get('source_revision',STABLE)
    before=folder/'network-before.json'
    baseline=json.loads(Path(str(before)+'.report.json').read_text(encoding='utf-8'))
    if not baseline['passed'] or baseline['revision']!=baseline_source or baseline['database_devices_absent_from_backup']:
        raise ValueError('Fresh stable network backup is incomplete')
    source_revision,destination_revision=(args.source_revision,candidate_revision) if args.action=='install' else (candidate_revision,STABLE)
    source=load_profile(source_revision)[0]
    destination=candidate if args.action=='install' else stable
    work=folder/args.action; work.mkdir()
    report=dict(action=args.action,passed=False,source_revision=source_revision,destination_revision=destination_revision,
        source_hex_sha256=PROFILES[source_revision][1],destination_hex_sha256=PROFILES[destination_revision][1],
        baudrate=1000000,NV_and_CCFG_not_erased_or_programmed=True,commissioning_commands_sent=False,
        Z2M_started=False,production_network_used=True)
    command=None; touched=False
    try:
        preflight=work/'network-preflight.json'
        network_snapshot(args,folder,preflight,source_revision,1000000,before)
        command,device=open_rom(args.port,args.usb_serial,args.expect_ieee)
        current=capture_twice(command,work,'current')
        target=upgrade_target(current,source,destination)
        (work/'target.bin').write_bytes(target)
        report['two_full_ROM_captures_identical']=True
        report['original_full_snapshot_sha256']=sha(current)
        report['NV_bytes_identical_before_boot']=target[NV_START:NV_END]==current[NV_START:NV_END]
        touched=True
        report['programmed']=write_verified(command,device,target,current,work,'target')
        boot(command); command.close(); command=None
        report['network_verification']=network_snapshot(args,folder,work/'network-after.json',destination_revision,1000000,preflight)
        report.update(passed=True,ready_for_Z2M=True,required_Z2M_baudrate=1000000)
    except BaseException:
        report['failed']=True
        if touched:
            try:
                if command: command.close(); command=None
                command,device=open_rom(args.port,args.usb_serial,args.expect_ieee)
                latest=capture_twice(command,work,'recovery-current')
                recovery=with_current_nv(stable,latest)
                report['automatic_20261117_recovery']=write_verified(command,device,recovery,latest,work,'recovery')
                boot(command); command.close(); command=None
                report['recovered_network']=network_snapshot(args,folder,work/'network-recovered.json',STABLE,1000000,preflight)
            except BaseException:
                report['automatic_recovery_failed']=True
        raise
    finally:
        if command:
            if not touched:
                try: boot(command)
                except Exception: pass
            command.close()
        (work/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))

if __name__=='__main__': main()
