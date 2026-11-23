#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Multi-day production report of the FreeRTOS coordinator.

Reads the history written by the Zigbee2MQTT extension (coordinator-diag.js:
<zigbee2mqtt data>/coordinator-diag-history.jsonl, one line per read) or
Zigbee2MQTT log files (MQTT publications of bridge/coordinator_diag), and
prints hourly rates: many-to-one requests, router restarts detected,
re-associations, Trust Center rejoins, APS authentication failures, frames
without ACK and channel access failures. Counters restart with the dongle:
each uptime segment is counted on its own, the first WARMUP_S of a segment
excluded from the radio shares.

    python tools/z2m/prod_report.py HISTORY.jsonl [LOG ...] [--bucket-h 6] [--since 2026-11-23T08:30]
"""
import argparse
import datetime as dt
import json
import re
import sys

WARMUP_S = 600
RATES = [('mtorr', 'demandes many-to-one'), ('early_mtorr', 'dont anticipées'), ('restarts', 'redémarrages de routeurs'),
         ('rejoins', 'réassociations (Rejoin)'), ('tc_rejoins', 'rejoins Trust Center'), ('assoc', 'associations'),
         ('update_device', 'Update Device'), ('leaves', 'départs'), ('aps_auth', 'échecs APS'),
         ('nwk_auth', 'échecs NWK'), ('unanswered', 'lectures sans réponse'), ('route_failures', 'routes en échec'),
         ('bcast_dup', 'échos de broadcast')]


def from_log_payload(d):
    c, m, r = d.get('coordinator', {}), d.get('mesh', {}), d.get('reliability', {})
    return {'t': d.get('time'), 'up': d.get('info', {}).get('uptime_s'), 'commit': d.get('info', {}).get('source_commit'),
            'tx': c.get('tx_frames'), 'no_ack': c.get('tx_no_ack'), 'busy': c.get('tx_busy'), 'retries': c.get('tx_retries'),
            'mtorr': m.get('mtorr_tx'), 'early_mtorr': m.get('early_mtorr'), 'restarts': m.get('neighbor_restarts'),
            'bcast_dup': m.get('broadcast_duplicates'), 'rejoins': m.get('rejoins'), 'tc_rejoins': m.get('tc_rejoins'),
            'assoc': c.get('associations_accepted'), 'update_device': r.get('update_device'), 'leaves': r.get('leaves'),
            'aps_auth': c.get('aps_auth_failures'), 'nwk_auth': c.get('nwk_auth_failures'),
            'unanswered': r.get('reads_unanswered'), 'route_failures': m.get('route_failures'),
            'neighbors': m.get('neighbors'), 'cpu': d.get('perf', {}).get('cpu_load_permille')}


def load(paths):
    rows = []
    for p in paths:
        for line in open(p, encoding='utf-8', errors='replace'):
            line = line.strip()
            if line.startswith('{'):
                try: rows.append(json.loads(line))
                except ValueError: pass
                continue
            if "topic 'zigbee2mqtt/bridge/coordinator_diag'" not in line: continue
            m = re.search(r"payload '(\{.*\})'\s*$", line)
            if not m: continue
            try: rows.append(from_log_payload(json.loads(m.group(1))))
            except ValueError: pass
    rows = [r for r in rows if r.get('t') and r.get('up') is not None]
    rows.sort(key=lambda r: r['t'])
    out = []
    for r in rows:   # duplicates (history + logs of the same read)
        if not out or r['t'] != out[-1]['t']: out.append(r)
    return out


def ts(r):
    return dt.datetime.fromisoformat(r['t'].replace('Z', '+00:00'))


def segments(rows):
    """Runs of reads of one uptime of the dongle (no counter going back)."""
    seg = []
    for r in rows:
        if seg and (r['up'] < seg[-1]['up'] or (r.get('tx') or 0) < (seg[-1].get('tx') or 0)):
            yield seg
            seg = []
        seg.append(r)
    if seg: yield seg


def delta(a, b, k):
    if a.get(k) is None or b.get(k) is None: return None
    return b[k] - a[k]


def report(rows, title):
    hours, sums, tx = 0.0, {k: 0 for k, _ in RATES}, {'tx': 0, 'no_ack': 0, 'busy': 0}
    commits, restarts_of_dongle = set(), -1
    for seg in segments(rows):
        restarts_of_dongle += 1
        commits.update(r.get('commit') for r in seg if r.get('commit'))
        if len(seg) < 2: continue
        hours += (ts(seg[-1]) - ts(seg[0])).total_seconds() / 3600
        for k, _ in RATES:
            d = delta(seg[0], seg[-1], k)
            if d is not None: sums[k] += d
        warm = [r for r in seg if r['up'] >= WARMUP_S]
        if len(warm) >= 2:
            for k in tx:
                d = delta(warm[0], warm[-1], k)
                if d is not None: tx[k] += d
    print(f"== {title}: {rows[0]['t'][:16]} -> {rows[-1]['t'][:16]}, {hours:.1f} h, {len(rows)} lectures, "
          f"{restarts_of_dongle} redémarrage(s) du dongle, image(s) {', '.join(sorted(commits)) or '?'}")
    if hours <= 0: return
    for k, label in RATES:
        print(f"   {label:28s} {sums[k]:8d}  {sums[k] / hours:9.1f} /h")
    if tx['tx']:
        print(f"   {'trames émises':28s} {tx['tx']:8d}  {tx['tx'] / hours:9.1f} /h (hors {WARMUP_S // 60} min après démarrage)")
        print(f"   {'sans ACK':28s} {100 * tx['no_ack'] / tx['tx']:8.1f} %")
        print(f"   {'accès au canal refusé':28s} {100 * tx['busy'] / tx['tx']:8.1f} %")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='+')
    ap.add_argument('--bucket-h', type=float, default=0, help='also report per bucket of this many hours')
    ap.add_argument('--since', help='ISO time (UTC) of the first read to count')
    a = ap.parse_args()
    rows = load(a.files)
    if a.since: rows = [r for r in rows if r['t'] >= a.since]
    if len(rows) < 2: sys.exit('fewer than two reads')
    report(rows, 'ensemble')
    if a.bucket_h:
        start, step = ts(rows[0]), dt.timedelta(hours=a.bucket_h)
        while start < ts(rows[-1]):
            part = [r for r in rows if start <= ts(r) < start + step]
            if len(part) >= 2: report(part, f'tranche de {a.bucket_h:g} h')
            start += step


if __name__ == '__main__':
    main()
