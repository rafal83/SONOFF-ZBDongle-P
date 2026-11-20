/* SPDX-License-Identifier: MIT
 * Green Power TempMaster, end to end in simulation: the REAL zigbee-herdsman
 * 11.2.1 Controller (its GreenPower module decides the replies) against the
 * FreeRTOS coordinator's portable stack in build/freertos/sim/znp_sim.
 * GPD frames are injected as if heard on the air (<image>.inject); the
 * simulator logs what the firmware asks the radio to do (gp_tx: MPDU to send
 * in the GPD's receive window, gp_channel: TempMaster channel moves).
 *
 *  1. bidirectional GPD (RxOnCapability) commissioning: herdsman answers a
 *     GP Response (Commissioning Reply) naming the coordinator TempMaster;
 *     the GPD's next frame with RxAfterTx makes the firmware transmit the
 *     Commissioning Reply;
 *  2. Channel Request (maintenance frame): herdsman answers a Channel
 *     Configuration for the GPD's next channel; the radio moves there, the
 *     next Channel Request heard there gets the reply, then the radio comes
 *     back to the network channel.
 *
 * Simulation only: radio timing, CSMA-less transmission and the channel
 * switch of the real radio are NOT exercised here.
 *
 *   node tools/znp_sim_gp.cjs
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

const dir = path.join(ROOT, 'build/freertos/sim', 'gp-' + Date.now());
fs.mkdirSync(dir, {recursive: true});
const sim = path.join(ROOT, 'build/freertos/sim', process.platform === 'win32' ? 'znp_sim.exe' : 'znp_sim');
const image = path.join(dir, 'flash.bin');
const simLog = path.join(dir, 'sim.log');
const log = (message, ns) => fs.appendFileSync(path.join(dir, 'herdsman.log'),
    `${new Date().toISOString()} ${ns}: ${typeof message === 'function' ? message() : message}\n`);
r('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
const {Controller} = r('zigbee-herdsman/dist/controller/controller.js');

const CHANNEL = 15;
const SRC_ID = 0x01AABBCC;
const sleep = ms => new Promise(res => setTimeout(res, ms));
let macSeq = 0x40;

function inject(nwkHex) {
    const mpdu = Buffer.concat([Buffer.from([0x01, 0x08, macSeq++ & 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]),
                                Buffer.from(nwkHex.replace(/ /g, ''), 'hex')]);
    const tmp = image + '.inject.tmp';
    fs.writeFileSync(tmp, mpdu.toString('hex') + '\n');
    fs.renameSync(tmp, image + '.inject');
}
const simLines = () => fs.existsSync(simLog) ? fs.readFileSync(simLog, 'utf8').split(/\r?\n/) : [];
async function waitLine(re, after, timeout = 5000) {
    const t0 = Date.now();
    for (;;) {
        const hit = simLines().slice(after).find(l => re.test(l));
        if (hit) return hit;
        if (Date.now() - t0 > timeout) throw new Error(`timeout waiting for ${re}`);
        await sleep(20);
    }
}

async function main() {
    const child = spawn(sim, ['0', image, '1'], {stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', d => fs.appendFileSync(simLog, d));
    const port = await new Promise((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('simulator did not start')), 5000);
        child.stderr.on('data', d => {
            const m = /listening on 127\.0\.0\.1:(\d+)/.exec(String(d));
            if (m) { clearTimeout(t); resolve(Number(m[1])); }
        });
    });
    const report = {passed: false, dir: path.relative(ROOT, dir), steps: []};
    const step = (name, data) => { report.steps.push({name, ...data}); console.log('ok', name, JSON.stringify(data ?? {})); };
    const controller = new Controller({
        network: {panID: 0x1A62, extendedPanID: [...crypto.randomBytes(8)], channelList: [CHANNEL],
                  networkKey: [...crypto.randomBytes(16)], networkKeyDistribute: false},
        transport: {path: `tcp://127.0.0.1:${port}`, adapter: 'zstack'},
        databasePath: path.join(dir, 'database.db'),
        databaseBackupPath: path.join(dir, 'database.db.backup'),
        backupPath: path.join(dir, 'coordinator_backup.json'),
        adapter: {disableLED: false},
        acceptJoiningDeviceHandler: async () => true,
    });
    const joined = [];
    controller.on('deviceJoined', e => joined.push(e.device.ieeeAddr));
    try {
        step('start', {result: await controller.start()});
        await controller.permitJoin(120);   /* herdsman also opens GP proxy commissioning */
        await sleep(300);

        /* 1. Bidirectional GPD: commissioning without RxAfterTx (data frame,
         *    SrcID), device 0x02, options 0x02 = RxOnCapability. */
        const src = Buffer.alloc(4);
        src.writeUInt32LE(SRC_ID);
        let mark = simLines().length;
        inject('0c' + src.toString('hex') + 'e0 02 02');
        await sleep(1500);   /* herdsman: notification -> GP Response + GP Pairing */
        assert.ok(!simLines().slice(mark).some(l => /gp_tx/.test(l)), 'nothing sent while the GPD does not listen');
        mark = simLines().length;
        inject('8c 40' + src.toString('hex') + 'e0 02 02');   /* extended FC: RxAfterTx */
        const tx1 = (await waitLine(/gp_tx /, mark)).split('gp_tx ')[1].trim();
        assert.match(tx1, new RegExp('^0108[0-9a-f]{2}ffffffff8c80' + src.toString('hex') + 'f000$'), tx1);
        step('commissioning reply transmitted', {mpdu: tx1, gpd_joined_in_herdsman: joined});
        assert.ok(joined.some(a => a.endsWith(SRC_ID.toString(16).padStart(8, '0'))), 'GPD added by herdsman');

        /* 2. Channel Request: next attempt on channel 16 (5). */
        mark = simLines().length;
        inject('0d e3 45');
        const move = await waitLine(/gp_channel /, mark);
        assert.equal(Number(move.split('gp_channel ')[1]), 16, move);
        mark = simLines().length;
        inject('0d e3 45');   /* heard again, now on channel 16 */
        const tx2 = (await waitLine(/gp_tx /, mark)).split('gp_tx ')[1].trim();
        const opChannel = ((CHANNEL - 11) | 0x10).toString(16).padStart(2, '0');   /* basic */
        assert.match(tx2, new RegExp('^0108[0-9a-f]{2}ffffffff0df3' + opChannel + '$'), tx2);
        const back = await waitLine(/gp_channel /, mark, 2000);
        assert.equal(Number(back.split('gp_channel ')[1]), CHANNEL, back);
        step('channel configuration transmitted on channel 16, radio back', {mpdu: tx2, back: CHANNEL});
        report.passed = true;
    } finally {
        await controller.stop().catch(() => {});
        child.kill();
        fs.writeFileSync(path.join(dir, 'report.json'), JSON.stringify(report, null, 2));
    }
    console.log(JSON.stringify({passed: report.passed, steps: report.steps.length, dir: report.dir}));
}

main().catch(e => { console.error(e); process.exitCode = 1; });
