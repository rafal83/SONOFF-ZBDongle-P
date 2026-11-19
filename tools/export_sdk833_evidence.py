"""Export secret-free evidence bound to the exact installed SDK 8.33 HEX."""
import argparse
import json
from pathlib import Path

from migrate_production import ROOT,NV_START,sha,same_program
from upgrade_production import load_profile,PROFILES

def read(path): return json.loads(path.read_text(encoding='utf-8'))

def export(folder,revision=2026111802):
    install=read(folder/'install/report.json')
    before=(folder/'install/current-a.bin').read_bytes()
    after=(folder/'install/target-readback.bin').read_bytes()
    stable,_=load_profile(install['source_revision']); candidate,_=load_profile(revision)
    identity=read(folder/'identity-after.json'); network=install['network_verification']
    if not (install['passed'] and install['ready_for_Z2M'] and install['NV_and_CCFG_not_erased_or_programmed']
            and install['destination_revision']==revision and install['destination_hex_sha256']==PROFILES[revision][1]
            and before==(folder/'install/current-b.bin').read_bytes()
            and after==(folder/'install/target.bin').read_bytes() and before[NV_START:]==after[NV_START:]
            and same_program(before,stable) and same_program(after,candidate)
            and sha(after)==install['programmed']['sha256'] and install['programmed']['full_readback_verified']
            and identity['passed'] and identity['revision']==revision and identity['pings']==300
            and network['passed'] and network['network_parameters_and_keys_preserved']
            and network['security_counters_non_decreasing'] and network['database_devices_absent_from_backup']==0):
        raise ValueError('Physical firmware/network evidence mismatch')
    evidence=['install/report.json','install/current-a.bin','install/current-b.bin','install/target-readback.bin',
        'identity-before.json','identity-after.json','network-before.json.report.json']
    report=dict(date='2026-11-18',revision=revision,sdk_version='8.33.00.16',baudrate=1000000,
        experimental_hex_sha256=PROFILES[revision][1],hardware_tested=True,
        hardware_test_scope='Application CRC/full readback; MCU boot; 300 read-only SYS pings; network backup continuity',
        full_readback_verified=True,two_original_ROM_captures_identical=True,NV_and_CCFG_not_erased_or_programmed=True,
        network_keys_preserved=True,security_counters_non_decreasing=True,
        database_devices=network['database_devices'],all_database_devices_in_coordinator_backup=True,
        ping_300_passed=True,ping_latency_ms=identity['ping_latency_ms'],
        z2m_startup_tested=False,MQTT_health_check_passed=False,coordinator_check_passed=False,
        controlled_radio_peer_exchange_tested=False,fresh_radio_device_count=0,
        full_network_radio_validation=False,endurance_tested=False,final_qualified=False,
        candidate_left_installed=True,rollback_strategy='20261117 application only; retain CURRENT NV/security counters',
        physical_rollback_to_20261117_executed=False)
    logs=folder/'production-log-check-sdk833.json'
    if logs.is_file():
        log=read(logs)
        report['z2m_startup_tested']=bool(log['expected_firmware_detected'] and log['Z2M_started'] and log['herdsman_started'])
        report['startup_log_error_categories']=log['error_categories']
        report['fresh_radio_device_count']=log['devices_observed_after_installation']
        raw=folder/'production-startup-sdk833.private.log'
        if raw.is_file():
            failures=sum('MEM_ERROR' in line and 'error:' in line for line in raw.read_text(encoding='utf-8',errors='replace').splitlines())
            report['AF_MEM_ERROR_log_count']=failures
            report['radio_trial_failed_with_MEM_ERROR']=bool(failures)
            evidence.append(raw.relative_to(folder).as_posix())
        evidence.append(logs.relative_to(folder).as_posix())
    mqtt=folder/'MQTT-observation-sdk833.private.json'
    if mqtt.is_file():
        observation=read(mqtt); health=observation['report']
        if health['status'].get('expected_firmware') and health['status'].get('firmware_revision')==revision:
            report.update(MQTT_health_check_passed=health['health_check_ok'],coordinator_check_passed=health['coordinator_check_ok'],
                missing_routers_in_coordinator=health['missing_routers_in_coordinator'],
                unexpected_MQTT_disconnections=health['transport']['unexpected_disconnections'],
                Z2M_version=health['status'].get('Z2M_version'),herdsman_version=health['status'].get('herdsman_version'),
                router_GETs_sent=health['router_GETs_sent'],GETs_followed_by_fresh_telemetry=health['GETs_followed_by_fresh_telemetry'])
            report['fresh_radio_device_count']=max(report['fresh_radio_device_count'],health['fresh_devices'])
        evidence.append(mqtt.relative_to(folder).as_posix())
    backup=folder/'running-Z2M-backup/report.json'
    if backup.is_file():
        b=read(backup)
        report['fresh_native_backup_verified']=bool(b['passed'] and b['firmware_revision']==revision and b['network_identity_and_keys_preserved'] and b['security_counters_non_decreasing'])
        report['MQTT_archive_export_response_received']=b['Z2M_backup_API_passed']
        evidence.append(backup.relative_to(folder).as_posix())
    fresh=set(); radio_reads=0
    for path in folder.glob('MQTT-observation-*.private.json'):
        observation=read(path); health=observation['report']
        if health['status'].get('firmware_revision')!=revision or not health['status'].get('expected_firmware'): continue
        fresh.update(observation['fresh'])
        if health['setters_sent']==0 and not health['associations_opened']:
            radio_reads+=health['GETs_followed_by_fresh_telemetry']
        relative=path.relative_to(folder).as_posix()
        if relative not in evidence: evidence.append(relative)
    report['fresh_radio_device_count']=max(report['fresh_radio_device_count'],len(fresh))
    report['controlled_radio_peer_exchange_tested']=radio_reads>0
    report['successful_read_only_radio_GETs']=radio_reads
    if report['z2m_startup_tested']: report['hardware_test_scope']+='; real Z2M startup'
    if report['MQTT_health_check_passed']: report['hardware_test_scope']+='; MQTT/coordinator health'
    if radio_reads: report['hardware_test_scope']+='; router state GET followed by fresh radio telemetry'
    report['evidence_sha256']={name:sha((folder/name).read_bytes()) for name in evidence}
    return report

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session-dir',required=True,type=Path); ap.add_argument('--output',required=True,type=Path)
    ap.add_argument('--revision',type=int,choices=[2026111801,2026111802],default=2026111802)
    args=ap.parse_args(); folder=args.session_dir.resolve()
    if not folder.is_relative_to((ROOT/'private').resolve()): raise ValueError('Private evidence required')
    report=export(folder,args.revision)
    args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:report[k] for k in ['revision','hardware_tested','database_devices','z2m_startup_tested','MQTT_health_check_passed','fresh_radio_device_count']}))

if __name__=='__main__': main()
