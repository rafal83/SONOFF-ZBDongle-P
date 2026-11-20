/* SPDX-License-Identifier: MIT
 * Read-only performance bench on the PRODUCTION network (Z2M stopped, dongle
 * on this PC). No adapter.start(), no commissioning, no permit join, no write
 * to any device: a whitelist of ZNP commands is enforced before sending.
 *
 *  1. identity: the network read from the dongle (NV, read-only) must match
 *     the Z2M backup (IEEE, PAN, extended PAN, channel, key, TCLK seed,
 *     counters not decreasing, previous devices present); otherwise abort;
 *  2. ZDO startupFromApp (resume the existing network, as Z2M does at every
 *     start) and wait for the coordinator state; AF register endpoint 1;
 *  3. latency: N sequential ZCL reads of genBasic.zclVersion per router
 *     (round trip host -> dongle -> radio -> device -> host);
 *  4. burst: one read to every router at once, several rounds.
 * Results: private/<out>/bench.json (device names stay private).
 *
 *   node tools/znp_prod_bench.cjs --port COM8 --baudrate 1000000 \
 *        --database <copy>/database.db --backup <copy>/coordinator_backup.json \
 *        --expect-ieee 0x... --out private/perf-YYYYMMDD-label [--reads 10] [--bursts 5]
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const lab = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));

const args = {};
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].slice(2)] = process.argv[i + 1];
for (const k of ['port', 'database', 'backup', 'expect-ieee', 'out']) if (!args[k]) throw Error(`--${k} is required`);
const out = path.resolve(args.out);
const privateRoot = fs.realpathSync(path.join(ROOT, 'private'));
if (!path.dirname(out).startsWith(privateRoot) || fs.existsSync(out)) throw Error('--out must be a new directory under private/');
fs.mkdirSync(out);
const logPath = path.join(out, 'znp.private.log');
const log = m => fs.appendFileSync(logPath, `${new Date().toISOString()} ${typeof m === 'function' ? m() : m}\n`);
lab('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
const {AdapterTransport} = lab('zigbee-herdsman/dist/adapter/transport.js');
const {Znp} = lab('zigbee-herdsman/dist/adapter/z-stack/znp/index.js');
const {AdapterNvMemory} = lab('zigbee-herdsman/dist/adapter/z-stack/adapter/adapter-nv-memory.js');
const {AdapterBackup} = lab('zigbee-herdsman/dist/adapter/z-stack/adapter/adapter-backup.js');
const {BackupUtils} = lab('zigbee-herdsman/dist/utils/index.js');
const {Subsystem, Type} = lab('zigbee-herdsman/dist/adapter/z-stack/unpi/constants.js');

const READS = Number(args.reads ?? 10), BURSTS = Number(args.bursts ?? 5), TIMEOUT = 4000;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const quantile = (v, q) => { const s = [...v].sort((a, b) => a - b); return s.length ? s[Math.min(s.length - 1, Math.floor(q * (s.length - 1) + 0.5))] : null; };
const stats = v => ({n: v.length, median_ms: quantile(v, 0.5), p95_ms: quantile(v, 0.95), max_ms: v.length ? Math.max(...v) : null});

/* Same identity rules as tools/production_nv_snapshot.cjs. */
function compareNetwork(before, after) {
    const same = (a, b, f) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw Error(`Network mismatch: ${f}`); };
    for (const f of ['coordinator_ieee', 'pan_id', 'extended_pan_id', 'channel', 'channel_mask', 'security_level'])
        same(before[f], after[f], f);
    same(before.network_key.key, after.network_key.key, 'network key');
    same(before.network_key.sequence_number, after.network_key.sequence_number, 'key sequence');
    same(before.stack_specific?.zstack?.tclk_seed, after.stack_specific?.zstack?.tclk_seed, 'TCLK seed');
    if (after.network_key.frame_counter < before.network_key.frame_counter) throw Error('Network frame counter decreased');
    const devices = new Set(after.devices.map(d => d.ieee_address));
    for (const p of before.devices) if (!devices.has(p.ieee_address)) throw Error('A previous network device is missing');
}

