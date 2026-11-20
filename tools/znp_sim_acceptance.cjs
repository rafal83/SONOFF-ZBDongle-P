/* SPDX-License-Identifier: MIT
 * ZNP acceptance test: the REAL zigbee-herdsman 11.2.1 ZStackAdapter
 * (the Zigbee2MQTT `zstack` adapter) against the FreeRTOS coordinator's
 * portable stack running in the host simulator (build/freertos/sim/znp_sim),
 * over tcp://127.0.0.1:<port>. Nothing is mocked on the host side; on the
 * simulator side only Flash, radio and TRNG are simulated (see znp_sim.c).
 *
 * Sequence (same as tools/herdsman_lab.cjs, plus device preservation):
 *  1. fresh start -> "reset" (commissioning), permit join, version, IEEE,
 *     network parameters, local node/active/simple descriptors (1, 242);
 *  2. backup; soft reset; restart -> "resumed"; same parameters and key,
 *     NWK frame counter not decreasing; local AF loopback (ZCL read);
 *  3. clear the adapter; restart with a backup that contains devices
 *     (one seed-derived TC link key, one explicit key) -> "restored";
 *     the backup taken afterwards must give back the same devices, link
 *     keys and a frame counter >= the restored one.
 *
 *   node tools/znp_sim_acceptance.cjs [--product 1] [--keep]
 *   node tools/znp_sim_acceptance.cjs --serial COM8 [--baud 115200]   (FreeRTOS image on the dongle)
 */
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const r = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
assert.equal(r('zigbee-herdsman/package.json').version, '11.2.1');

const argv = process.argv.slice(2);
const opt = (name, dflt) => argv.includes(name) ? argv[argv.indexOf(name) + 1] : dflt;
const product = opt('--product', '1');
const serial = opt('--serial', null);
const baud = Number(opt('--baud', '115200'));
let port = 0;   /* chosen by the OS, read back from the simulator */
const dir = path.join(ROOT, serial ? 'private/znp-dongle' : 'build/freertos/sim', 'run-' + Date.now());
fs.mkdirSync(dir, {recursive: true});
const sim = path.join(ROOT, 'build/freertos/sim', process.platform === 'win32' ? 'znp_sim.exe' : 'znp_sim');
const image = path.join(dir, 'flash.bin');

const log = (message, ns) => fs.appendFileSync(path.join(dir, 'herdsman.log'),
    `${new Date().toISOString()} ${ns}: ${typeof message === 'function' ? message() : message}\n`);
