"""Publish bounded 20261117 trial evidence; no keys, addresses or device names."""
import argparse
from collections import Counter
from datetime import datetime
import json
import re
from pathlib import Path
import subprocess
import zipfile
from upgrade_production import load_profile, PROFILES
from migrate_production import ROOT, NV_START, NV_END, sha, same_program

def read(path): return json.loads(path.read_text(encoding='utf-8'))

def export(folder):
    upgrade=read(folder/'upgrade/report.json')
    current=(folder/'upgrade/current-a.bin').read_bytes()
    target=(folder/'upgrade/target-readback.bin').read_bytes()
    stable,_=load_profile(20261114); candidate,_=load_profile(20261117)
    if not (upgrade['passed'] and upgrade['ready_for_Z2M'] and upgrade['NV_and_CCFG_not_erased_or_programmed']
            and current==(folder/'upgrade/current-b.bin').read_bytes()
            and target==(folder/'upgrade/target.bin').read_bytes()
            and sha(target)==upgrade['programmed']['sha256'] and same_program(current,stable)
            and same_program(target,candidate) and current[NV_START:]==target[NV_START:]):
        raise ValueError('Physical upgrade evidence mismatch')
    start=read(folder/'startup-initial-report.json')
    repaired=(folder/'running-Z2M-backup-after-archive/report.json').is_file()
    phase='-after-backup-repair' if repaired else ''
    mqtt=read(folder/f'MQTT-observation{phase}.private.json')
    health=mqtt['report']
    if not (start['expected_firmware_detected'] and start['Z2M_started']
            and health['status']['firmware_revision']==20261117 and health['status']['expected_firmware']
            and health['health_check_ok'] and health['coordinator_check_ok']):
        raise ValueError('Real startup/health evidence incomplete')
    live_path=folder/'coordinator-backup-after-server-start.private.json'
    backup=read(live_path)
    date=backup['metadata']['internal']['date']
    if datetime.fromisoformat(date.replace('Z','+00:00')).timestamp()<=(folder/'upgrade/report.json').stat().st_mtime:
        raise ValueError('Native backup is not fresh')
    script="const fs=require('node:fs');const{compareNetwork}=require('./tools/production_nv_snapshot.cjs');compareNetwork(JSON.parse(fs.readFileSync(process.argv[1])),JSON.parse(fs.readFileSync(process.argv[2])));"
    with (folder/'final-backup-comparison.private.log').open('wb') as log:
        result=subprocess.run(['node','-e',script,str(folder/'upgrade/network-preflight.json'),str(live_path)],
                              cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,timeout=15)
    if result.returncode: raise ValueError('Live backup network/key/counter continuity failed')
    trials=[]
    for path in folder.glob('codex-light-demo-*/report.json'):
        trial=read(path)
        if trial.get('firmware_revision')=='20261117': trials.append((path,trial))
    brightness=[(p,r) for p,r in trials if r.get('passed') and r.get('verified_changes')==48]
    colour=[(p,r) for p,r in trials if r.get('passed') and r.get('on_off_strobe') and r.get('colour_commands')==6]
    if not brightness or not colour: raise ValueError('Targeted radio/control regressions incomplete')
    bp,br=brightness[-1]; cp,cr=colour[-1]
    if not all(r.get('final_brightness_verified') and r.get('final_ON_verified') for r in [br,cr]):
        raise ValueError('Final bulb state not verified')
    observations=read(folder/f'device-observations{phase}.private.json')
    initial=read(folder/'startup-initial-devices.private.json')
    seen={r['friendly_name']:r['type'] for r in observations+initial if r['fresh_radio_timestamp_after_install']}
    seen.update({n:r['type'] for n,r in mqtt['fresh'].items()})
    inputs=['upgrade/report.json','upgrade/target-readback.bin','upgrade/network-after.json.report.json',
        'startup-initial-report.json','startup-initial.private.log','production-log-check.json',
        'production-startup.private.log','MQTT-observation.private.json',
        'coordinator-backup-after-server-start.private.json','running-backup-process.log']
    inputs += [bp.relative_to(folder).as_posix(),cp.relative_to(folder).as_posix()]
    latest=read(folder/'production-log-check.json')
    repair=None
    if repaired:
        retry=read(folder/'running-Z2M-backup-after-archive/report.json')
        archive_path=folder/'running-Z2M-backup-after-archive/Z2M-data.zip'
        with zipfile.ZipFile(archive_path) as z:
            if z.testzip(): raise ValueError('Repaired backup archive CRC mismatch')
        if not (retry['passed'] and retry['Z2M_backup_API_passed'] and retry['archive_CRC_verified']
                and retry['network_identity_and_keys_preserved'] and retry['security_counters_non_decreasing']
                and retry['previous_devices_preserved'] and retry['firmware_revision']==20261117
                and retry['backup_archive_bytes']==archive_path.stat().st_size
                and retry['backup_base64_bytes']+16384<1048576
                and health['transport']['unexpected_disconnections']==0):
            raise ValueError('Backup transport repair evidence incomplete')
        archival=read(folder/'backup-transport-repair/applied.json')
        if not (archival['passed'] and archival['all_archived_checksums_verified'] and archival['active_configuration_unchanged']):
            raise ValueError('Archival verification incomplete')
        native_retry=read(folder/'running-Z2M-backup-after-archive/coordinator-backup.json')
        boundary=datetime.fromisoformat(native_retry['metadata']['internal']['date'].replace('Z','+00:00')).astimezone().replace(tzinfo=None)
        errors=0
        for line in (folder/'production-startup-after-backup-repair.private.log').read_text(encoding='utf-8').splitlines():
            match=re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\].*\berror\s*:',line,re.I)
            if match and datetime.strptime(match[1],'%Y-%m-%d %H:%M:%S')>=boundary.replace(microsecond=0): errors+=1
        repair=dict(verified=True, historical_configuration_copies_archived=archival['historical_configs'],
                    unreferenced_icons_archived=archival['unreferenced_icons'],
                    archive_bytes=retry['backup_archive_bytes'],base64_bytes=retry['backup_base64_bytes'],
                    CRC_verified=True,network_and_counters_verified=True,
                    post_repair_health_passed=True,post_repair_unexpected_MQTT_disconnections=0,
                    post_repair_log_errors=errors,
                    Z2M_restarted=False,active_configuration_modified=False)
        inputs += ['backup-transport-repair/plan.private.json','backup-transport-repair/applied.json',
            'running-Z2M-backup-after-archive/report.json','running-Z2M-backup-after-archive/Z2M-data.zip',
            'running-Z2M-backup-after-archive/coordinator-backup.json',
            'MQTT-observation-after-backup-repair.private.json','production-log-check-after-backup-repair.json',
            'production-startup-after-backup-repair.private.log','initial-upgrade-evidence.zip']
    return dict(date='2026-11-17',experimental_hex_sha256=PROFILES[20261117][1],revision=20261117,baudrate=1000000,
        hardware_tested=True,hardware_test_scope='Application CRC/full readback; read-only SYS/NV; network continuity; real Z2M/MQTT; controlled HG06106C ZCL reads and actions',
        full_readback_verified=True,NV_and_CCFG_not_erased_or_programmed=True,
        database_devices=upgrade['network_verification']['database_devices'],
        all_database_devices_in_coordinator_backup=upgrade['network_verification']['database_devices_absent_from_backup']==0,
        network_keys_preserved=True,security_counters_non_decreasing=True,z2m_startup_tested=True,
        MQTT_health_check_passed=True,coordinator_check_passed=True,
        missing_routers_in_coordinator=health['missing_routers_in_coordinator'],
        Z2M_version=health['status']['Z2M_version'],herdsman_version=health['status']['herdsman_version'],
        fresh_radio_device_count=len(seen),fresh_radio_types=dict(Counter(seen.values())),
        controlled_radio_peers=1,controlled_radio_model='HG06106C',radio_peer_exchange_tested=True,
        verified_brightness_changes=48,colour_commands=cr['colour_commands'],OFF_commands=cr['OFF_commands'],
        final_bulb_ON_and_maximum_verified=True,starting_colour_restored_at_Z2M=cr.get('starting_colour_restored_at_Z2M',False),
        fresh_native_backup_verified=True,logical_backup_restore_tested=False,
        MQTT_archive_export_response_received=repaired,initial_MQTT_archive_export_timeout_seconds=45,
        MQTT_backup_transport_repair=repair,
        transient_MQTT_unknown_property_and_reconnection_observed=True,
        startup_log_errors=start['error_categories'],later_log_errors=latest['error_categories'],
        failed_control_attempts=sum(not r.get('passed') for _,r in trials),successful_colour_retry=True,
        all_attempts_passed=False,candidate_left_installed=True,source_20261114_saved=True,
        rollback_strategy='Restore 20261114 application only; retain current NV and security counters',
        all_75_radio_devices_validated=False,all_74_radio_devices_validated=False,
        native_security_fault_injection_on_hardware_tested=False,endurance_tested=False,final_qualified=False,
        evidence_sha256={name:sha((folder/name).read_bytes()) for name in inputs})

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session-dir',required=True,type=Path)
    ap.add_argument('--output',required=True,type=Path)
    args=ap.parse_args()
    if not args.session_dir.resolve().is_relative_to((ROOT/'private').resolve()): raise ValueError('Private evidence required')
    report=export(args.session_dir)
    args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:report[k] for k in ['revision','database_devices','fresh_radio_device_count','MQTT_health_check_passed','fresh_native_backup_verified','all_attempts_passed']}))

if __name__=='__main__': main()
