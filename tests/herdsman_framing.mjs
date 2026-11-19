// SPDX-License-Identifier: MIT
// Exercise the pinned, actual herdsman Frame implementation against the C fixture.
// This tests transport interoperability; it never starts ZStackAdapter or a radio.
import assert from 'node:assert/strict';
import {stripTypeScriptTypes} from 'node:module';
import {readFileSync, writeFileSync, mkdirSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
import {pathToFileURL, fileURLToPath} from 'node:url';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = path.join(root, 'upstream/zigbee-herdsman/src/adapter/z-stack/unpi');
const dest = path.join(root, 'build/herdsman-framing');
mkdirSync(dest, {recursive: true});
for (const name of ['constants', 'frame']) {
    const original = readFileSync(path.join(source, name + '.ts'), 'utf8');
    const transformed = stripTypeScriptTypes(original, {mode: 'transform'})
        .replace('from "./constants"', 'from "./constants.mjs"');
    writeFileSync(path.join(dest, name + '.mjs'), transformed);
}
const {Frame} = await import(pathToFileURL(path.join(dest, 'frame.mjs')));
const {Type, Subsystem} = await import(pathToFileURL(path.join(dest, 'constants.mjs')));
const requests = [1, 2, 4, 255].map(id => new Frame(Type.SREQ, Subsystem.SYS, id, Buffer.alloc(0)).toBuffer());
const executable = path.join(root, 'build', process.platform === 'win32' ? 'npi_stdio.exe' : 'npi_stdio');
const result = spawnSync(executable, [], {input: Buffer.concat(requests)});
assert.equal(result.error, undefined);
assert.equal(result.status, 0, result.stderr?.toString());
const replies = [];
for (let offset = 0; offset < result.stdout.length;) {
    const length = result.stdout[offset + 1];
    const bytes = result.stdout.subarray(offset, offset + length + 5);
    assert.equal(bytes.length, length + 5);
    assert.equal(bytes[0], 0xfe);
    replies.push(Frame.fromBuffer(length, length + 4, bytes));
    offset += bytes.length;
}
assert.equal(replies.length, 4);
assert.equal(replies[0].type, Type.SRSP);
assert.equal(replies[0].subsystem, Subsystem.SYS);
assert.deepEqual(replies[0].data, Buffer.from([1, 0]));
assert.equal(replies[1].data.length, 9);
assert.equal(replies[1].data[1], 0xfe); // no false coordinator product ID
assert.deepEqual(replies[2].data, Buffer.from([1, 0, 0, 0, 0, 0, 0, 2]));
assert.equal(replies[3].subsystem, Subsystem.RESERVED);
assert.deepEqual(replies[3].data, Buffer.from([2, 0x21, 255]));
// Recover the first real ping following delimiter noise or a missing checksum.
const ping = requests[0];
for (const input of [Buffer.concat([Buffer.alloc(300, 0xfe), ping]),
    Buffer.concat([ping.subarray(0, 4), ping])]) {
    const response = spawnSync(executable, [], {input});
    assert.equal(response.status, 0);
    const decoded = Frame.fromBuffer(2, 6, response.stdout);
    assert.equal(response.stdout.length, 7);
    assert.deepEqual(decoded.data, Buffer.from([1, 0]));
}
console.log('PASS: pinned zigbee-herdsman Frame <-> C diagnostic: 4 commands + delimiter/missing-FCS recovery; no adapter start or RF');
