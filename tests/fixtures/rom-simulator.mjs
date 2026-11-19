// SPDX-License-Identifier: MIT
// Byte-level TI ROM simulator; never accesses a physical USB device.
import { APP_END, CCFG, FLASH_SIZE, PAGE_SIZE } from '../../web-flasher/rom.mjs';

export function fixtureCcfg() {
  const bytes = new Uint8Array(FLASH_SIZE - CCFG).fill(255);
  const view = new DataView(bytes.buffer);
  view.setUint32(0x57fd8 - CCFG, 0xc5fe0fc5, true);
  view.setUint32(0x57fe4 - CCFG, 0xffc5ffff, true);
  view.setUint32(0x57fec - CCFG, 0, true);
  return bytes;
}
export function fixtureFirmware() {
  const bytes = new Uint8Array(APP_END).fill(255);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, 0x20010000, true);
  view.setUint32(4, 0x101, true);
  for (let i = 0x100; i < PAGE_SIZE + 400; i++) bytes[i] = (i * 17) & 255;
  return bytes;
}
function crc(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let i = 0; i < 8; i++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  }
  return (value ^ 0xffffffff) >>> 0;
}
const little = n => Uint8Array.of(n, n >>> 8, n >>> 16, n >>> 24);
const big = n => Uint8Array.of(n >>> 24, n >>> 16, n >>> 8, n);
const readBig = (data, offset = 0) => new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(offset, false);

export class RomSimulator {
  constructor(options = {}) {
    this.options = options;
    this.memory = Uint8Array.from({ length: FLASH_SIZE }, (_, i) => (i * 19 + 7) & 255);
    this.memory.set(options.ccfg ?? fixtureCcfg(), CCFG);
    this.original = this.memory.slice();
    this.commands = [];
    this.signals = [];
    this.resets = 0;
    this.sent = 0;
    this.closed = false;
  }
  getInfo() { return { usbVendorId: 0x10c4, usbProductId: 0xea60 }; }
  async open(options) {
    this.openOptions = options;
    this.closed = false;
    this.readable = new ReadableStream({ start: controller => { this.controller = controller; } });
    this.writable = new WritableStream({ write: data => this.write(data) });
  }
  async close() { this.closed = true; try { this.controller.close(); } catch {} }
  async setSignals(value) { this.signals.push(value); }
  emit(data) {
    // Fragment every response, including ACK and header boundaries.
    for (let i = 0; i < data.length; i += 3) this.controller.enqueue(Uint8Array.from(data.slice(i, i + 3)));
  }
  response(bytes) {
    const sum = bytes.reduce((a, b) => a + b, 0) & 255;
    this.emit([bytes.length + 2, this.options.badChecksum ? sum ^ 1 : sum, ...bytes]);
  }
  write(packet) {
    if (packet.length === 2 && packet[0] === 0) return; // host ACK/NACK
    if (packet.length === 2 && packet[0] === 0x55 && packet[1] === 0x55) {
      if (this.options.noisy) this.emit([0xaa, 0xcc, 0xfe, 0, 0x21, 0x90]);
      this.emit([0, 0xcc]);
      return;
    }
    const [size, sum, cmd] = packet;
    if (size !== packet.length || sum !== (packet.slice(2).reduce((a, b) => a + b, 0) & 255)) throw new Error('Malformed host command');
    const args = packet.slice(3);
    this.commands.push({ cmd, args: Array.from(args) });
    if (this.options.silent === cmd) return;
    if (this.options.nack === cmd) { this.emit([0, 0x33]); return; }
    if (cmd === 0x24 && ++this.sent === this.options.disconnectAtSend) {
      this.controller.error(new Error('USB débranché'));
      return;
    }
    this.emit([0, 0xcc]);
    if (cmd === 0x23) this.response([this.options.status ?? 0x40]);
    else if (cmd === 0x2a) {
      const address = readBig(args), count = args[5];
      if (args[4] !== 1 || !count || count > 63) throw new Error('Invalid ROM read');
      const registers = new Map([
        [0x50001318, this.options.deviceId ?? 0x0bb41000],
        [0x50001294, this.options.userId ?? 0x02005000],
        [0x4003002c, this.options.sectors ?? 44],
        [0x500012f0, 0x11223344], [0x500012f4, 0x00124b00],
      ]);
      if (registers.has(address)) this.response(little(registers.get(address)));
      else {
        if (address + count * 4 > FLASH_SIZE) throw new Error('Read exceeds Flash');
        const bytes = this.memory.slice(address, address + count * 4);
        if (this.options.corruptReadback && this.sent && address === 0) bytes[0] ^= 1;
        this.response(bytes);
      }
    } else if (cmd === 0x27) {
      if (args.length !== 12 || readBig(args, 8) !== 0) throw new Error('Invalid CRC command');
      const value = crc(this.memory.subarray(readBig(args), readBig(args) + readBig(args, 4)));
      this.response(big(this.options.badCrc ? value ^ 1 : value));
    } else if (cmd === 0x26) {
      if (args.length !== 4) throw new Error('Sector erase requires address only');
      const address = readBig(args);
      if (address >= APP_END || address % PAGE_SIZE) throw new Error('Protected sector erase');
      this.memory.fill(255, address, address + PAGE_SIZE);
    } else if (cmd === 0x21) {
      this.pointer = readBig(args);
      this.remaining = readBig(args, 4);
      if (this.pointer % 4 || this.remaining % 4 || this.pointer + this.remaining > APP_END) throw new Error('Protected download');
    } else if (cmd === 0x24) {
      if (args.length > 252 || args.length % 4 || args.length > this.remaining) throw new Error('Invalid data chunk');
      for (const byte of args) this.memory[this.pointer++] &= byte;
      this.remaining -= args.length;
    } else if (cmd === 0x25) this.resets++;
    else throw new Error(`Unsupported command ${cmd}`);
  }
}
