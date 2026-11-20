// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { APP_END, CCFG, FLASH_SIZE, PAGE_SIZE, TiRom, crc32, sha256, installApplication, validateApplication, applicationInfo, encodeBackup, decodeBackup, restoreApplication } from '../web-flasher/rom.mjs';
import { RomSimulator, fixtureCcfg, fixtureFirmware } from './fixtures/rom-simulator.mjs';

async function connect(options = {}) {
  const port = new RomSimulator(options);
  const rom = new TiRom(port, { timeout: options.silent ? 500 : 5000, delay: async () => {} });
  await rom.open();
  return { rom, port };
}
async function prepared(options = {}) {
  const result = await connect(options);
  await result.rom.identify();
  result.backup = await result.rom.backup();
  result.binary = fixtureFirmware();
  result.hash = await sha256(result.binary);
  return result;
}
const mutations = port => port.commands.filter(c => [0x21, 0x24, 0x26, 0x2c].includes(c.cmd));

const ieee = '00124b0011223344';
async function savedImage(original) {
  const bytes = original.slice();
  bytes.set(fixtureFirmware());
  // Old network state must NEVER overwrite the current data and counter floor.
  bytes[APP_END] ^= 1;
  const json = await encodeBackup(bytes, ieee);
  return { bytes, json, saved: await decodeBackup(new TextEncoder().encode(json), 'backup.json') };
}

test('portable JSON backup verifies its complete flash digest, identity and firmware metadata', async () => {
  const bytes = new Uint8Array(FLASH_SIZE).fill(255); bytes.set(fixtureFirmware());
  const { saved, json } = await savedImage(bytes);
  assert.equal(saved.ieee, ieee);
  assert.equal(saved.baud, 1000000);
  assert.equal(saved.revision, 20261119);
  assert.deepEqual(applicationInfo(saved.bytes.subarray(0, APP_END)), { baud: 1000000, revision: 20261119 });
  const data = JSON.parse(json); data.sha256 = '0'.repeat(64);
  await assert.rejects(decodeBackup(new TextEncoder().encode(JSON.stringify(data)), 'backup.json'), /SHA-256/);
  await assert.rejects(decodeBackup(new Uint8Array(1024 * 1024 + 1), 'backup.json'), /volumineux/);
  await assert.rejects(decodeBackup(new TextEncoder().encode('null'), 'backup.json'), /Format/);
  await assert.rejects(decodeBackup(new TextEncoder().encode('{'), 'backup.json'), /JSON/);
  await assert.rejects(decodeBackup(new TextEncoder().encode('{"metadata":{"format":"zigpy/open-coordinator-backup"}}'), 'coordinator_backup.json'), /Format/);
});

test('legacy full BIN needs its original IEEE filename; unsupported firmware and storage layouts are refused', async () => {
  const bytes = new Uint8Array(FLASH_SIZE).fill(255); bytes.set(fixtureFirmware());
  const name = `zbdongle-p-${ieee}-2026-11-19.bin`;
  assert.equal((await decodeBackup(bytes, name)).legacy, true);
  await assert.rejects(decodeBackup(bytes, 'renamed.bin'), /nom/);
  await assert.rejects(decodeBackup(bytes.subarray(0, APP_END), name), /352/);
  for (const offset of [0x100, 0x108, 0x10c, 0x110]) {
    const incompatible = bytes.slice(); incompatible[offset] ^= 1;
    await assert.rejects(decodeBackup(incompatible, name), /stockage compatible/);
  }
});

test('restore writes saved firmware, keeps current network bytes and CCFG, and programs vectors last', async () => {
  const { rom, port, backup } = await prepared();
  try {
    const { saved, bytes } = await savedImage(backup);
    await restoreApplication(rom, saved, backup);
    assert.deepEqual(port.memory.subarray(0, APP_END), bytes.subarray(0, APP_END));
    assert.deepEqual(port.memory.subarray(APP_END), backup.subarray(APP_END));
    assert.notEqual(port.memory[APP_END], saved.bytes[APP_END]);
    assert.deepEqual(port.commands.filter(c => c.cmd === 0x21).at(-1).args.slice(0, 4), [0, 0, 0, 0]);
    assert.equal(port.commands.some(c => c.cmd === 0x2c), false);
    assert.equal(port.resets, 0);
  } finally { await rom.close(); }
});

for (const failure of ['identity', 'digest', 'ccfg', 'changed', 'storage']) {
  test(`restore refuses ${failure} before writing`, async () => {
    const { rom, port, backup } = await prepared();
    try {
      const { saved } = await savedImage(backup);
      if (failure === 'identity') saved.ieee = '0000000000000000';
      if (failure === 'digest') saved.bytes[0x200] ^= 1;
      if (failure === 'ccfg') { saved.bytes[CCFG] ^= 1; saved.hash = await sha256(saved.bytes); }
      if (failure === 'changed') port.memory[APP_END] ^= 1;
      if (failure === 'storage') { saved.bytes[0x10c] = 2; saved.hash = await sha256(saved.bytes); }
      await assert.rejects(restoreApplication(rom, saved, backup));
      assert.equal(mutations(port).length, 0);
    } finally { await rom.close(); }
  });
}

