"""Bounded one-bulb speed demo: brightness, burst, or on/off/colour show.

No group commands, pairing or persistent device options. The bulb is left
ON at brightness 254. Show mode restores the starting colour on completion.
"""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import statistics
import threading
import time
from urllib.parse import urlparse
import uuid
import yaml
import paho.mqtt.client as mqtt

SHOW_COLORS = [
    ('red', .70, .30), ('green', .17, .70), ('blue', .14, .05),
    ('yellow', .44, .52), ('cyan', .16, .34), ('magenta', .38, .16),
]

def show_action(index):
    if index % 2: return dict(state='OFF', transition=0)
    _, x, y = SHOW_COLORS[(index // 2) % len(SHOW_COLORS)]
    return dict(state='ON', brightness=254, color=dict(x=x, y=y), transition=0)

def matches_action(state, action):
    if state.get('state') != action['state']: return False
    if 'color' in action:
        color = state.get('color') or {}
        if any(not isinstance(color.get(axis), (int, float)) or abs(color[axis] - action['color'][axis]) > .025
               for axis in ['x', 'y']): return False
    if 'color_temp' in action and state.get('color_temp') != action['color_temp']: return False
    return True

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session-dir', required=True, type=Path)
    ap.add_argument('--broker-host', required=True)
    ap.add_argument('--device', required=True)
    ap.add_argument('--expected-revision', type=int, choices=[20261114, 20261117], help='Otherwise accept either known project revision on the saved coordinator')
    modes = ap.add_mutually_exclusive_group()
    modes.add_argument('--burst', action='store_true', help='Pipelined brightness changes, with final independent ZCL readback')
    modes.add_argument('--show', action='store_true', help='Rapid ON/colour, OFF, next colour; restores starting colour')
    ap.add_argument('--window', type=int, choices=[4, 8, 16], default=4)
    ap.add_argument('--rate', type=float, default=8, help='Show actions per second, 0.5..20; actual rate depends on replies')
    ap.add_argument('--seconds', type=float, default=10, help='Show duration, 1..30 seconds')
    args = ap.parse_args()
    if not math.isfinite(args.rate) or not .5 <= args.rate <= 20: ap.error('--rate must be between 0.5 and 20')
    if not math.isfinite(args.seconds) or not 1 <= args.seconds <= 30: ap.error('--seconds must be between 1 and 30')
    folder = args.session_dir.resolve()
    root = Path(__file__).resolve().parents[1]
    if not folder.is_relative_to((root / 'private').resolve()): raise ValueError('Private session required')
    if args.device != '0x842e14fffe2fadf0': raise ValueError('Only the explicitly requested bulb is authorized here')
    metadata = json.loads((folder / 'bridge-devices.private.json').read_text())
    device = next(d for d in metadata if d.get('ieee_address') == args.device)
    if (device.get('definition') or {}).get('model') != 'HG06106C': raise ValueError('Unexpected device model')
    name = device['friendly_name']
    expected_ieee = json.loads((folder / 'z2m-snapshot.json').read_text())['ieee'].lower()
    config = yaml.safe_load((folder / 'z2m-data/configuration.yaml').read_text(encoding='utf-8'))
    connection = config['mqtt']; url = urlparse(connection['server']); base = connection.get('base_topic', 'zigbee2mqtt')
    if url.scheme != 'mqtt': raise ValueError('Expected existing LAN MQTT setup')
    token = 'codex-light-demo-' + uuid.uuid4().hex[:10]
    output = folder / token
    output.mkdir()
    condition = threading.Condition()
    shared = dict(connected=False, online=False, firmware=False, sequence=0, state={})
    reads, errors, measurements = [], [], []
    pending, completions = {}, []
    client = mqtt.Client(client_id=token, clean_session=True)
    client.username_pw_set(connection.get('user', url.username), connection.get('password', url.password))
    def connected(c, userdata, flags, code):
        with condition:
            shared['connected'] = code == 0
            if code == 0:
                c.subscribe([(base + '/' + name, 0), (base + '/bridge/logging', 0),
                             (base + '/bridge/state', 0), (base + '/bridge/info', 0)])
            condition.notify_all()
    def received(c, userdata, msg):
        try: payload = json.loads(msg.payload)
        except (ValueError, UnicodeDecodeError): return
        with condition:
            if msg.topic == base + '/bridge/state': shared['online'] = payload.get('state') == 'online'
            elif msg.topic == base + '/bridge/info':
                coordinator = payload.get('coordinator', {})
                revision = str(coordinator.get('meta', {}).get('revision'))
                allowed = {str(args.expected_revision)} if args.expected_revision else {'20261114', '20261117'}
                shared['firmware'] = revision in allowed and str(coordinator.get('ieee_address', '')).lower() == expected_ieee
                shared['revision'] = revision
            elif msg.topic == base + '/' + name and not msg.retain:
                shared['sequence'] += 1; shared['state'] = payload
                value = payload.get('brightness')
                if value in pending:
                    sent = pending.pop(value)
                    completions.append(dict(brightness=value, sent_monotonic=sent,
                        received_monotonic=time.perf_counter(), MQTT_set_to_state_ms=(time.perf_counter()-sent)*1000))
            elif msg.topic == base + '/bridge/logging':
                message = payload.get('message', '')
                marker = message.find("Read result of '")
                if marker >= 0:
                    message_read = message[marker:]
                    try: reads.append((time.perf_counter(), message_read.split("'",2)[1], json.loads(message_read.split(': ',1)[1])))
                    except (ValueError, IndexError): pass
                if payload.get('level') == 'error' and (name in message or args.device in message):
                    errors.append(message)
            condition.notify_all()
    client.on_connect, client.on_message = connected, received
    def wait(predicate, timeout=4):
        deadline = time.monotonic() + timeout
        with condition:
            while not predicate():
                if errors: raise RuntimeError('Z2M reported an error for the requested bulb')
                remaining = deadline - time.monotonic()
                if remaining <= 0: raise TimeoutError('Requested bulb response deadline exceeded')
                condition.wait(remaining)
    def publish(data):
        result = client.publish(base + '/' + name + '/set', json.dumps(data), qos=0, retain=False)
        if result.rc != mqtt.MQTT_ERR_SUCCESS: raise RuntimeError('MQTT publish failed')
    def level(value, power=False):
        with condition: sequence = shared['sequence']
        start = time.perf_counter()
        command = dict(brightness=value, transition=0)
        if power: command['state'] = 'ON'
        publish(command)
        wait(lambda: shared['sequence'] > sequence and shared['state'].get('brightness') == value)
        acknowledged = time.perf_counter()
        with condition: read_start = len(reads)
        publish({'read': {'cluster': 'genLevelCtrl', 'attributes': ['currentLevel']}})
        wait(lambda: any(cluster == 'genLevelCtrl' and data.get('currentLevel') == value
                         for _,cluster,data in reads[read_start:]))
        end = time.perf_counter()
        return dict(brightness=value, MQTT_set_to_state_ms=(acknowledged-start)*1000,
                    set_and_ZCL_readback_ms=(end-start)*1000)
    def set_action(action):
        with condition: sequence = shared['sequence']
        started = time.perf_counter()
        publish(action)
        wait(lambda: shared['sequence'] > sequence and matches_action(shared['state'], action))
        return (time.perf_counter() - started) * 1000
    report = dict(passed=False, target=args.device, transition_seconds=0, synthetic_responses=False,
                  method='Sequential set, MQTT state, then actual genLevelCtrl.currentLevel ZCL read result',
                  on_off_strobe=False, final_requested_state='ON', final_requested_brightness=254)
    client.connect(args.broker_host, url.port or 1883, keepalive=30); client.loop_start()
    activated = False
    starting_colour = None
    try:
        wait(lambda: shared['connected'] and shared['online'] and shared['firmware'], timeout=8)
        report['firmware_revision'] = shared['revision']
        activated = True
        level(254, power=True)
        if args.show:
            with condition:
                state = dict(shared['state'])
                if state.get('color_mode') == 'color_temp' and isinstance(state.get('color_temp'), (int,float)):
                    starting_colour = dict(color_temp=state['color_temp'])
                elif isinstance(state.get('color'), dict) and all(axis in state['color'] for axis in ['x','y']):
                    starting_colour = dict(color=dict(state['color']))
            print(f'ON/OFF and six-colour show: {args.seconds:g}s, up to {args.rate:g} actions/s, transition 0', flush=True)
            started = time.perf_counter()
            index = 0
            while time.perf_counter() - started < args.seconds:
                action = show_action(index)
                sent = time.perf_counter()
                latency = set_action(action)
                measurements.append(dict(index=index, command=action, MQTT_set_to_state_ms=latency))
                index += 1
                remaining = min(sent + 1 / args.rate, started + args.seconds) - time.perf_counter()
                if remaining > 0: time.sleep(remaining)
            duration = time.perf_counter() - started
            report.update(passed=True, method='Paced ON/colour then OFF; wait for Z2M state acknowledgement per action',
                on_off_strobe=True, requested_actions_per_second=args.rate, requested_duration_seconds=args.seconds,
                acknowledged_actions=index, duration_seconds=duration, actions_per_second=index/duration,
                colour_commands=(index+1)//2, OFF_commands=index//2,
                each_show_action_individually_read_back=False, optical_latency_measured=False)
            return
        print('Bulb ON at maximum brightness; starting immediate verified changes', flush=True)
        started = time.perf_counter()
        if args.burst:
            targets = list(range(64, 224))
            index = 0
            while index < len(targets) or pending:
                if time.perf_counter() - started > 10: raise TimeoutError('Bounded burst duration reached')
                with condition:
                    if errors: raise RuntimeError('Z2M reported a target-device error')
                    while index < len(targets) and len(pending) < args.window:
                        target = targets[index]; index += 1
                        pending[target] = time.perf_counter()
                        publish(dict(brightness=target, transition=0))
                    if pending: condition.wait(.02)
            duration = time.perf_counter() - started
            with condition: read_start = len(reads)
            publish({'read': {'cluster': 'genLevelCtrl', 'attributes': ['currentLevel']}})
            wait(lambda: any(cluster == 'genLevelCtrl' and data.get('currentLevel') == targets[-1]
                             for _,cluster,data in reads[read_start:]))
            measurements.extend(completions)
            report.update(passed=True, method=f'At most {args.window} sets in flight; Z2M state acknowledgements; final independent ZCL readback',
                Z2M_acknowledged_changes=len(completions), duration_seconds=duration,
                Z2M_acknowledged_changes_per_second=len(completions)/duration,
                max_in_flight=args.window, final_burst_level_verified_by_ZCL=True,
                each_burst_level_individually_read_back=False,
                maximum_hardware_throughput_established=False, optical_latency_measured=False)
            return
        targets = [64 + 8*i for i in range(24)] + [247 - 8*i for i in range(24)]
        for index, target in enumerate(targets):
            if time.perf_counter() - started > 20: raise TimeoutError('Bounded demo duration reached')
            measurements.append(level(target))
        duration = time.perf_counter() - started
        latencies = sorted(m['set_and_ZCL_readback_ms'] for m in measurements)
        report.update(passed=True, verified_changes=len(measurements), duration_seconds=duration,
            verified_changes_per_second=len(measurements)/duration,
            cycle_ms=dict(min=min(latencies), median=statistics.median(latencies),
                          p95=latencies[math.ceil(.95*len(latencies))-1], max=max(latencies)),
            maximum_hardware_throughput_established=False, optical_latency_measured=False)
    finally:
        if activated:
            try:
                if args.show and starting_colour:
                    set_action(dict(state='ON', brightness=254, transition=0, **starting_colour))
                    report['starting_colour_restored_at_Z2M'] = True
                report['final_brightness_verified'] = level(254, power=True)['brightness'] == 254
                with condition: read_start = len(reads)
                publish({'read': {'cluster': 'genOnOff', 'attributes': ['onOff']}})
                wait(lambda: any(cluster == 'genOnOff' and data.get('onOff') in [1, True]
                                 for _,cluster,data in reads[read_start:]))
                report['final_ON_verified'] = True
            except Exception:
                report['final_verification_failed'] = True
        client.disconnect(); client.loop_stop()
        report['finished_UTC'] = datetime.now(timezone.utc).isoformat()
        report['device_error_count'] = len(errors)
        (output / 'measurements.json').write_text(json.dumps(measurements, indent=2) + '\n')
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        (output / 'errors.private.json').write_text(json.dumps(errors, indent=2) + '\n')
        if args.burst or args.show: print(json.dumps(report), flush=True)
    print(json.dumps(report), flush=True)

if __name__ == '__main__': main()