r('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
const {ZStackAdapter} = r('zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js');
const Zdo = r('zigbee-herdsman/dist/zspec/zdo/index.js');
const Zcl = r('zigbee-herdsman/dist/zspec/zcl/index.js');
const {BackupUtils} = r('zigbee-herdsman/dist/utils/index.js');

const report = {passed: false, simulator: !serial, serial_port: serial, product: serial ? undefined : Number(product), steps: []};
let child;
function startSim() {
    if (serial) return Promise.resolve();
    child = spawn(sim, ['0', image, product], {stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', d => fs.appendFileSync(path.join(dir, 'sim.log'), d));
    return new Promise((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('simulator did not start')), 5000);
        child.stderr.on('data', d => {
            const m = /listening on 127\.0\.0\.1:(\d+)/.exec(String(d));
            if (m) { port = Number(m[1]); clearTimeout(t); resolve(); }
        });
    });
}

async function run() {
    await startSim();
    const network = {panID: crypto.randomInt(1, 65000), extendedPanID: [...crypto.randomBytes(8)],
        networkKey: [...crypto.randomBytes(16)], networkKeyDistribute: false, channelList: [25]};
    const backupPath = path.join(dir, 'coordinator_backup.json');
    const serialOptions = serial ? {path: serial, adapter: 'zstack', baudRate: baud, rtscts: false}
                                 : {path: `tcp://127.0.0.1:${port}`, adapter: 'zstack'};
    const makeAdapter = () => new ZStackAdapter(network, serialOptions,
        backupPath, {disableLED: false, transmitPower: 5, concurrent: 2});
    let adapter;
    const step = async (name, fn) => {
        report.current = name;
        const t0 = Date.now();
        const value = await fn();
        report.steps.push({name, ms: Date.now() - t0});
        return value;
    };
    const ieee = await (async () => { adapter = makeAdapter(); return null; })();
    void ieee;
    const query = async (coordIeee, id, ...values) => {
        const response = await adapter.sendZdo(coordIeee, 0, id, Zdo.Buffalo.buildRequest(false, id, ...values), false);
        assert(response && response[0] === 0, 'local ZDO response must succeed, got ' + JSON.stringify(response));
        return response[1];
    };
    try {
        report.first_start = await step('start (fresh)', () => adapter.start());
        assert.equal(report.first_start, 'reset');
        await step('permit join coordinator', () => adapter.permitJoin(0, 0));
        await step('permit join broadcast', () => adapter.permitJoin(10));
        const version = await adapter.getCoordinatorVersion();
        report.version = version;
        const coordIeee = await adapter.getCoordinatorIEEE();
        const parameters = await step('network parameters', () => adapter.getNetworkParameters());
        assert.equal(parameters.panID, network.panID);
        assert.equal(parameters.channel, 25);
        assert.equal(parameters.extendedPanID, '0x' + Buffer.from(network.extendedPanID).toString('hex'));
        const node = await step('node descriptor', () => query(coordIeee, Zdo.ClusterId.NODE_DESCRIPTOR_REQUEST, 0));
        assert.equal(node.logicalType, 0);
        const eps = await step('active endpoints', () => query(coordIeee, Zdo.ClusterId.ACTIVE_ENDPOINTS_REQUEST, 0));
        assert(eps.endpointList.includes(1) && eps.endpointList.includes(242), JSON.stringify(eps));
        for (const ep of [1, 242]) {
            const d = await step('simple descriptor ' + ep, () => query(coordIeee, Zdo.ClusterId.SIMPLE_DESCRIPTOR_REQUEST, 0, ep));
            assert.equal(d.endpoint, ep);
        }
        const backup = await step('backup', () => adapter.backup([]));
        assert.equal(backup.networkOptions.panId, network.panID);
        assert.deepEqual(backup.networkOptions.networkKey, Buffer.from(network.networkKey));
        assert(backup.znp.trustCenterLinkKeySeed && backup.znp.trustCenterLinkKeySeed.length === 16, 'TCLK seed in backup');
        fs.writeFileSync(backupPath, JSON.stringify(BackupUtils.toUnifiedBackup(backup), null, 2) + '\n');
        await step('soft reset', () => adapter.reset('soft'));
        await adapter.stop(); adapter = undefined;
        adapter = makeAdapter();
        report.second_start = await step('start (resume)', () => adapter.start());
        assert.equal(report.second_start, 'resumed');
        assert.deepEqual(await adapter.getNetworkParameters(), parameters);
        const after = await step('backup after resume', () => adapter.backup([]));
        assert.deepEqual(after.networkOptions.networkKey, backup.networkOptions.networkKey);
        assert(after.networkKeyInfo.frameCounter >= backup.networkKeyInfo.frameCounter);
        await step('AF loopback ZCL', async () => {
            const frame = Zcl.Frame.create(Zcl.FrameType.GLOBAL, Zcl.Direction.CLIENT_TO_SERVER,
                true, undefined, 173, 'read', 0x0000, [{attrId: 0x0004}], {});
            let received = false;
            const listener = p => {
                if (p.address === 0 && p.clusterID === 0 && p.endpoint === 1 && p.destinationEndpoint === 1 &&
                    p.data.equals(frame.toBuffer())) received = true;
            };
            adapter.on('zclPayload', listener);
            try {
                await adapter.sendZclFrameToEndpoint(coordIeee, 0, 1, frame, 5000, true, true, 1);
                for (let i = 0; i < 100 && !received; i++) await new Promise(res => setTimeout(res, 20));
                assert(received, 'loopback ZCL frame must come back as incoming message');
            } finally { adapter.removeListener('zclPayload', listener); }
        });

        /* Device preservation through restoreBackup: a synthetic backup with
         * two devices (clearly test data). Device A's key is derived from the
         * backup's TC link key seed (shift 5), device B's key is arbitrary. */
        const seed = backup.znp.trustCenterLinkKeySeed;
        const ieeeA = Buffer.from('00124b0011223344', 'hex'), ieeeB = Buffer.from('a4c1380000000001', 'hex');
        const derive = (ieeeBE, shift) => {
            const rot = Buffer.concat([seed.subarray(shift), seed.subarray(0, shift)]);
            const rev = Buffer.from(ieeeBE).reverse();
            return Buffer.from(rot.map((b, i) => b ^ rev[i % 8]));
        };
        const devices = [
            {networkAddress: 0x1A2B, ieeeAddress: ieeeA, isDirectChild: true,
                linkKey: {key: derive(ieeeA, 5), rxCounter: 100, txCounter: 200}},
            {networkAddress: 0x3C4D, ieeeAddress: ieeeB, isDirectChild: false,
                linkKey: {key: crypto.randomBytes(16), rxCounter: 7, txCounter: 9}},
        ];
        const withDevices = {...after, devices};
        fs.writeFileSync(backupPath, JSON.stringify(BackupUtils.toUnifiedBackup(withDevices), null, 2) + '\n');
        await step('clear adapter', () => adapter.adapterManager.clearAdapter());
        await adapter.stop(); adapter = undefined;
        adapter = makeAdapter();
        report.restore_start = await step('start (restore backup)', () => adapter.start());
        assert.equal(report.restore_start, 'restored');
        assert.deepEqual(await adapter.getNetworkParameters(), parameters);
        const restored = await step('backup after restore', () => adapter.backup([]));
        assert.deepEqual(restored.networkOptions.networkKey, backup.networkOptions.networkKey);
        assert(restored.networkKeyInfo.frameCounter >= after.networkKeyInfo.frameCounter, 'frame counter not decreasing');
        assert.equal(restored.devices.length, 2, 'both devices restored');
        for (const d of devices) {
            const got = restored.devices.find(x => x.ieeeAddress.equals(d.ieeeAddress));
            assert(got, 'device ' + d.ieeeAddress.toString('hex') + ' present');
            assert.equal(got.networkAddress, d.networkAddress);
            assert.equal(got.isDirectChild, d.isDirectChild);
            assert(got.linkKey && got.linkKey.key.equals(d.linkKey.key), 'link key of ' + d.ieeeAddress.toString('hex'));
        }
        report.devices_preserved = true;
        await adapter.stop(); adapter = undefined;
        adapter = makeAdapter();
        report.after_restore_start = await step('start (resume after restore)', () => adapter.start());
        assert.equal(report.after_restore_start, 'resumed');
        if (!serial) {
            /* What the herdsman Controller sends when the configured channel
             * changes (controller.changeChannel): the network must follow. */
            await step('channel change (Mgmt_NWK_Update_req 0xFE)', async () => {
                const before = await adapter.getNetworkParameters();
                const target = before.channel === 25 ? 20 : 25;
                const zdo = Zdo.Buffalo.buildRequest(adapter.hasZdoMessageOverhead, Zdo.ClusterId.NWK_UPDATE_REQUEST,
                    [target], 0xfe, undefined, (before.nwkUpdateID + 1) & 0xff, undefined);
                await adapter.sendZdo('0x0000000000000000', 0xFFFF, Zdo.ClusterId.NWK_UPDATE_REQUEST, zdo, true);
                await new Promise(res => setTimeout(res, 11500));
                const after = await adapter.getNetworkParameters();
                assert.equal(after.channel, target, 'coordinator on the new channel');
                assert.equal(after.panID, before.panID);
                report.channel_change = {from: before.channel, to: after.channel};
            });
            /* ZNP extUpdateNwkKey + extSwitchNwkKey as herdsman defines them:
             * the active key read back is the new one (Z2M's network_key must
             * then be updated by the user, see docs/firmware/znp-compat.md). */
            await step('network key rotation (extUpdateNwkKey / extSwitchNwkKey)', async () => {
                const {Subsystem} = r('zigbee-herdsman/dist/adapter/z-stack/unpi/constants.js');
                const before = await adapter.znp.request(Subsystem.SYS, 'osalNvRead', {id: 0x003a, offset: 0});
                const seq = (before.payload.value[0] + 1) & 0xff;
                const key = crypto.randomBytes(16);
                await adapter.znp.request(Subsystem.ZDO, 'extUpdateNwkKey', {dstaddr: 0xfffd, keyseqnum: seq, key});
                await adapter.znp.request(Subsystem.ZDO, 'extSwitchNwkKey', {dstaddr: 0xfffd, keyseqnum: seq});
                await new Promise(res => setTimeout(res, 2600));
                const after = await adapter.znp.request(Subsystem.SYS, 'osalNvRead', {id: 0x003a, offset: 0});
                assert.equal(after.payload.value[0], seq, 'new key sequence active');
                assert(Buffer.from(after.payload.value.subarray(1, 17)).equals(key), 'new key active');
                report.key_rotation = {from_seq: before.payload.value[0], to_seq: seq};
            });
        }
        report.passed = true;
        delete report.current;
    } catch (error) {
        report.error = String(error.stack ?? error);
        process.exitCode = 1;
    } finally {
        if (adapter) { try { await adapter.stop(); } catch { /* ignore */ } }
        if (child) child.kill();
        fs.writeFileSync(path.join(dir, 'report.json'), JSON.stringify(report, null, 2) + '\n');
        console.log(JSON.stringify({passed: report.passed, steps: report.steps.length, failed_at: report.current,
            error: report.error && report.error.split('\n')[0], dir: path.relative(ROOT, dir)}, null, 1));
        if (!argv.includes('--keep') && report.passed) fs.rmSync(image, {force: true});
        setTimeout(() => process.exit(process.exitCode ?? 0), 200);
    }
}
setTimeout(() => { report.timeout = true; console.log(JSON.stringify(report)); child && child.kill(); process.exit(2); }, 240000).unref();
run();