for (const options of [{ corruptReadback: true }, { disconnectAtSend: 3 }]) {
  test(`restore failure leaves the device without a reset: ${JSON.stringify(options)}`, async () => {
    const { rom, port, backup } = await prepared(options);
    try {
      const { saved } = await savedImage(backup);
      await assert.rejects(restoreApplication(rom, saved, backup));
      assert.equal(port.resets, 0);
    } finally { await rom.close(); }
  });
}

test('standard CRC32 vector, SHA256, and invalid application vectors', async () => {
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
  assert.equal(await sha256(new Uint8Array()), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.throws(() => validateApplication(new Uint8Array(4)));
  const image = fixtureFirmware(); image[0] = 1;
  assert.throws(() => validateApplication(image), /Vecteurs/);
});
test('fragmented packets and startup noise; strict device identity; complete CRC-verified backup', async () => {
  const { rom, port } = await connect({ noisy: true });
  try {
    assert.equal((await rom.identify()).ieee, '00124b0011223344');
    assert.equal(port.openOptions.baudRate, 500000);
    assert.deepEqual(port.signals.slice(1, 4), [{ dataTerminalReady: false, requestToSend: true }, { dataTerminalReady: true, requestToSend: false }, { requestToSend: true }]);
    assert.deepEqual(await rom.backup(), port.original);
    assert.equal(mutations(port).length, 0);
  } finally { await rom.close(); }
});
for (const options of [{ deviceId: 0x0bb77000 }, { userId: 0x02805000 }, { userId: 0x00005000 }, { userId: 0x02001000 }, { sectors: 88 }]) {
  test(`unsupported target is rejected before erasure: ${JSON.stringify(options)}`, async () => {
    const { rom, port } = await connect(options);
    try { await assert.rejects(rom.identify(), /pas un CC2652P/); assert.equal(mutations(port).length, 0); }
    finally { await rom.close(); }
  });
}
for (const options of [{ nack: 0x2a }, { badChecksum: true }, { status: 0x44 }, { silent: 0x2a }]) {
  test(`ROM errors abort: ${JSON.stringify(options)}`, async () => {
    const { rom, port } = await connect(options);
    try { await assert.rejects(rom.identify()); assert.equal(mutations(port).length, 0); }
    finally { await rom.close(); }
  });
}
test('a corrupt backup CRC prevents any write', async () => {
  const { rom, port } = await connect({ badCrc: true });
  try { await rom.identify(); await assert.rejects(rom.backup(), /CRC/); assert.equal(mutations(port).length, 0); }
  finally { await rom.close(); }
});
test('protected storage, unaligned erases and crossing writes are rejected', async () => {
  const { rom, port } = await prepared();
  try {
    for (const address of [APP_END, CCFG, -PAGE_SIZE, 1]) await assert.rejects(rom.eraseSector(address));
    await assert.rejects(rom.programPage(APP_END - 4, new Uint8Array(PAGE_SIZE)));
    await assert.rejects(rom.programPage(0, new Uint8Array(3)));
    assert.equal(mutations(port).length, 0);
  } finally { await rom.close(); }
});
test('bad firmware hash, incompatible CCFG or changed dongle prevents erasure', async () => {
  const { rom, port, binary, backup, hash } = await prepared();
  try {
    await assert.rejects(installApplication(rom, binary, '0'.repeat(64), backup, fixtureCcfg()), /SHA-256/);
    const ccfg = fixtureCcfg(); ccfg[0] ^= 1;
    await assert.rejects(installApplication(rom, binary, hash, backup, ccfg), /CCFG/);
    port.memory[APP_END] ^= 1;
    await assert.rejects(installApplication(rom, binary, hash, backup, fixtureCcfg()), /changé/);
    assert.equal(mutations(port).length, 0);
  } finally { await rom.close(); }
});
test('full flash: vectors last; all 64 KiB of protected data preserved; reset only after verification', async () => {
  const { rom, port, binary, backup, hash } = await prepared();
  try {
    await installApplication(rom, binary, hash, backup, fixtureCcfg());
    assert.deepEqual(port.memory.subarray(0, APP_END), binary);
    assert.deepEqual(port.memory.subarray(APP_END), backup.subarray(APP_END));
    const erases = port.commands.filter(c => c.cmd === 0x26);
    assert.equal(erases.length, 36);
    assert.deepEqual(erases[0].args, [0, 0, 0, 0]);
    const downloads = port.commands.filter(c => c.cmd === 0x21);
    assert.deepEqual(downloads.at(-1).args.slice(0, 4), [0, 0, 0, 0]);
    assert.equal(port.resets, 0);
    await rom.reset();
    assert.equal(port.resets, 1);
    assert.equal(port.commands.some(c => c.cmd === 0x2c), false);
  } finally { await rom.close(); }
});
for (const options of [{ corruptReadback: true }, { disconnectAtSend: 3 }, { silent: 0x24 }]) {
  test(`interrupted or corrupt programming never resets into incomplete firmware: ${JSON.stringify(options)}`, async () => {
    const { rom, port, binary, backup, hash } = await prepared(options);
    try {
      await assert.rejects(installApplication(rom, binary, hash, backup, fixtureCcfg()));
      assert.equal(port.resets, 0);
      assert.deepEqual(port.memory.subarray(APP_END), backup.subarray(APP_END));
      if (options.silent) assert.equal(port.commands.filter(c => c.cmd === 0x24).length, 1);
    } finally { try { await rom.close(); } catch {} }
  });
}
