"""Clean known generated files; preserve current proofs and rollback assets.

Default is a plan. --apply archives legacy build results and the old SDK
workspace/patches privately, verifies the archive, then removes allowed paths.
No serial, MQTT, shared server, Git metadata or existing private backup access.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import zipfile

ROOT=Path(__file__).resolve().parents[1]
CURRENT='coordinator-optimized-1000000-cores-sdk833-r2'
OLD_SDK='vendor-sdk/simplelink_cc13xx_cc26xx_sdk_8_30_01_01'
KEEP_BUILD={CURRENT,CURRENT+'-logs','coordinator-overlay-sdk833-r2','core-integration-inputs-sdk833-r2',
    'coordinator-optimized-1000000-cores-sdk833', # Source image of R2's physical upgrade proof.
    'coordinator-optimized-1000000-next','coordinator-optimized-1000000','core-engines','maintenance',
    'sdk-migration-comparison.json','coordinator-optimization-tests-sdk833-r2.json',
    'sonoff-znp-optimized-2026111802-1000000-cores-sdk833-r2-lab.zip',
    'sonoff-znp-optimized-2026111802-1000000-cores-sdk833-r2-lab.sha256',
    'sonoff-znp-optimized-20261117-1000000-next-lab.zip','sonoff-znp-optimized-20261117-1000000-next-lab.sha256',
    'sonoff-znp-optimized-20261114-1000000-lab.zip','sonoff-znp-optimized-20261114-1000000-lab.sha256'}
GENERATED=['.cache','.tooling/ccs-installer','.tooling/ccs-temp','.tooling/ghidra',
    '.tooling/downloads/CCS_20.1.0.00006_win.zip',
    '.tooling/downloads/ghidra_11.4.3_PUBLIC_20251203.zip',
    '.tooling/downloads/sysconfig-1.23.0_4000-setup.exe',
    '.tooling/downloads/ti_cgt_armllvm_4.0.2.LTS_windows-x64_installer.exe',
    OLD_SDK,'tools/__pycache__','tests/__pycache__']

def sha(path):
    with path.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()

def safe(relative):
    p=ROOT/relative; resolved=p.resolve()
    if not resolved.is_relative_to(ROOT.resolve()) or resolved==ROOT.resolve(): raise ValueError('Path escapes workspace: '+relative)
    if Path(relative).parts[0] in ('private','.git','firmware','docs','upstream'): raise ValueError('Protected deletion target: '+relative)
    if p.is_symlink() or p.is_junction(): raise ValueError('Refusing a link/junction: '+relative)
    if p.is_dir():
        for directory,dirs,files in os.walk(p,followlinks=False):
            for name in dirs+files:
                child=Path(directory)/name
                if child.is_symlink() or child.is_junction(): raise ValueError('Linked child in deletion target: '+str(child))
    return p

def files_under(path):
    if path.is_file(): yield path
    elif path.is_dir():
        for directory,_,files in os.walk(path,followlinks=False):
            for name in files: yield Path(directory)/name

def proof_inputs():
    folder=ROOT/'build'/CURRENT; protected={}
    for name in ['optimization-tests.json','core-engine-tests.json','core-integration-tests.json']:
        report=json.loads((folder/name).read_text(encoding='utf-8'))
        if not report['passed']: raise ValueError('Current test proof failed')
        protected.update(report.get('input_sha256',{}))
        protected.update(report.get('link_audit',{}).get('source_sha256',{}))
    for relative,digest in protected.items():
        if sha(ROOT/relative)!=digest: raise ValueError('Current proof source differs: '+relative)
    return protected

def plan():
    protected=proof_inputs()
    legacy=['build/'+p.name for p in (ROOT/'build').iterdir() if p.name not in KEEP_BUILD]
    targets=[n for n in GENERATED+legacy if (ROOT/n).exists()]
    for relative in targets:
        safe(relative)
        if any(n==relative or n.startswith(relative+'/') for n in protected):
            raise ValueError('Cleanup would remove a current proof input: '+relative)
    size=sum(p.stat().st_size for n in targets for p in files_under(ROOT/n))
    return dict(removals=targets,legacy_build=legacy,bytes_before_archive=size,
        current_firmware_sha256=sha(ROOT/'build'/CURRENT/'sonoff-znp-coordinator.hex'),
        rollback_20261117_sha256=sha(ROOT/'build/coordinator-optimized-1000000-next/sonoff-znp-coordinator.hex'),
        private_backups_removed=False,firmware_source_files_removed=False)

def main():
    ap=argparse.ArgumentParser(description=__doc__); ap.add_argument('--apply',action='store_true')
    args=ap.parse_args(); maintenance=ROOT/'build/maintenance'; maintenance.mkdir(exist_ok=True)
    report=plan(); (maintenance/'cleanup-plan-20261118.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    if not args.apply:
        print(json.dumps(report,indent=2)); return
    history=ROOT/'private/project-history-20261118'; history.mkdir()
    selected=set()
    for relative in report['legacy_build']:
        selected.update(files_under(ROOT/relative))
    selected.update(files_under(ROOT/OLD_SDK/'workspace'))
    selected.update(files_under(ROOT/'docs')); selected.add(ROOT/'README.md')
    for name in ['.extraction-verified.json','source/preinclude.h']:
        p=ROOT/OLD_SDK/name
        if p.is_file(): selected.add(p)
    patchfiles=list((ROOT/'build').glob('*-logs/standalone-baseline.patch'))
    if patchfiles:
        for relative in re.findall(r'^\+\+\+ b/(.+)$',patchfiles[0].read_text(encoding='utf-8'),re.M):
            p=ROOT/OLD_SDK/relative
            if p.is_file(): selected.add(p)
    digests={p.relative_to(ROOT).as_posix():sha(p) for p in sorted(selected)}
    archive=history/'legacy-builds-and-sdk-workspace.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for relative in digests: z.write(ROOT/relative,relative)
    with zipfile.ZipFile(archive) as z:
        if z.testzip(): raise ValueError('History archive CRC failure')
        for relative,digest in digests.items():
            if hashlib.sha256(z.read(relative)).hexdigest()!=digest: raise ValueError('History archive mismatch')
    (history/'manifest.json').write_text(json.dumps(dict(archive_sha256=sha(archive),files=digests,
        original_sdk_archives_retained=True,cleanup=report),indent=2)+'\n',encoding='utf-8')
    print('Verified private history archive:',len(digests),'files',flush=True)
    for relative in report['removals']:
        p=safe(relative)  # Resolve and check again immediately before removal.
        print('Removing generated:',relative,flush=True)
        if p.is_dir(): shutil.rmtree(p)
        else: p.unlink()
    proof_inputs()
    if sha(ROOT/'build'/CURRENT/'sonoff-znp-coordinator.hex')!=report['current_firmware_sha256']: raise ValueError('Current firmware changed')
    if sha(ROOT/'build/coordinator-optimized-1000000-next/sonoff-znp-coordinator.hex')!=report['rollback_20261117_sha256']: raise ValueError('Rollback image changed')
    report.update(passed=True,archive_sha256=sha(archive),archive_bytes=archive.stat().st_size,
        bytes_recovered=report['bytes_before_archive']-sum(p.stat().st_size for p in history.iterdir() if p.is_file()))
    (maintenance/'cleanup-report-20261118.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=True,GiB_recovered=round(report['bytes_recovered']/1024**3,2),
        current_and_rollback_unchanged=True,existing_private_backups_preserved=True)),flush=True)

if __name__=='__main__': main()
