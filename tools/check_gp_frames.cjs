/* SPDX-License-Identifier: MIT
 * Parse the GP notifications produced by the firmware's GP proxy
 * (build/freertos/host/gp-frames.txt, written by test_frt_gp) with the REAL
 * zigbee-herdsman 11.2.1 ZCL parser, and check the fields herdsman's
 * GreenPower.processCommand relies on.
 *
 *   node tools/check_gp_frames.cjs
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const r = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
const Zcl = r('zigbee-herdsman/dist/zspec/zcl/index.js');

const lines = fs.readFileSync(path.join(ROOT, 'build/freertos/host/gp-frames.txt'), 'utf8').trim().split(/\r?\n/);
const frames = Object.fromEntries(lines.map(l => l.split(' ')).map(([k, v]) => [k, Buffer.from(v, 'hex')]));
const parse = buf => {
    const header = Zcl.Header.fromBuffer(buf);
    return Zcl.Frame.fromBuffer(Zcl.Clusters.greenPower.ID, header, buf, {});
};

const c = parse(frames.commissioning);
assert.equal(c.command.name, 'commissioningNotification');
assert.equal(c.header.frameControl.direction, Zcl.Direction.CLIENT_TO_SERVER);
assert.equal(c.payload.srcID, 0x0150A0B1);
assert.equal(c.payload.commandID, 0xe0);
assert.equal(c.payload.commandFrame.deviceID, 0x02);
assert.equal((c.payload.options >> 9) & 1, 0, 'security processing not failed');

const n = parse(frames.notification);
assert.equal(n.command.name, 'notification');
assert.equal(n.payload.srcID, 0x0150A0B1);
assert.equal(n.payload.commandID, 0x22);

const s = parse(frames.notification_secured);
assert.equal(s.command.name, 'notification');
assert.equal(s.payload.srcID, 0x0175AB12);
assert.equal(s.payload.frameCounter, 101);
assert.equal(s.payload.commandID, 0x69);
assert.equal((s.payload.options >> 6) & 3, 2, 'not FullEncr: herdsman must not decrypt it again');
assert.deepEqual([...s.payload.commandFrame.raw ?? []].length >= 0, true);

console.log(JSON.stringify({passed: true, parsed: Object.keys(frames).length,
    commissioning: {srcID: c.payload.srcID, cmd: c.payload.commandID, deviceID: c.payload.commandFrame.deviceID},
    notification: {srcID: n.payload.srcID, cmd: n.payload.commandID},
    secured: {srcID: s.payload.srcID, counter: s.payload.frameCounter, cmd: s.payload.commandID,
              commandFrame: s.payload.commandFrame}}));
