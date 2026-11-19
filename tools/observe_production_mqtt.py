"""Bounded production observation and read-only GETs to powered routers.

No /set, association, networkmap, reset, interview or configuration requests.
Credentials come from the private saved Z2M config and are never printed.
"""
import argparse
import json
from pathlib import Path
import time
import uuid
from urllib.parse import urlparse
import yaml
import paho.mqtt.client as mqtt
from check_production_logs import last_seen

def state_property(exposes):
    for item in exposes:
        prop = item.get("property", "")
        if (prop == "state" or prop.startswith("state_")) and item.get("access", 0) & 4:
            return prop
        nested = state_property(item.get("features", []))
        if nested: return nested
    return None

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--session-dir", type=Path, required=True)
    ap.add_argument("--broker-host", required=True)
    ap.add_argument("--duration", type=int, default=150)
    ap.add_argument("--read-router-state", action="store_true")
    ap.add_argument("--read-device", help="Read state only from this router IEEE")
    ap.add_argument("--expected-revision", type=int, choices=[20261114, 20261117, 2026111801, 2026111802], default=20261114)
    ap.add_argument("--output-tag", default="")
    args = ap.parse_args()
    if args.output_tag and not args.output_tag.replace('-', '').isalnum(): raise ValueError('Invalid output tag')
    suffix = '-' + args.output_tag if args.output_tag else ''
    if args.read_device: args.read_router_state = True
    folder = args.session_dir.resolve()
    if not folder.is_relative_to((Path(__file__).resolve().parents[1] / "private").resolve()): raise ValueError("Private session required")
    if not 30 <= args.duration <= 300: raise ValueError("Bounded observation required")
    config = yaml.safe_load((folder / "z2m-data/configuration.yaml").read_text(encoding="utf-8"))
    connection = config["mqtt"]
    url = urlparse(connection["server"])
    if url.scheme != "mqtt": raise ValueError("This tool expects the configured plaintext LAN MQTT transport")
    base = connection.get("base_topic", "zigbee2mqtt")
    if any(char in base for char in "#+"): raise ValueError("Invalid base topic")
    rows = [json.loads(s) for s in (folder / "z2m-data/database.db").read_text(encoding="utf-8").splitlines() if s.strip()]
    devices = {config.get("devices", {}).get(r["ieeeAddr"], {}).get("friendly_name", r["ieeeAddr"]): r
               for r in rows if r.get("type") not in ("Coordinator", "Group")}
    migration_report = folder / "install/report.json"
    if not migration_report.is_file(): migration_report = folder / "upgrade/report.json"
    boundary = migration_report.stat().st_mtime
    old_state = json.loads((folder / "z2m-data/state.json").read_text(encoding="utf-8"))
    fresh, requests, metadata, responses = {}, {}, [], {}
    status = {"connected": False, "online": False, "expected_firmware": False}
    transport = {"connections": 0, "unexpected_disconnections": 0}
    closing = False
    transaction = 'codex-readonly-' + uuid.uuid4().hex[:12]
    client = mqtt.Client(client_id=transaction, clean_session=True)
    client.username_pw_set(connection.get("user", url.username), connection.get("password", url.password))
    def connected(c, userdata, flags, code):
        transport['connections'] += 1
        status["connected"] = code == 0
        status["connection_code"] = int(code)
        if code == 0: c.subscribe(base + '/#', qos=0)
    def received(c, userdata, message):
        nonlocal metadata
        try: payload = json.loads(message.payload)
        except (ValueError, UnicodeDecodeError): return
        topic = message.topic[len(base) + 1:]
        if topic == 'bridge/state': status["online"] = payload.get('state') == 'online'
        elif topic == 'bridge/info':
            status["firmware_revision"] = payload.get('coordinator', {}).get('meta', {}).get('revision')
            status["expected_firmware"] = str(status["firmware_revision"]) == str(args.expected_revision)
            status["Z2M_version"] = payload.get('version')
            status["herdsman_version"] = payload.get('zigbee_herdsman', {}).get('version') if isinstance(payload.get('zigbee_herdsman'), dict) else payload.get('zigbee_herdsman')
        elif topic == 'bridge/devices' and isinstance(payload, list):
            metadata = payload
            (folder / ('bridge-devices' + suffix + '.private.json')).write_text(json.dumps(payload), encoding='utf-8')
        elif topic.startswith('bridge/response/') and isinstance(payload, dict) and payload.get('transaction') == transaction:
            responses[topic.rsplit('/', 1)[-1]] = payload
        elif topic in devices and isinstance(payload, dict):
            timestamp = last_seen(payload.get('last_seen'))
            previous = last_seen(old_state.get(topic, old_state.get(devices[topic]['ieeeAddr'], {})).get('last_seen'))
            if timestamp is not None and timestamp >= boundary and (previous is None or timestamp > previous):
                fresh[topic] = {"type": devices[topic].get('type'), "last_seen": timestamp}
                if topic in requests and timestamp >= requests[topic]['sent_wall_time']-1:
                    requests[topic]['fresh_telemetry_after_get'] = True
    def disconnected(c, userdata, code):
        if not closing:
            status['connected'] = False
            transport['unexpected_disconnections'] += 1
    client.on_connect, client.on_message, client.on_disconnect = connected, received, disconnected
    client.connect(args.broker_host, url.port or 1883, keepalive=30)
    client.loop_start()
    start = time.monotonic(); checks_sent = False; last_progress = 0
    try:
        while time.monotonic() - start < args.duration:
            if status["connected"] and status["online"] and status["expected_firmware"]:
                if not checks_sent:
                    for action in ['health_check', 'coordinator_check']:
                        client.publish(base + '/bridge/request/' + action, json.dumps({'transaction': transaction}), qos=1, retain=False)
                    checks_sent = True
                pending = [r for r in requests.values() if not r.get('fresh_telemetry_after_get') and time.monotonic() - r['sent_monotonic'] < 12]
                if args.read_router_state and metadata and not pending:
                    for device in list(metadata):
                        name = device.get('friendly_name')
                        if args.read_device and device.get('ieee_address') != args.read_device: continue
                        if device.get('type') != 'Router' or device.get('disabled') or name not in devices or name in requests: continue
                        if name in fresh and not args.read_device: continue
                        prop = state_property((device.get('definition') or {}).get('exposes', []))
                        if not prop: continue
                        requests[name] = {'property': prop, 'sent_monotonic': time.monotonic(), 'sent_wall_time':time.time(), 'fresh_telemetry_after_get': False}
                        client.publish(base + '/' + name + '/get', json.dumps({prop: ''}), qos=0, retain=False)
                        break
            elapsed = int(time.monotonic() - start)
            if elapsed - last_progress >= 15:
                print(json.dumps({'elapsed_seconds': elapsed, 'fresh_devices': len(fresh), 'read_only_router_GETs': len(requests),
                    'MQTT_connected': status['connected'], 'expected_firmware': status['expected_firmware']}), flush=True)
                last_progress = elapsed
            time.sleep(.25)
    finally:
        closing = True
        client.disconnect(); client.loop_stop()
    report = dict(status=status, duration_seconds=round(time.monotonic() - start, 2),
        database_devices=len(devices), fresh_devices=len(fresh), router_GETs_sent=len(requests),
        GETs_followed_by_fresh_telemetry=sum(bool(r.get('fresh_telemetry_after_get')) for r in requests.values()),
        health_check_ok=responses.get('health_check', {}).get('status') == 'ok' and responses.get('health_check', {}).get('data', {}).get('healthy') is True,
        coordinator_check_ok=responses.get('coordinator_check', {}).get('status') == 'ok',
        missing_routers_in_coordinator=len(responses.get('coordinator_check', {}).get('data', {}).get('missing_routers', [])),
        setters_sent=0, associations_opened=False, networkmap_requested=False, transport=transport)
    (folder / ('MQTT-observation' + suffix + '.private.json')).write_text(json.dumps({'report': report, 'fresh': fresh,
        'requests': requests, 'responses': responses}, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report), flush=True)
    if not checks_sent or not report['health_check_ok'] or not report['coordinator_check_ok'] or transport['unexpected_disconnections']:
        raise RuntimeError('Production MQTT health verification incomplete')

if __name__ == '__main__': main()
