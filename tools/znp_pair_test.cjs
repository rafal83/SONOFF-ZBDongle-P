/* SPDX-License-Identifier: MIT
 * Real pairing through the full zigbee-herdsman 11.2.1 Controller (the
 * Zigbee2MQTT stack minus MQTT) on the FreeRTOS coordinator (dongle).
 * Persistent LABORATORY network and database under private/znp-lab/, so a
 * second run resumes like Z2M does. Prints markers for the operator:
 *   STARTED <result>, PERMIT_OPEN, JOINED, ANNOUNCE, INTERVIEW_OK/FAILED,
 *   MESSAGE, TOGGLE_OK/TOGGLE_ERROR, DONE
 *
 *   node tools/znp_pair_test.cjs --serial COM8 [--baud 115200] [--permit 180] [--toggles 4] [--no-permit]
 *                                [--power-test]   (5 then 20 dBm: LQI of the coordinator in the device's table)
 */
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const r = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
const argv = process.argv.slice(2);
const opt = (name, dflt) => argv.includes(name) ? argv[argv.indexOf(name) + 1] : dflt;
const serial = opt('--serial', 'COM8');
const permitSeconds = Number(opt('--permit', '180'));
const toggles = Number(opt('--toggles', '4'));
const baud = Number(opt('--baud', '115200'));
const dir = path.join(ROOT, 'private/znp-lab');
fs.mkdirSync(dir, {recursive: true});
const out = (...a) => { const line = a.join(' '); console.log(line); fs.appendFileSync(path.join(dir, 'pair-test.log'), `${new Date().toISOString()} ${line}\n`); };
const log = (m, ns) => fs.appendFileSync(path.join(dir, 'herdsman.log'), `${new Date().toISOString()} ${ns}: ${typeof m === 'function' ? m() : m}\n`);
r('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
const {Controller} = r('zigbee-herdsman');

const netFile = path.join(dir, 'network.json');
if (!fs.existsSync(netFile)) {
    fs.writeFileSync(netFile, JSON.stringify({panID: crypto.randomInt(0x1000, 0xFFF0),
        extendedPanID: [...crypto.randomBytes(8)], networkKey: [...crypto.randomBytes(16)], channelList: [25]}));
}
const network = JSON.parse(fs.readFileSync(netFile));

const sleep = ms => new Promise(res => setTimeout(res, ms));
async function main() {
    const controller = new Controller({
        network: {...network, networkKeyDistribute: false},
        transport: {path: serial, baudRate: baud, rtscts: false, adapter: 'zstack'},
        databasePath: path.join(dir, 'database.db'),
        databaseBackupPath: path.join(dir, 'database.db.backup'),
        backupPath: path.join(dir, 'coordinator_backup.json'),
        adapter: {disableLED: false, concurrent: 4},
        acceptJoiningDeviceHandler: async () => true,
    });
    let interviewed = null;
    controller.on('deviceJoined', e => out('JOINED', e.device.ieeeAddr, '0x' + e.device.networkAddress.toString(16)));
    controller.on('deviceAnnounce', e => out('ANNOUNCE', e.device.ieeeAddr, '0x' + e.device.networkAddress.toString(16)));
    controller.on('deviceLeave', e => out('LEAVE', e.ieeeAddr));
    controller.on('message', m => out('MESSAGE', m.device.ieeeAddr, m.type, m.cluster, JSON.stringify(m.data).slice(0, 120)));
    controller.on('deviceInterview', e => {
        if (e.status === 'started') { out('INTERVIEW_STARTED', e.device.ieeeAddr); return; }
        if (e.status === 'failed') { out('INTERVIEW_FAILED', e.device.ieeeAddr); return; }
        const d = e.device;
        out('INTERVIEW_OK', d.ieeeAddr, JSON.stringify({model: d.modelID, manufacturer: d.manufacturerName, type: d.type,
            power: d.powerSource, endpoints: d.endpoints.map(ep => ({id: ep.ID, in: ep.inputClusters, out: ep.outputClusters}))}));
        interviewed = d;
    });
    const result = await controller.start();
    out('STARTED', result, 'pan=0x' + network.panID.toString(16), 'channel=25');
    try {
        const known = controller.getDevicesIterator ? [...controller.getDevicesIterator()].filter(d => d.type !== 'Coordinator') : [];
        out('KNOWN_DEVICES', known.length, known.map(d => d.ieeeAddr + '/' + (d.modelID ?? '?')).join(' '));
        if (!argv.includes('--no-permit')) {
            await controller.permitJoin(permitSeconds);
            out('PERMIT_OPEN', permitSeconds);
            const until = Date.now() + permitSeconds * 1000;
            while (!interviewed && Date.now() < until) await sleep(500);
            await controller.permitJoin(0);
        } else if (known.length) {
            interviewed = known[known.length - 1];
        }
        if (!interviewed) { out('NO_DEVICE'); return; }
        const ep = interviewed.endpoints.find(e => e.inputClusters.includes(6));
        if (!ep) { out('NO_ONOFF_CLUSTER'); return; }
        for (let i = 0; i < toggles; i++) {
            try {
                await ep.command('genOnOff', 'toggle', {}, {disableDefaultResponse: false});
                out('TOGGLE_OK', i + 1);
            } catch (error) {
                out('TOGGLE_ERROR', i + 1, String(error.message).slice(0, 160));
            }
            await sleep(3000);
        }
        try {
            const v = await ep.read('genOnOff', ['onOff']);
            out('READ_ONOFF', JSON.stringify(v));
        } catch (error) { out('READ_ERROR', String(error.message).slice(0, 160)); }
        if (argv.includes('--power-test')) {
            /* PA check: the device's own neighbour table tells how well it
             * hears the coordinator, at the standard PA then at the 20 dBm PA. */
            for (const dbm of [5, 20, 5, 20, 5]) {
                const tuned = await controller.adapter.znp.request(1, 'stackTune', {operation: 0, value: dbm});
                const applied = tuned?.payload?.value;
                let ok = 0;
                for (let i = 0; i < 6; i++) {
                    try { await ep.read('genOnOff', ['onOff']); ok++; } catch { /* counted */ }
                    await sleep(1500);
                }
                let lqi = null;
                try {
                    const table = await interviewed.lqi();
                    const me = table.find(e => e.nwkAddress === 0);
                    lqi = me ? me.lqi : 'absent';
                } catch (error) { lqi = 'error ' + String(error.message).slice(0, 80); }
                out('POWER', dbm, 'applied', applied, 'reads_ok', ok + '/6', 'lqi_at_device', lqi, 'lqi_at_coordinator', interviewed.linkquality);
            }
        }
    } finally {
        await controller.stop();
        out('DONE');
    }
}
main().catch(e => { out('FATAL', String(e.stack ?? e).split('\n').slice(0, 3).join(' | ')); process.exitCode = 1; });
