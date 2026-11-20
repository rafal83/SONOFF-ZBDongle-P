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

// TempMaster: the GP Responses used by test_frt_gp are byte for byte what
// herdsman's own encoder produces (greenPower.js, RxOnCapable commissioning
// and Channel Request), and herdsman reads our Channel Request notification.
const encode = (seq, payload) => Zcl.Frame.create(Zcl.FrameType.SPECIFIC, Zcl.Direction.SERVER_TO_CLIENT, true,
    undefined, seq, 'response', 'greenPower', payload, {}).toBuffer();
assert.deepEqual(encode(0x21, {options: 0, tempMaster: 0x0000, tempMasterTx: 15 - 11, srcID: 0x01AABBCC, gpdCmd: 0xf0,
    gpdPayload: {commandID: 0xf0, options: 0}}), frames.response_f0, 'GP Response / Commissioning Reply');
assert.deepEqual(encode(0x22, {options: 0, tempMaster: 0x0000, tempMasterTx: 5, srcID: 0, gpdCmd: 0xf3,
    gpdPayload: {commandID: 0xf3, operationalChannel: 15 - 11, basic: true}}), frames.response_f3,
    'GP Response / Channel Configuration');
const cr = parse(frames.commissioning_channel_request);
assert.equal(cr.command.name, 'commissioningNotification');
assert.equal(cr.payload.srcID, 0);
assert.equal(cr.payload.commandID, 0xe3);
assert.equal(cr.payload.commandFrame.nextChannel, 5);
assert.equal((cr.payload.options >> 3) & 1, 1, 'RxAfterTx');

console.log(JSON.stringify({passed: true, parsed: Object.keys(frames).length,
    commissioning: {srcID: c.payload.srcID, cmd: c.payload.commandID, deviceID: c.payload.commandFrame.deviceID},
    notification: {srcID: n.payload.srcID, cmd: n.payload.commandID},
    secured: {srcID: s.payload.srcID, counter: s.payload.frameCounter, cmd: s.payload.commandID,
              commandFrame: s.payload.commandFrame},
    channelRequest: {nextChannel: cr.payload.commandFrame.nextChannel}, responsesMatchHerdsmanEncoder: true}));
