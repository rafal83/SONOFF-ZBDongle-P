"""Request one Z2M backup, retain it privately, verify network continuity.

No filesystem extraction of the returned archive, configuration edit, reset,
pairing, or actuation. MQTT credentials never appear in output/arguments.
"""
import argparse
import base64
from datetime import datetime
import io
import json
from pathlib import Path
import subprocess
import threading
import time
from urllib.parse import urlparse
import uuid
import zipfile
import yaml
import paho.mqtt.client as mqtt

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session-dir', type=Path, required=True)
    ap.add_argument('--broker-host', required=True)
    ap.add_argument('--expected-revision', type=int, choices=[20261114,20261117,2026111801,2026111802], required=True)
    ap.add_argument('--output-name', default='running-Z2M-backup')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    folder = args.session_dir.resolve()
    if not folder.is_relative_to((root/'private').resolve()): raise ValueError('Private output required')
    if not args.output_name.startswith('running-Z2M-backup') or Path(args.output_name).name != args.output_name:
        raise ValueError('Expected bounded backup output name')
    output = folder/args.output_name
    output.mkdir()
    config = yaml.safe_load((folder/'z2m-data/configuration.yaml').read_text(encoding='utf-8'))
    connection=config['mqtt']; url=urlparse(connection['server']); base=connection.get('base_topic','zigbee2mqtt')
    if url.scheme!='mqtt': raise ValueError('Expected existing LAN MQTT connection')
    expected_ieee=json.loads((folder/'z2m-snapshot.json').read_text())['ieee'].lower()
    token='codex-backup-'+uuid.uuid4().hex[:12]
    ready, complete=threading.Event(),threading.Event()
    response={}
    client=mqtt.Client(client_id=token,clean_session=True)
    client.username_pw_set(connection.get('user',url.username),connection.get('password',url.password))
    def connected(c,userdata,flags,code):
        if code==0: c.subscribe([(base+'/bridge/info',0),(base+'/bridge/response/backup',0)])
    def received(c,userdata,msg):
        try: data=json.loads(msg.payload)
        except (ValueError,UnicodeDecodeError): return
        if msg.topic==base+'/bridge/info':
            coordinator=data.get('coordinator',{})
            if str(coordinator.get('meta',{}).get('revision'))==str(args.expected_revision) and str(coordinator.get('ieee_address','')).lower()==expected_ieee:
                ready.set()
        elif msg.topic==base+'/bridge/response/backup' and data.get('transaction')==token:
            response.update(data); complete.set()
    client.on_connect,client.on_message=connected,received
    client.connect(args.broker_host,url.port or 1883,30); client.loop_start()
    try:
        if not ready.wait(8): raise RuntimeError('Expected coordinator revision did not become ready')
        client.publish(base+'/bridge/request/backup',json.dumps({'transaction':token}),qos=1,retain=False)
        if not complete.wait(45): raise TimeoutError('Z2M backup response deadline exceeded')
    finally: client.disconnect(); client.loop_stop()
    (output/'response.private.json').write_text(json.dumps(response),encoding='utf-8')
    if response.get('status')!='ok': raise RuntimeError('Z2M backup failed; response retained privately')
    encoded=response['data']['zip']
    if len(encoded)>70*1024*1024: raise ValueError('Unexpected archive size')
    archive=base64.b64decode(encoded,validate=True)
    (output/'Z2M-data.zip').write_bytes(archive)
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        if z.testzip(): raise ValueError('Returned archive CRC verification failed')
        selected=[i for i in z.infolist() if Path(i.filename).name=='coordinator_backup.json']
        if len(selected)!=1 or selected[0].file_size>1024*1024: raise ValueError('Expected one coordinator backup')
        backup=json.loads(z.read(selected[0]))
    after=output/'coordinator-backup.json'
    after.write_text(json.dumps(backup,indent=2)+'\n',encoding='utf-8')
    migration=folder/('install' if (folder/'install/report.json').is_file() else 'upgrade')
    before=migration/'network-preflight.json'
    script="const fs=require('node:fs'); const {compareNetwork}=require('./tools/production_nv_snapshot.cjs'); compareNetwork(JSON.parse(fs.readFileSync(process.argv[1])),JSON.parse(fs.readFileSync(process.argv[2])));"
    with (output/'comparison.private.log').open('xb') as log:
        run=subprocess.run(['node','-e',script,str(before),str(after)],cwd=root,stdout=log,stderr=subprocess.STDOUT,timeout=15)
    if run.returncode: raise RuntimeError('Backup network continuity check failed; see private comparison')
    date=backup.get('metadata',{}).get('internal',{}).get('date')
    fresh=bool(date and datetime.fromisoformat(date.replace('Z','+00:00')).timestamp()>(migration/'report.json').stat().st_mtime)
    report=dict(passed=fresh,firmware_revision=args.expected_revision,Z2M_backup_API_passed=True,
        native_coordinator_backup_created_after_upgrade=fresh,network_identity_and_keys_preserved=True,
        security_counters_non_decreasing=True,previous_devices_preserved=True,
        backup_devices=len(backup['devices']),backup_archive_bytes=len(archive),
        backup_base64_bytes=len(encoded),archive_CRC_verified=True,
        configuration_modified=False,device_state_modified=False)
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))
    if not fresh: raise RuntimeError('Returned coordinator backup predates this upgrade')

if __name__=='__main__': main()
