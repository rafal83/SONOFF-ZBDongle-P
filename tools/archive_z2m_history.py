"""Plan or archive obsolete Z2M migration copies and unreferenced icons.

Active configuration, database, state, coordinator backup and firmware are
never moved. Every candidate must match the private pre-upgrade snapshot.
The sibling archive retains original files; a second copy remains on this PC.
"""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import zipfile

SOURCE = Path(r'\\192.168.1.25\config\zigbee2mqtt')
ARCHIVE = Path(r'\\192.168.1.25\config\zigbee2mqtt-history-20261117')
HISTORY = {f'configuration_backup_v{i}.yaml' for i in range(1, 5)}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session-dir', type=Path, required=True)
    ap.add_argument('--apply-plan-sha256')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    folder = args.session_dir.resolve()
    if not folder.is_relative_to((root/'private').resolve()):
        raise ValueError('Private session required')
    source, archive = SOURCE.resolve(strict=True), ARCHIVE.resolve()
    if source != SOURCE or archive != ARCHIVE or archive.parent != source.parent:
        raise ValueError('Unexpected resolved share path')
    if source.is_symlink() or archive.is_symlink() or archive.is_relative_to(source):
        raise ValueError('Archive must be a separate sibling directory')
    output = folder/'backup-transport-repair'
    output.mkdir(exist_ok=True)
    if (output/'applied.json').exists():
        raise ValueError('Already applied; preserve evidence')
    files = {}
    for p in source.rglob('*'):
        rel = p.relative_to(source)
        if rel.parts[0] in ('log', 'ota'):
            continue
        if p.is_file():
            if p.is_symlink() or not p.resolve().is_relative_to(source):
                raise ValueError('Unexpected linked file')
            files[rel.as_posix()] = p.read_bytes()
    if not HISTORY.issubset(files):
        raise ValueError('Expected historical migration files absent')
    # Search every remaining textual file, including group/device settings,
    # persisted frontend state and external converters, not just config.yaml.
    text = b'\n'.join(data for name, data in files.items()
                      if name not in HISTORY and Path(name).suffix.lower()
                      in ('.yaml', '.yml', '.json', '.db', '.js', '.mjs', '.cjs', '.ts', '.html', '.css', '.svg'))
    unused = {name for name in files if name.startswith('device_icons/')
              and len(Path(name).parts) == 2 and Path(name).suffix == '.png'
              and Path(name).name.encode() not in text}
    candidates = sorted(HISTORY | unused)
    if not 4 <= len(candidates) <= 34:
        raise ValueError('Unexpected archival scope')
    for name in candidates:
        saved = folder/'z2m-data'/name
        if not saved.is_file() or saved.read_bytes() != files[name]:
            raise ValueError('Candidate differs from existing private snapshot')
        dest = archive/name
        if dest.exists() or not dest.resolve().is_relative_to(archive):
            raise ValueError('Archive target exists or escapes intended directory')
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, data in files.items():
            if name not in candidates:
                z.writestr(name, data)
    projected = len(base64.b64encode(stream.getvalue()))
    if projected + 16384 >= 1048576:
        raise ValueError('Projected backup still exceeds conservative MQTT budget')
    plan = dict(source=str(source), archive=str(archive),
                active_configuration_sha256=sha(files['configuration.yaml']),
                files=[dict(name=n, bytes=len(files[n]), sha256=sha(files[n])) for n in candidates],
                projected_zip_bytes=len(stream.getvalue()), projected_base64_bytes=projected,
                active_files_moved=False, Z2M_restarted=False, firmware_modified=False)
    encoded = (json.dumps(plan, indent=2)+'\n').encode()
    digest = sha(encoded)
    if not args.apply_plan_sha256:
        (output/'plan.private.json').write_bytes(encoded)
        print(json.dumps(dict(plan_sha256=digest, files=len(candidates),
                             historical_configs=len(HISTORY), unreferenced_icons=len(unused),
                             projected_base64_bytes=projected)))
        return
    reviewed_bytes = (output/'plan.private.json').read_bytes()
    reviewed = json.loads(reviewed_bytes)
    # Live state/backup sizes can change while Z2M runs. The exact selected
    # files, checksums, configuration and paths must remain identical.
    stable = lambda p: {k: v for k, v in p.items() if k not in ('projected_zip_bytes', 'projected_base64_bytes')}
    if sha(reviewed_bytes) != args.apply_plan_sha256 or stable(plan) != stable(reviewed):
        raise ValueError('Reviewed plan changed; regenerate it first')
    moved = []
    try:
        for row in plan['files']:
            name = row['name']
            src, dest = source/name, archive/name
            if sha((source/'configuration.yaml').read_bytes()) != plan['active_configuration_sha256']:
                raise ValueError('Active configuration changed during archival')
            if sha(src.read_bytes()) != row['sha256'] or dest.exists():
                raise ValueError('Candidate changed or archive target appeared')
            dest.parent.mkdir(parents=True, exist_ok=True)
            src.rename(dest)
            moved.append(row)
            if sha(dest.read_bytes()) != row['sha256']:
                raise ValueError('Archived file verification failed')
    except Exception:
        for row in reversed(moved):
            src, dest = source/row['name'], archive/row['name']
            if not src.exists() and sha(dest.read_bytes()) == row['sha256']:
                dest.rename(src)
        raise
    report = dict(passed=True, plan_sha256=args.apply_plan_sha256, moved_files=len(moved),
                  historical_configs=len(HISTORY), unreferenced_icons=len(unused),
                  all_archived_checksums_verified=True, private_snapshot_matches=True,
                  active_configuration_unchanged=sha((source/'configuration.yaml').read_bytes()) == plan['active_configuration_sha256'],
                  projected_base64_bytes=projected, Z2M_restarted=False, firmware_modified=False)
    (output/'applied.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
