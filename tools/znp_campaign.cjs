/* SPDX-License-Identifier: MIT
 * Hardware campaign on a LABORATORY network (docs/firmware/hardware-campaign.md):
 * the full zigbee-herdsman 11.2.1 Controller (Zigbee2MQTT minus MQTT) on the
 * FreeRTOS coordinator, persistent network/database under private/znp-lab/
 * (shared with znp_pair_test.cjs). Never use it with the production network.
 *
 *   node tools/znp_campaign.cjs --serial COM8 --baud 1000000 --join 3 [--permit 300]
 *   node tools/znp_campaign.cjs --serial COM8 --baud 1000000 --latency 50
 *   node tools/znp_campaign.cjs --serial COM8 --baud 1000000 --burst 40
 *   node tools/znp_campaign.cjs --serial COM8 --baud 1000000 --soak 1440 --interval 60
 *   node tools/znp_campaign.cjs --serial COM8 --baud 1000000 --map
 *
 * --latency N : N ZCL reads (Basic/zclVersion) per device; round trip in ms
 *               (host -> ZNP -> radio -> device -> ZCL response -> host).
 *               Sleepy devices: the read is delivered at their next poll.
 * --burst N   : N reads issued at once to every router (congestion of the
 *               coordinator's queues); counts successes and error statuses.
 * --soak M    : M rounds, one read per device every --interval seconds.
 * --map       : Mgmt_Lqi of the coordinator and every router (topology check).
 * Results: one JSON per run under private/hw-campaign/. No firmware is written.
 * Tested against the simulator only for start-up (it simulates no devices).
 */
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const r = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
const argv = process.argv.slice(2);
const opt = (name, dflt) => argv.includes(name) ? argv[argv.indexOf(name) + 1] : dflt;
const serial = opt('--serial', null);
const baud = Number(opt('--baud', '115200'));
const dir = path.resolve(ROOT, opt('--lab-dir', 'private/znp-lab'));
if (!dir.startsWith(path.join(ROOT, 'private') + path.sep)) throw new Error('--lab-dir must be under private/');
const outDir = path.join(ROOT, 'private/hw-campaign');
fs.mkdirSync(dir, {recursive: true});
fs.mkdirSync(outDir, {recursive: true});
const stamp = new Date().toISOString().replace(/[:.]/g, '-');
const out = (...a) => { const line = a.join(' '); console.log(line); fs.appendFileSync(path.join(outDir, `campaign-${stamp}.log`), `${new Date().toISOString()} ${line}\n`); };
const log = (m, ns) => fs.appendFileSync(path.join(dir, 'herdsman.log'), `${new Date().toISOString()} ${ns}: ${typeof m === 'function' ? m() : m}\n`);
r('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
const {Controller} = r('zigbee-herdsman');
const sleep = ms => new Promise(res => setTimeout(res, ms));

const netFile = path.join(dir, 'network.json');
if (!fs.existsSync(netFile)) {
    fs.writeFileSync(netFile, JSON.stringify({panID: crypto.randomInt(0x1000, 0xFFF0),
        extendedPanID: [...crypto.randomBytes(8)], networkKey: [...crypto.randomBytes(16)], channelList: [25]}));
}
const network = JSON.parse(fs.readFileSync(netFile));

function stats(values) {
    if (!values.length) return null;
    const s = [...values].sort((a, b) => a - b);
    const q = p => s[Math.min(s.length - 1, Math.floor(p * (s.length - 1) + 0.5))];
    return {n: s.length, min: s[0], median: q(0.5), p95: q(0.95), max: s[s.length - 1],
            mean: Math.round(s.reduce((a, b) => a + b, 0) / s.length)};
}

async function timedRead(device, timeout) {
    const ep = device.endpoints.find(e => e.inputClusters.includes(0)) ?? device.endpoints[0];
    if (!ep) throw new Error('no endpoint');
    const t0 = process.hrtime.bigint();
    await ep.read('genBasic', ['zclVersion'], {timeout, disableDefaultResponse: true});
    return Number(process.hrtime.bigint() - t0) / 1e6;
}

async function main() {
    if (!serial) throw new Error('--serial is required (laboratory dongle)');
    const controller = new Controller({
        network: {...network, networkKeyDistribute: false},
        transport: {path: serial, baudRate: baud, rtscts: false, adapter: 'zstack'},
        databasePath: path.join(dir, 'database.db'),
        databaseBackupPath: path.join(dir, 'database.db.backup'),
        backupPath: path.join(dir, 'coordinator_backup.json'),
        adapter: {disableLED: false, concurrent: 16},
        acceptJoiningDeviceHandler: async () => true,
    });
    const result = {started_at: new Date().toISOString(), serial, baud, pan: '0x' + network.panID.toString(16)};
    let interviewed = 0;
    controller.on('deviceInterview', e => { if (e.status === 'successful') { interviewed++; out('INTERVIEW_OK', e.device.ieeeAddr, e.device.type); } });
    controller.on('deviceJoined', e => out('JOINED', e.device.ieeeAddr));
    result.start = await controller.start();
    out('STARTED', result.start);
    try {
        const devices = () => [...controller.getDevicesIterator()].filter(d => d.type !== 'Coordinator');
        const join = Number(opt('--join', '0'));
        if (join) {
            const permit = Number(opt('--permit', '300'));
            await controller.permitJoin(permit);
            const until = Date.now() + permit * 1000;
            while (interviewed < join && Date.now() < until) await sleep(500);
            await controller.permitJoin(0);
            result.joined = interviewed;
        }
        result.devices = devices().map(d => ({ieee: d.ieeeAddr, nwk: '0x' + d.networkAddress.toString(16), type: d.type,
                                              model: d.modelID, power: d.powerSource}));
        out('DEVICES', JSON.stringify(result.devices));
        const timeoutFor = d => d.type === 'EndDevice' ? 30000 : 10000;
        const latency = Number(opt('--latency', '0'));
        if (latency) {
            result.latency_ms = {};
            for (const d of devices()) {
                const ok = [], errors = {};
                for (let i = 0; i < latency; i++) {
                    try { ok.push(await timedRead(d, timeoutFor(d))); }
                    catch (e) { const k = String(e.message).slice(0, 60); errors[k] = (errors[k] ?? 0) + 1; }
                    await sleep(d.type === 'EndDevice' ? 200 : 300);
                }
                result.latency_ms[d.ieeeAddr] = {type: d.type, ...stats(ok), failures: latency - ok.length, errors};
                out('LATENCY', d.ieeeAddr, d.type, JSON.stringify(result.latency_ms[d.ieeeAddr]));
            }
        }
        const burst = Number(opt('--burst', '0'));
        if (burst) {
            result.burst = {};
            for (const d of devices().filter(x => x.type === 'Router')) {
                const t0 = Date.now();
                const settled = await Promise.allSettled(Array.from({length: burst}, () => timedRead(d, 20000)));
                const ok = settled.filter(s => s.status === 'fulfilled').map(s => s.value);
                const errors = {};
                for (const s of settled.filter(x => x.status === 'rejected')) {
                    const k = String(s.reason?.message).slice(0, 60); errors[k] = (errors[k] ?? 0) + 1;
                }
                result.burst[d.ieeeAddr] = {requested: burst, ok: ok.length, wall_ms: Date.now() - t0, latency: stats(ok), errors};
                out('BURST', d.ieeeAddr, JSON.stringify(result.burst[d.ieeeAddr]));
            }
        }
        const soak = Number(opt('--soak', '0'));
        if (soak) {
            const interval = Number(opt('--interval', '60')) * 1000;
            result.soak = {rounds: soak, per_device: {}};
            for (let round = 0; round < soak; round++) {
                const t = Date.now();
                for (const d of devices()) {
                    const e = result.soak.per_device[d.ieeeAddr] ??= {ok: 0, failed: 0, latencies: []};
                    try { e.latencies.push(await timedRead(d, timeoutFor(d))); e.ok++; } catch { e.failed++; }
                }
                out('SOAK', round + 1, JSON.stringify(Object.fromEntries(Object.entries(result.soak.per_device).map(([k, v]) => [k, `${v.ok}/${v.ok + v.failed}`]))));
                fs.writeFileSync(path.join(outDir, `campaign-${stamp}.json`), JSON.stringify(result, null, 1));
                await sleep(Math.max(0, interval - (Date.now() - t)));
            }
            for (const v of Object.values(result.soak.per_device)) { v.latency = stats(v.latencies); delete v.latencies; }
        }
        if (argv.includes('--map')) {
            result.map = {};
            const coord = controller.getDeviceByNetworkAddress(0);
            for (const d of [coord, ...devices().filter(x => x.type === 'Router')]) {
                try { result.map[d.ieeeAddr] = await d.lqi(); }
                catch (e) { result.map[d.ieeeAddr] = {error: String(e.message).slice(0, 80)}; }
                out('LQI', d.ieeeAddr, JSON.stringify(result.map[d.ieeeAddr]).slice(0, 400));
            }
        }
    } finally {
        await controller.stop();
        result.stopped_at = new Date().toISOString();
        result.backup_written = fs.existsSync(path.join(dir, 'coordinator_backup.json'))
            ? fs.statSync(path.join(dir, 'coordinator_backup.json')).mtime.toISOString() : null;
        fs.writeFileSync(path.join(outDir, `campaign-${stamp}.json`), JSON.stringify(result, null, 1));
        out('DONE', path.join('private/hw-campaign', `campaign-${stamp}.json`));
    }
}
main().catch(e => { out('FATAL', String(e.stack ?? e).split('\n').slice(0, 3).join(' | ')); process.exitCode = 1; });
