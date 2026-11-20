/* SPDX-License-Identifier: MIT
 * Touchlink acceptance test: the REAL zigbee-herdsman 11.2.1 ZStackAdapter
 * drives the inter-PAN sequence of its Touchlink controller against the
 * FreeRTOS coordinator's portable stack in the host simulator
 * (build/freertos/sim/znp_sim), over tcp://127.0.0.1:<port>:
 *   setChannelInterPAN(11)  -> AF interPanCtl (channel, endpoint 12)
 *   scan request broadcast  -> AF dataRequestExt to PAN 0xFFFF, the bulb's
 *                              Scan Response injected as heard on the air
 *                              (<image>.inject), back as AF_INCOMING_MSG_EXT
 *   identify, reset to factory new to the bulb's IEEE (inter-PAN unicast)
 *   restoreChannelInterPAN  -> AF interPanCtl 0, radio back on channel 25
 * No radio exists in the simulator: the channel moves are read from its log.
 *
 *   node tools/znp_sim_touchlink.cjs
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

const dir = path.join(ROOT, 'build/freertos/sim', 'touchlink-' + Date.now());
fs.mkdirSync(dir, {recursive: true});
const sim = path.join(ROOT, 'build/freertos/sim', process.platform === 'win32' ? 'znp_sim.exe' : 'znp_sim');
const image = path.join(dir, 'flash.bin');
const log = (message, ns) => fs.appendFileSync(path.join(dir, 'herdsman.log'),
    `${new Date().toISOString()} ${ns}: ${typeof message === 'function' ? message() : message}\n`);
r('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
const {ZStackAdapter} = r('zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js');
const Zcl = r('zigbee-herdsman/dist/zspec/zcl/index.js');

let simLog = '';
function startSim() {
    const child = spawn(sim, ['0', image, '1', '--format-store'], {stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', d => { simLog += String(d); fs.appendFileSync(path.join(dir, 'sim.log'), d); });
    return new Promise((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('simulator did not start')), 5000);
        child.stderr.on('data', () => {
            const m = /listening on 127\.0\.0\.1:(\d+)/.exec(simLog);
            if (m) { clearTimeout(t); resolve({child, port: Number(m[1])}); }
        });
    });
}
const waitLog = async (re) => { for (let i = 0; i < 40 && !re.test(simLog); i++) await new Promise(r => setTimeout(r, 50)); return re.test(simLog); };
const le = (value, bytes) => { const b = Buffer.alloc(bytes); b.writeUIntLE(Number(value & ((1n << BigInt(bytes * 8)) - 1n)), 0, bytes); return b; };
const ext = (v) => Buffer.from(v.toString(16).padStart(16, '0'), 'hex').reverse();

async function run() {
    const {child, port} = await startSim();
    const network = {panID: crypto.randomInt(1, 65000), extendedPanID: [...crypto.randomBytes(8)],
        networkKey: [...crypto.randomBytes(16)], networkKeyDistribute: false, channelList: [25]};
    const adapter = new ZStackAdapter(network, {path: `tcp://127.0.0.1:${port}`, adapter: 'zstack'},
        path.join(dir, 'coordinator_backup.json'), {disableLED: false, transmitPower: 5, concurrent: 2});
    const report = {passed: false, steps: []};
    try {
        assert.equal(await adapter.start(), 'reset');
        const coord = BigInt(await adapter.getCoordinatorIEEE());
        await adapter.setChannelInterPAN(11);
        assert(await waitLog(/gp_channel 11/), 'radio moved to channel 11');
        report.steps.push('interPanCtl channel 11 + endpoint 12');

        const bulb = 0x0017880100abcdefn, tsn = 0x2a;
        const scanResponse = Buffer.concat([
            Buffer.from([0x19, tsn, 0x01]),                       /* ZCL: specific, server->client, scanResponse */
            le(0x12345678n, 4), Buffer.from([0x00, 0x04, 0x12]),  /* transaction, RSSI correction, ZigBee/touchlink info */
            le(0x0003n, 2), le(0xCAFEn, 4), ext(0x1122334455667788n), Buffer.from([0x00, 11]),
            le(0x1234n, 2), le(0x5678n, 2), Buffer.from([1, 0]),  /* PAN, address, 1 sub-device, 0 groups */
            Buffer.from([11]), le(0xC05En, 2), le(0x0100n, 2), Buffer.from([2, 0]),
        ]);
        const nwk = Buffer.concat([Buffer.from([0x0B, 0x00, 0x03]), le(0x1000n, 2), le(0xC05En, 2), scanResponse]);
        const mac = Buffer.concat([Buffer.from([0x21, 0xCC, 0x55]), le(BigInt(network.panID), 2), ext(coord),
            le(0x1234n, 2), ext(bulb), nwk]);
        const scan = Zcl.Frame.create(Zcl.FrameType.SPECIFIC, Zcl.Direction.CLIENT_TO_SERVER, true, undefined, tsn,
            'scanRequest', 'touchlink', {transactionID: 0x12345678, zigbeeInformation: 4, touchlinkInformation: 18}, {});
        const pending = adapter.sendZclFrameInterPANBroadcast(scan, 3000, false);
        setTimeout(() => fs.writeFileSync(image + '.inject', mac.toString('hex') + '\n'), 200);
        const response = await pending;
        assert.equal(response.address, '0x' + bulb.toString(16).padStart(16, '0'));
        assert.equal(response.endpoint, 0xfe);
        assert.equal(response.clusterID, 0x1000);
        report.steps.push('scan request broadcast, scan response from ' + response.address);

        const identify = Zcl.Frame.create(Zcl.FrameType.SPECIFIC, Zcl.Direction.CLIENT_TO_SERVER, true, undefined, 0,
            'identifyRequest', 'touchlink', {transactionID: 0x12345678, duration: 65535}, {});
        await adapter.sendZclFrameInterPANToIeeeAddr(identify, response.address);
        const reset = Zcl.Frame.create(Zcl.FrameType.SPECIFIC, Zcl.Direction.CLIENT_TO_SERVER, true, undefined, 0,
            'resetToFactoryNew', 'touchlink', {transactionID: 0x12345678}, {});
        await adapter.sendZclFrameInterPANToIeeeAddr(reset, response.address);
        report.steps.push('identify + reset to factory new to the bulb');

        await adapter.restoreChannelInterPAN();
        assert(await waitLog(/gp_channel 25/), 'radio back on the network channel');
        report.steps.push('interPanCtl restore: channel 25');
        report.passed = true;
    } finally {
        fs.writeFileSync(path.join(dir, 'report.json'), JSON.stringify(report, null, 1) + '\n');
        console.log(JSON.stringify(report, null, 1));
        try { await adapter.stop(); } catch { /* simulator killed below */ }
        child.kill();
    }
}
run().catch(error => { console.error(error); process.exit(1); });
