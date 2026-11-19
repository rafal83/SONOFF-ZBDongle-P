// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { APP_END, CCFG, FLASH_SIZE, PAGE_SIZE, TiRom, crc32, sha256, installApplication, validateApplication } from '../web-flasher/rom.mjs';
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
