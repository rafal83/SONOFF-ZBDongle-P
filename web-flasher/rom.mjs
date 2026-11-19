// SPDX-License-Identifier: MIT
// TI SWRA466 / SWCU185 serial ROM protocol. No bank erase or CCFG writes.
export const FLASH_SIZE = 0x58000;
export const APP_END = 0x48000;
export const PAGE_SIZE = 0x2000;
export const CCFG = 0x57fa8;
const ACK = Uint8Array.of(0, 0xcc);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const be32 = n => Uint8Array.of(n >>> 24, n >>> 16, n >>> 8, n);
export const le32 = (b, offset = 0) => new DataView(b.buffer, b.byteOffset, b.byteLength).getUint32(offset, true);
export async function sha256(bytes) {
  return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), b => b.toString(16).padStart(2, '0')).join('');
}
export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const b of bytes) {
    crc ^= b;
    for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}
export function validateApplication(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length !== APP_END) throw new Error('Taille du firmware incompatible avec le CC2652P.');
  const stack = le32(bytes), reset = le32(bytes, 4);
  if (stack <= 0x20000000 || stack > 0x20014000 || stack % 8 || !(reset & 1) || reset >= APP_END || bytes[reset & ~1] === 0xff) {
    throw new Error('Vecteurs du firmware invalides.');
  }
}
function appRange(address, length) {
  if (!Number.isInteger(address) || !Number.isInteger(length) || address < 0 || length <= 0 || address + length > APP_END) {
    throw new Error('Écriture refusée hors de la zone application.');
  }
}

export class TiRom {
  constructor(port, { timeout = 5000, delay = sleep } = {}) {
    this.port = port;
    this.timeout = timeout;
    this.delay = delay;
    this.queue = [];
    this.opened = false;
    this.device = null;
  }
  async open() {
    await this.port.open({ baudRate: 500000, flowControl: 'none', bufferSize: 65536 });
    this.opened = true;
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
    this.reading = this.pump();
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: true });
    await this.port.setSignals({ dataTerminalReady: true, requestToSend: false });
    await this.delay(10);
    await this.port.setSignals({ requestToSend: true });
    await this.delay(150);
    let lastError;
    for (let attempt = 0; attempt < 3; attempt++) {
      this.queue.length = 0;
      await this.writer.write(Uint8Array.of(0x55, 0x55));
      try { await this.ack(1200); return; } catch (error) { lastError = error; }
    }
    throw new Error(`Bootloader inaccessible. Arrêtez Zigbee2MQTT et reconnectez le dongle. ${lastError.message}`);
  }
  async pump() {
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.queue.push(...value);
        this.wake?.();
      }
      this.readError ??= new Error('Le port série a été fermé.');
    } catch (error) { this.readError = error; }
    finally { this.wake?.(); }
  }
  async read(length, timeout = this.timeout) {
    const deadline = Date.now() + timeout;
    while (this.queue.length < length) {
      if (this.readError) throw this.readError;
      const remaining = deadline - Date.now();
      if (remaining <= 0) throw new Error('Délai de réponse du dongle dépassé.');
      await new Promise(resolve => {
        const timer = setTimeout(() => { this.wake = null; resolve(); }, remaining);
        this.wake = () => { clearTimeout(timer); this.wake = null; resolve(); };
      });
    }
    return Uint8Array.from(this.queue.splice(0, length));
  }
  async ack(timeout = this.timeout) {
    // Search for the exact ROM ACK pair, including after startup UART noise.
    const deadline = Date.now() + timeout;
    let zero = false;
    for (let i = 0; i < 512; i++) {
      const b = (await this.read(1, Math.max(1, deadline - Date.now())))[0];
      if (zero && b === 0xcc) return;
      if (zero && b === 0x33) throw new Error('Commande refusée par le bootloader.');
      zero = b === 0;
    }
    throw new Error('Acquittement du bootloader invalide.');
  }
  async response() {
    const [size, checksum] = await this.read(2);
    if (size < 3) throw new Error('Longueur de réponse ROM invalide.');
    const data = await this.read(size - 2);
    if ((data.reduce((a, b) => a + b, 0) & 255) !== checksum) {
      await this.writer.write(Uint8Array.of(0, 0x33));
      throw new Error('Somme de contrôle de la réponse ROM invalide.');
    }
    await this.writer.write(ACK);
    return data;
  }
  async command(cmd, payload = [], { response = false, status = true, timeout = this.timeout } = {}) {
    const body = Uint8Array.of(cmd, ...payload);
    if (body.length > 253) throw new Error('Paquet ROM trop long.');
    await this.writer.write(Uint8Array.of(body.length + 2, body.reduce((a, b) => a + b, 0) & 255, ...body));
    await this.ack(timeout);
    const data = response ? await this.response() : undefined;
    if (status) {
      const result = await this.command(0x23, [], { response: true, status: false });
      if (result.length !== 1 || result[0] !== 0x40) throw new Error(`Échec ROM (statut ${result[0]?.toString(16)}).`);
    }
    return data;
  }
  async word(address) {
    const allowed = [0x50001318, 0x50001294, 0x4003002c, 0x500012f0, 0x500012f4];
    if (!allowed.includes(address)) throw new Error('Registre non autorisé.');
    const data = await this.command(0x2a, [...be32(address), 1, 1], { response: true });
    if (data.length !== 4) throw new Error('Lecture du registre incomplète.');
    return le32(data);
  }
  async identify() {
    const deviceId = await this.word(0x50001318);
    const userId = await this.word(0x50001294);
    const flash = await this.word(0x4003002c);
    if (((deviceId >>> 12) & 0xffff) !== 0xbb41 || (userId & 0x00800000) || !(userId & 0x02000000) ||
        !(userId & 0x00004000) || (flash & 255) * PAGE_SIZE !== FLASH_SIZE) {
      throw new Error('Ce composant n’est pas un CC2652P de 352 Kio. Aucun flash effectué.');
    }
    const low = await this.word(0x500012f0), high = await this.word(0x500012f4);
    const ieee = high.toString(16).padStart(8, '0') + low.toString(16).padStart(8, '0');
    this.device = { model: 'CC2652P', ieee, flashSize: FLASH_SIZE };
    return this.device;
  }
  async readFlash(address, length, progress = () => {}) {
    if (!Number.isInteger(address) || !Number.isInteger(length) || address < 0 || length <= 0 || address + length > FLASH_SIZE || address % 4 || length % 4) {
      throw new Error('Lecture hors de la mémoire Flash.');
    }
    const result = new Uint8Array(length);
    for (let offset = 0; offset < length; offset += 252) {
      const count = Math.min(252, length - offset);
      const data = await this.command(0x2a, [...be32(address + offset), 1, count / 4], { response: true });
      if (data.length !== count) throw new Error('Lecture Flash incomplète.');
      result.set(data, offset);
      progress((offset + count) / length);
    }
    return result;
  }
  async crc(address, length) {
    if (address < 0 || length <= 0 || address + length > FLASH_SIZE) throw new Error('CRC hors de la mémoire Flash.');
    const data = await this.command(0x27, [...be32(address), ...be32(length), 0, 0, 0, 0], { response: true });
    if (data.length !== 4) throw new Error('Réponse CRC incomplète.');
    return new DataView(data.buffer, data.byteOffset, 4).getUint32(0, false);
  }
  async backup(progress) {
    if (!this.device) throw new Error('Identification du dongle requise.');
    const data = await this.readFlash(0, FLASH_SIZE, progress);
    if (crc32(data) !== await this.crc(0, FLASH_SIZE)) throw new Error('La sauvegarde ne correspond pas au CRC du dongle.');
    return data;
  }
  async eraseSector(address) {
    if (!this.device) throw new Error('Identification du dongle requise.');
    appRange(address, PAGE_SIZE);
    if (address % PAGE_SIZE) throw new Error('Secteur non aligné.');
    await this.command(0x26, be32(address), { timeout: 15000 });
  }
  async programPage(address, bytes) {
    if (!this.device) throw new Error('Identification du dongle requise.');
    appRange(address, bytes.length);
    if (address % PAGE_SIZE || bytes.length !== PAGE_SIZE) throw new Error('Page non alignée.');
    await this.command(0x21, [...be32(address), ...be32(bytes.length)]);
    for (let offset = 0; offset < bytes.length; offset += 252) await this.command(0x24, bytes.subarray(offset, offset + 252));
  }
  async reset() {
    await this.command(0x25, [], { status: false });
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
    await this.delay(100);
  }
  async close() {
    if (!this.opened) return;
    try { await this.reader.cancel(); await this.reading; }
    finally {
      this.reader.releaseLock();
      this.writer.releaseLock();
      await this.port.close();
      this.opened = false;
    }
  }
}

