"""Refresh only the 20261117 trial package after evidence validation; no flashing."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile
from audit_coordinator import audit
from production_release_scope import attach
from upgrade_production import load_profile, PROFILES

ROOT=Path(__file__).resolve().parents[1]
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    _,manifest=load_profile(20261117)
    folder=ROOT/'build'/PROFILES[20261117][0]
    evidence=ROOT/'docs/production-validation-20261117-next.json'
    proof=json.loads(evidence.read_text())
    private=ROOT/'private/production-next-20261117'
    for name,expected in proof['evidence_sha256'].items():
        if sha(private/name)!=expected: raise ValueError('Trial evidence changed')
    native=json.loads((folder/'optimization-tests.json').read_text())
    if not native['passed']: raise ValueError('Native tests did not pass')
    for name,expected in native['input_sha256'].items():
        if sha(ROOT/name)!=expected: raise ValueError('Software evidence is stale: '+name)
    if not attach(manifest,evidence): raise ValueError('Trial firmware does not match')
    audit(folder/'sonoff-znp-coordinator.hex',folder/'sonoff-znp-coordinator.out',folder/'sonoff-znp-coordinator.map',
          revision=20261117,optimized=True,baudrate=1000000,auto_baud=False)
    for name in ['arm-radio-tests.json','arm-autobaud-tests.json']:
        arm=json.loads((folder/name).read_text())
        if not arm['passed'] or arm['elf_sha256']!=manifest['elf_sha256']: raise ValueError('ARM evidence mismatch')
    for document in (ROOT/'docs').glob('*.md'): shutil.copyfile(document,folder/'docs'/document.name)
    shutil.copyfile(evidence,folder/'docs'/evidence.name)
    status=(ROOT/'docs/final-status.md').read_text(encoding='utf-8')
    (folder/'FINAL-STATUS.md').write_text(re.sub(r'\]\(([a-zA-Z0-9_-]+\.md)\)',r'](docs/\1)',status),encoding='utf-8',newline='\n')
    manifest['documentation_sha256']={p.name:sha(p) for p in sorted((ROOT/'docs').glob('*.md'))}
    (folder/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    text=(folder/'README.txt').read_text(encoding='utf-8')
    text=text.replace('This exact image has not passed hardware tests.',
        'Authorized hardware/Z2M trial passed within the recorded scope; see docs/production-validation-20261117-next.json.')
    text=text.replace('Z2M service/MQTT remain untested on this exact profile.',
        'Z2M startup, health and targeted HG06106C control passed. MQTT ZIP backup export timed out; the fresh native network backup was verified separately.')
    if proof['MQTT_archive_export_response_received']:
        text=text.replace('MQTT ZIP backup export timed out; the fresh native network backup was verified separately.',
            'MQTT ZIP backup initially timed out; after archiving obsolete configuration copies and unreferenced icons, ZIP receipt/CRC and network continuity passed without a Z2M restart.')
    (folder/'README.txt').write_text(text,encoding='utf-8')
    archive=ROOT/'build/sonoff-znp-optimized-20261117-1000000-next-lab.zip'
    with zipfile.ZipFile(archive) as previous: names=previous.namelist()
    names=sorted(set(names)|{'sonoff-znp-coordinator/docs/'+p.name for p in (ROOT/'docs').glob('*.md')}
                 |{'sonoff-znp-coordinator/docs/'+evidence.name})
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as package:
        for name in names:
            if not name.startswith('sonoff-znp-coordinator/') or '..' in Path(name).parts: raise ValueError('Unsafe package path')
            package.write(folder/name.removeprefix('sonoff-znp-coordinator/'),name)
    with zipfile.ZipFile(archive) as package:
        if package.testzip(): raise ValueError('Archive validation failed')
        for name in names:
            if package.read(name)!=(folder/name.removeprefix('sonoff-znp-coordinator/')).read_bytes(): raise ValueError('Archive member differs')
    archive.with_suffix('.sha256').write_text(sha(archive)+'  '+archive.name+'\n',encoding='ascii')
    report=dict(passed=True,revision=20261117,hex_sha256=manifest['hex_sha256'],archive_sha256=sha(archive),
        hardware_tested=manifest['hardware_tested'],z2m_startup_tested=True,network_devices=proof['database_devices'],
        native_backup_verified=proof['fresh_native_backup_verified'],MQTT_archive_export_response_received=proof['MQTT_archive_export_response_received'],
        full_trial_passed=False,final_qualified=False)
    (ROOT/'build/next-release-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))

if __name__=='__main__': main()