async function main() {
    const baudRate = Number(args.baudrate ?? 1000000);
    const znp = new Znp(new AdapterTransport({path: args.port, baudRate, rtscts: false}));
    const raw = znp.request.bind(znp), sent = {};
    const READ_ONLY_SYS = new Set(['ping', 'version', 'getExtAddr', 'osalNvLength', 'osalNvReadExt', 'nvLength', 'nvRead']);
    znp.request = async (subsystem, name, payload, ...rest) => {
        const ok = (subsystem === Subsystem.SYS && READ_ONLY_SYS.has(name)) ||
            (subsystem === Subsystem.UTIL && name === 'getDeviceInfo') ||
            (subsystem === Subsystem.ZDO && name === 'startupFromApp') ||
            (subsystem === Subsystem.AF && name === 'register' && payload.endpoint === 1) ||
            (subsystem === Subsystem.AF && name === 'dataRequest' && payload.clusterid === 0 && payload.data[2] === 0x00 &&
             payload.data.length === 5);   /* ZCL read (cmd 0x00) of one Basic attribute */
        if (!ok) throw Error(`ZNP command blocked by the read-only whitelist: ${subsystem}/${name}`);
        sent[name] = (sent[name] ?? 0) + 1;
        return raw(subsystem, name, payload, ...rest);
    };
    const report = {started_at: new Date().toISOString(), port: args.port, baudRate, read_only: true,
                    adapter_start_called: false, permit_join: false, device_writes: 0};
    try {
        await znp.open();
        const version = (await znp.requestWithReply(Subsystem.SYS, 'version', {})).payload;
        report.firmware = {product: version.product, revision: version.revision};

        /* 1. identity */
        const rows = fs.readFileSync(args.database, 'utf8').split(/\r?\n/).filter(x => x.trim()).map(x => JSON.parse(x));
        const nv = new AdapterNvMemory(znp);
        await nv.init();
        const current = BackupUtils.toUnifiedBackup(await new AdapterBackup(znp, nv, args.backup).createBackup(rows.map(d => d.ieeeAddr)));
        if ('0x' + current.coordinator_ieee !== args['expect-ieee'].toLowerCase()) throw Error('Coordinator identity mismatch');
        const previous = JSON.parse(fs.readFileSync(args.backup, 'utf8'));
        const active = new Set(rows.filter(d => !['Coordinator', 'Group'].includes(d.type)).map(d => d.ieeeAddr));
        previous.devices = previous.devices.filter(d => active.has('0x' + d.ieee_address));
        compareNetwork(previous, current);
        report.identity = {checked: true, channel: current.channel, devices_in_dongle: current.devices.length};
        console.log('identity ok', JSON.stringify(report.identity));

        /* 2. resume the existing network */
        const info0 = (await znp.requestWithReply(Subsystem.UTIL, 'getDeviceInfo', {})).payload;
        if (info0.devicestate !== 9) {
            await znp.request(Subsystem.ZDO, 'startupFromApp', {startdelay: 100});
            for (let i = 0; i < 50; i++) {
                await sleep(200);
                if ((await znp.requestWithReply(Subsystem.UTIL, 'getDeviceInfo', {})).payload.devicestate === 9) break;
            }
        }
        const info = (await znp.requestWithReply(Subsystem.UTIL, 'getDeviceInfo', {})).payload;
        assert.equal(info.devicestate, 9, 'coordinator not started');
        try {
            await znp.request(Subsystem.AF, 'register', {endpoint: 1, appprofid: 0x0104, appdeviceid: 0x0005, appdevver: 0,
                latencyreq: 0, appnuminclusters: 0, appinclusterlist: [], appnumoutclusters: 0, appoutclusterlist: []});
        } catch (e) {
            report.register_note = String(e.message ?? e).slice(0, 120);   /* already registered (Z-Stack keeps it) */
        }
        report.resume = {state_before: info0.devicestate, state: info.devicestate};
        console.log('network resumed', JSON.stringify(report.resume));

        /* targets: routers with a Basic server cluster */
        const routers = rows.filter(d => d.type === 'Router').map(d => {
            const ep = Object.values(d.endpoints ?? {}).find(e => (e.inClusterList ?? []).includes(0));
            return ep ? {ieee: d.ieeeAddr, nwk: d.nwkAddr, ep: ep.epId, model: d.modelId} : null;
        }).filter(Boolean);
        report.routers = routers.length;

        let seq = 1, transid = 1;
        const pending = new Map();   /* key nwk:seq -> resolve */
        znp.on('received', obj => {
            if (obj.type !== Type.AREQ || obj.subsystem !== Subsystem.AF || obj.command.name !== 'incomingMsg') return;
            const p = obj.payload;
            if (p.clusterid !== 0 || p.data.length < 3 || p.data[2] !== 0x01) return;
            const k = `${p.srcaddr}:${p.data[1]}`;
            const w = pending.get(k);
            if (w) { pending.delete(k); w({lqi: p.linkquality}); }
        });
        async function read(r) {
            const s = seq = (seq % 255) + 1;
            const k = `${r.nwk}:${s}`;
            const t0 = process.hrtime.bigint();
            const done = new Promise(res => { pending.set(k, res); setTimeout(() => { if (pending.delete(k)) res(null); }, TIMEOUT); });
            try {
                await znp.request(Subsystem.AF, 'dataRequest', {dstaddr: r.nwk, destendpoint: r.ep, srcendpoint: 1, clusterid: 0,
                    transid: (transid = (transid % 255) + 1), options: 0, radius: 30, len: 5, data: Buffer.from([0x00, s, 0x00, 0x00, 0x00])});
            } catch (e) { pending.delete(k); return {ok: false, error: String(e.message ?? e)}; }
            const got = await done;
            return got ? {ok: true, ms: Number(process.hrtime.bigint() - t0) / 1e6, lqi: got.lqi} : {ok: false, error: 'timeout'};
        }

        /* 3. sequential latency */
        const all = [], perDevice = [];
        for (const r of routers) {
            const v = [];
            let fails = 0;
            for (let i = 0; i < READS; i++) {
                const res = await read(r);
                if (res.ok) { v.push(res.ms); all.push(res.ms); } else fails++;
                await sleep(150);
            }
            perDevice.push({nwk: r.nwk, ep: r.ep, ...stats(v), failures: fails});
            console.log(`latency 0x${r.nwk.toString(16)} median ${quantile(v, 0.5)?.toFixed(1)} ms, ${fails} failed`);
        }
        report.latency = {reads_per_router: READS, all: stats(all),
                          failures: perDevice.reduce((a, d) => a + d.failures, 0), per_router: perDevice};

        /* 4. bursts: every router at once */
        const bursts = [];
        for (let b = 0; b < BURSTS; b++) {
            const t0 = Date.now();
            const res = await Promise.all(routers.map(r => read(r)));
            const ok = res.filter(x => x.ok).map(x => x.ms);
            bursts.push({sent: res.length, ok: ok.length, ...stats(ok), wall_ms: Date.now() - t0,
                         errors: res.filter(x => !x.ok).map(x => x.error)});
            console.log(`burst ${b + 1}: ${ok.length}/${res.length} ok, median ${quantile(ok, 0.5)?.toFixed(1)} ms, max ${Math.max(...ok).toFixed(1)} ms`);
            await sleep(3000);
        }
        report.bursts = bursts;
        report.passed = true;
    } finally {
        report.znp_commands_sent = sent;
        report.finished_at = new Date().toISOString();
        fs.writeFileSync(path.join(out, 'bench.json'), JSON.stringify(report, null, 2) + '\n');
        await znp.close().catch(() => {});
    }
    console.log(JSON.stringify({passed: report.passed ?? false, all: report.latency?.all, out: path.relative(ROOT, out)}));
}
main().catch(e => { console.error(e); process.exitCode = 1; });
