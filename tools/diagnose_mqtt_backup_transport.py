"""Isolate backup-sized MQTT transport on diagnostic topics, without Zigbee.

Reads Z2M runtime settings and compares MQTT5 receive limits on disposable
clients. Publishes only synthetic bytes, never on any Zigbee2MQTT topic.
"""
import argparse
import hashlib
import json
from pathlib import Path
import threading
import time
from urllib.parse import urlparse
import uuid
import yaml
import paho.mqtt.client as mqtt
from paho.mqtt.properties import Properties
from paho.mqtt.packettypes import PacketTypes

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session-dir',type=Path,required=True)
    ap.add_argument('--broker-host',required=True)
    ap.add_argument('--payload-bytes',type=int,default=11037344)
    args=ap.parse_args()
    root=Path(__file__).resolve().parents[1]; folder=args.session_dir.resolve()
    if not folder.is_relative_to((root/'private').resolve()): raise ValueError('Private session required')
    if not 1 <= args.payload_bytes <= 16*1024*1024-4096: raise ValueError('Bounded payload required')
    config=yaml.safe_load((folder/'z2m-data/configuration.yaml').read_text(encoding='utf-8'))
    connection=config['mqtt']; url=urlparse(connection['server']); base=connection.get('base_topic','zigbee2mqtt')
    token=uuid.uuid4().hex[:12]; output=folder/('mqtt-transport-'+token); output.mkdir()
    def authenticate(client): client.username_pw_set(connection.get('user',url.username),connection.get('password',url.password))
    runtime={}; ready=threading.Event()
    info=mqtt.Client(client_id='codex-info-'+token)
    authenticate(info)
    def info_connect(c,u,flags,code):
        if code==0: c.subscribe([(base+'/bridge/info',0),('$SYS/broker/version',0)])
    def info_message(c,u,msg):
        if msg.topic=='$SYS/broker/version': runtime['broker_version']=msg.payload.decode(errors='replace'); return
        if msg.topic==base+'/bridge/info':
            data=json.loads(msg.payload)
            runtime.update(MQTT_version=data.get('mqtt',{}).get('version'),
                maximum_packet_size=data.get('config',{}).get('mqtt',{}).get('maximum_packet_size'),
                firmware_revision=data.get('coordinator',{}).get('meta',{}).get('revision'))
            ready.set()
    info.on_connect,info.on_message=info_connect,info_message
    info.connect(args.broker_host,url.port or 1883,30); info.loop_start()
    if not ready.wait(6): info.disconnect(); info.loop_stop(); raise RuntimeError('Runtime settings unavailable')
    time.sleep(.2); info.disconnect(); info.loop_stop()
    payload=b'C'*args.payload_bytes; digest=hashlib.sha256(payload).hexdigest(); trials=[]
    for limit in [1024*1024,16*1024*1024]:
        topic='codex-diagnostics/zbdongle-p/'+token+'/'+str(limit)
        if topic.startswith(base+'/'): raise ValueError('Diagnostic topic overlaps production')
        subscribed,received,disconnected=threading.Event(),threading.Event(),threading.Event()
        trial=dict(advertised_receive_limit=limit,payload_bytes=len(payload),received=False)
        client=mqtt.Client(client_id='codex-mqtt5-'+token+'-'+str(limit),protocol=mqtt.MQTTv5,reconnect_on_failure=False)
        authenticate(client)
        def connected(c,u,flags,reason,properties):
            trial['connect_reason']=str(reason)
            trial['broker_maximum_packet_size']=getattr(properties,'MaximumPacketSize',None)
            if reason==0: c.subscribe(topic,qos=0)
        def sub(c,u,mid,granted,properties=None): subscribed.set()
        def message(c,u,msg):
            if msg.topic==topic:
                trial['received_bytes']=len(msg.payload)
                trial['payload_identical']=hashlib.sha256(msg.payload).hexdigest()==digest
                received.set()
        def disconnect(c,u,reason,properties=None): trial['disconnect_reason']=str(reason); disconnected.set()
        client.on_connect,client.on_subscribe,client.on_message,client.on_disconnect=connected,sub,message,disconnect
        properties=Properties(PacketTypes.CONNECT); properties.MaximumPacketSize=limit
        client.connect(args.broker_host,url.port or 1883,30,properties=properties); client.loop_start()
        try:
            if not subscribed.wait(6): raise RuntimeError('Diagnostic subscription unavailable')
            server_limit=trial.get('broker_maximum_packet_size')
            if server_limit and len(payload)+len(topic)+32>server_limit:
                trial['skipped_broker_inbound_limit']=True
            else:
                started=time.perf_counter()
                client.publish(topic,payload,qos=0,retain=False)
                trial['received']=received.wait(7)
                trial['elapsed_seconds']=time.perf_counter()-started
                trial['unexpected_disconnect']=disconnected.is_set()
        finally: client.disconnect(); client.loop_stop()
        trials.append(trial)
    report=dict(runtime=runtime,synthetic_payload=True,production_topics_published=False,
        Zigbee_commands_sent=0,Z2M_restarted=False,settings_modified=False,trials=trials)
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))

if __name__=='__main__': main()
