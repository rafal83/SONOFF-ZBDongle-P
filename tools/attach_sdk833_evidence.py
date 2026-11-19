"""Refresh the SDK 8.33 package from verified physical evidence and docs."""
import argparse
import json
import re
from pathlib import Path
import shutil
import zipfile

from export_sdk833_evidence import export,ROOT,sha
from upgrade_sdk833 import qualify_candidate
from upgrade_production import load_profile,PROFILES

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session-dir',required=True,type=Path)
    ap.add_argument('--revision',type=int,choices=[2026111801,2026111802],default=2026111802)
    args=ap.parse_args(); session=args.session_dir.resolve()
    if not session.is_relative_to((ROOT/'private').resolve()): raise ValueError('Private evidence required')
    revision=args.revision
    manifest=qualify_candidate(revision) if revision==2026111802 else load_profile(revision)[1]
    report=export(session,revision)
    folder=ROOT/'build'/PROFILES[revision][0]
    suffix='-r2' if revision==2026111802 else ''
    archive=ROOT/f'build/sonoff-znp-optimized-{revision}-1000000-cores-sdk833{suffix}-lab.zip'
    public=ROOT/f'docs/production-validation-20261118-sdk833{suffix}.json'
    public.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    manifest.update(hardware_tested=True,hardware_test_scope=report['hardware_test_scope'],
        hardware_report_sha256=sha(public.read_bytes()),z2m_startup_tested=report['z2m_startup_tested'],
        MQTT_health_tested=report['MQTT_health_check_passed'],
        production_trial_started=report['z2m_startup_tested'],production_network_device_count=report['database_devices'],
        production_radio_observed_device_count=report['fresh_radio_device_count'],production_all_devices_validated=False,
        physical_rollback_to_20261117_executed=False,final_qualified=False)
    manifest['candidate_rejected_for_radio_failure']=report.get('radio_trial_failed_with_MEM_ERROR',False)
    if manifest['candidate_rejected_for_radio_failure']: manifest['release_stage']='rejected_candidate'
    manifest['controlled_radio_peer_exchange_tested']=report['controlled_radio_peer_exchange_tested']
    if report.get('fresh_native_backup_verified'): manifest['fresh_native_backup_tested']=True
    names=[]
    tool_index=ROOT/'tools/README.md'
    if tool_index.is_file():
        (folder/'tools').mkdir(exist_ok=True)
        shutil.copyfile(tool_index,folder/'tools/README.md')
        names.append('tools/README.md')
        manifest['tool_index_sha256']=sha(tool_index.read_bytes())
    for p in sorted((ROOT/'docs').iterdir()):
        if p.suffix not in ('.md','.json'): continue
        shutil.copyfile(p,folder/'docs'/p.name); names.append('docs/'+p.name)
    manifest['documentation_sha256']={p.name:sha(p.read_bytes()) for p in sorted((ROOT/'docs').glob('*.md'))}
    manifest['public_reports_sha256']={p.name:sha(p.read_bytes()) for p in sorted((ROOT/'docs').glob('*.json'))}
    manifest['physical_evidence_exporter_sha256']=sha((ROOT/'tools/export_sdk833_evidence.py').read_bytes())
    manifest['physical_evidence_packager_sha256']=sha(Path(__file__).read_bytes())
    (folder/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    status=(ROOT/'docs/final-status.md').read_text(encoding='utf-8')
    (folder/'FINAL-STATUS.md').write_text(re.sub(r'\]\(([a-zA-Z0-9_-]+\.(?:md|json))\)',r'](docs/\1)',status),encoding='utf-8')
    (folder/'README.txt').write_text(
        f'SONOFF ZBDongle-P CC2652P - complete TI ZNP; SDK 8.33.00.16; revision {revision}\n'
        'Z2M: adapter zstack, baudrate 1000000, rtscts false. Integrated indirect, BTT and NWK ENC-MIC-32 engines.\n'
        'Physical tests passed: MCU boot, 300 SYS pings, application CRC/full Flash readback and network backup continuity.\n'
        f'Z2M startup verified: {report["z2m_startup_tested"]}. MQTT health verified: {report["MQTT_health_check_passed"]}.\n'
        f'Database devices preserved: {report["database_devices"]}; observed fresh radio timestamps: {report["fresh_radio_device_count"]}.\n'
        'NV and CCFG sectors were not erased/programmed. Rollback uses the 20261117 application and CURRENT NV/counters.\n'
        f'The ARM emulation reports retain their separate modeled-boundary scope. See docs/{public.name} for hardware evidence.\n'
        f'MEM_ERROR radio failure observed: {report.get("radio_trial_failed_with_MEM_ERROR",False)}. Revision 2026111801 must not be used.\n'
        'APS, Trust Center and Green Power remain TI. Full device coverage, mesh load and endurance remain open.\n'
        'Laboratory/production trial, final_qualified=false.\n'
        'Keep all included licenses/notices. HEX is the flash image; ELF/map are debug evidence.\n',encoding='utf-8')
    prefix='sonoff-znp-coordinator/'
    with zipfile.ZipFile(archive) as z: members=set(z.namelist())
    members|={prefix+n for n in names}
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for name in sorted(members):
            if not name.startswith(prefix) or '..' in Path(name).parts: raise ValueError('Unsafe package member')
            z.write(folder/name.removeprefix(prefix),name)
    with zipfile.ZipFile(archive) as z:
        if z.testzip(): raise ValueError('Package CRC failed')
        for name in members:
            if z.read(name)!=(folder/name.removeprefix(prefix)).read_bytes(): raise ValueError('Package member mismatch')
    digest=sha(archive.read_bytes())
    archive.with_suffix('.sha256').write_text(digest+'  '+archive.name+'\n',encoding='ascii')
    print(json.dumps(dict(passed=True,revision=revision,hardware_tested=True,
        z2m_startup_tested=report['z2m_startup_tested'],archive_sha256=digest,final_qualified=False)))

if __name__=='__main__': main()
