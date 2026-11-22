/* SPDX-License-Identifier: MIT
 * Reads the coordinator diagnostics exactly as the Zigbee2MQTT extension
 * (tools/z2m/coordinator-diag.js) does: herdsman 11.2.1's ZNP layer, SYS
 * osalNvRead of the items 0x0FA0.., nothing else. Read-only: no network
 * start, no NV write. Zigbee2MQTT must not hold the port.
 *
 *   node tools/z2m/diag_check.cjs --port COM8 [--baudrate 1000000]
 */
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '../..');
const lab = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
const quiet = () => {};
lab('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: quiet, info: quiet,
    warning: (m) => console.error(typeof m === 'function' ? m() : m), error: (m) => console.error(typeof m === 'function' ? m() : m)});
const {AdapterTransport} = lab('zigbee-herdsman/dist/adapter/transport.js');
const {Znp} = lab('zigbee-herdsman/dist/adapter/z-stack/znp/index.js');
const {readDiagnostics} = require('./coordinator-diag.js');

const args = {};
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].slice(2)] = process.argv[i + 1];
if (!args.port) throw Error('--port is required');

(async () => {
    const znp = new Znp(new AdapterTransport({path: args.port, baudRate: Number(args.baudrate ?? 1000000), rtscts: false}));
    await znp.open();
    try {
        console.log(JSON.stringify(await readDiagnostics(znp), null, 1));
    } finally {
        await znp.close().catch(() => {});
    }
})().catch((e) => { console.error(e); process.exit(1); });