export async function installApplication(rom, binary, expectedSha, original, expectedCcfg, progress = () => {}) {
  const image = Uint8Array.from(binary), backup = Uint8Array.from(original);
  validateApplication(image);
  if (await sha256(image) !== expectedSha) throw new Error('Le SHA-256 du firmware est invalide.');
  if (!rom.device || backup.length !== FLASH_SIZE) throw new Error('Sauvegarde complète requise avant installation.');
  if (expectedCcfg.length !== FLASH_SIZE - CCFG || expectedCcfg.some((b, i) => b !== backup[CCFG + i])) {
    throw new Error('La configuration CCFG du dongle diffère du firmware. Installation refusée.');
  }
  // Ensure the device still matches the saved image, including its network data.
  if (await rom.crc(0, FLASH_SIZE) !== crc32(backup)) throw new Error('Le dongle a changé depuis sa sauvegarde. Reconnectez-le.');
  progress('erase', 0);
  for (let address = 0; address < APP_END; address += PAGE_SIZE) {
    await rom.eraseSector(address);
    progress('erase', (address + PAGE_SIZE) / APP_END);
  }
  // Program the vector page last; interrupted writes keep the application invalid.
  for (let address = APP_END - PAGE_SIZE; address >= 0; address -= PAGE_SIZE) {
    const page = image.subarray(address, address + PAGE_SIZE);
    if (page.some(b => b !== 255)) await rom.programPage(address, page);
    progress('program', (APP_END - address) / APP_END);
  }
  const expected = backup.slice();
  expected.set(image);
  const readback = await rom.readFlash(0, FLASH_SIZE, p => progress('verify', p));
  if (readback.some((b, i) => b !== expected[i])) throw new Error('La relecture Flash diffère du résultat attendu. Ne redémarrez pas : relancez l’installation.');
  if (await rom.crc(0, FLASH_SIZE) !== crc32(expected)) throw new Error('La vérification CRC finale a échoué.');
  progress('verified', 1);
}
