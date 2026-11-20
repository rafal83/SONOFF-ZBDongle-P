/* SPDX-License-Identifier: MIT
 * Restore a unified (Z2M) network backup into the dongle with zigbee-herdsman
 * 11.2.1's own ZStackAdapter, as Zigbee2MQTT does, for a coordinator whose
 * network state was cleared. Guards:
 *  - the network options are derived FROM the backup (herdsman finds them
 *    identical to its backup and restores it);
 *  - the startup strategy is checked BEFORE herdsman acts: anything but
 *    "restoreBackup" (e.g. "startCommissioning" = a NEW network) aborts;
 *  - the coordinator IEEE must be the expected one;
 *  - afterwards: network parameters and key equal the backup, NWK frame
 *    counter >= the backup's.
 *
 *   node tools/znp_restore_backup.cjs --port COM8 --baudrate 115200 \
 *        --backup private/<dir>/fresh.json --expect-ieee 0x... --out private/<dir>/restore
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const lab = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
assert.equal(lab('zigbee-herdsman/package.json').version, '11.2.1');

const args = {};
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].slice(2)] = process.argv[i + 1];
for (const k of ['port', 'baudrate', 'backup', 'expect-ieee', 'out']) if (!args[k]) throw Error(`--${k} is required`);
const out = path.resolve(args.out);
if (!out.startsWith(fs.realpathSync(path.join(ROOT, 'private'))) || fs.existsSync(out)) throw Error('--out must be new, under private/');
fs.mkdirSync(out);
const log = m => fs.appendFileSync(path.join(out, 'herdsman.private.log'), `${new Date().toISOString()} ${typeof m === 'function' ? m() : m}\n`);
lab('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
const {ZStackAdapter} = lab('zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js');
const {ZnpAdapterManager} = lab('zigbee-herdsman/dist/adapter/z-stack/adapter/manager.js');
const {BackupUtils} = lab('zigbee-herdsman/dist/utils/index.js');

async function main() {
    const unified = JSON.parse(fs.readFileSync(args.backup, 'utf8'));
    if ('0x' + unified.coordinator_ieee.toLowerCase() !== args['expect-ieee'].toLowerCase()) throw Error('Backup of another coordinator');
    /* herdsman reads the backup from backupPath: a private copy, never the Z2M one */
    const backupPath = path.join(out, 'coordinator_backup.json');
    fs.writeFileSync(backupPath, JSON.stringify(unified, null, 2) + '\n');
    const b = BackupUtils.fromUnifiedBackup(unified);
    const network = {panID: b.networkOptions.panId, extendedPanID: [...b.networkOptions.extendedPanId],
                     channelList: b.networkOptions.channelList, networkKey: [...b.networkOptions.networkKey],
                     networkKeyDistribute: false};
    const strategyOf = ZnpAdapterManager.prototype.determineStrategy;
    const report = {backup: path.relative(ROOT, args.backup), backup_frame_counter: unified.network_key.frame_counter};
    ZnpAdapterManager.prototype.determineStrategy = async function () {
        const s = await strategyOf.call(this);
        report.strategy = s;
        if (s !== 'restoreBackup') throw Error(`Refusing strategy "${s}": only a restore of the given backup is allowed`);
        return s;
    };
    const adapter = new ZStackAdapter(network, {path: args.port, adapter: 'zstack', baudRate: Number(args.baudrate), rtscts: false},
                                      backupPath, {disableLED: false, concurrent: 2});
    try {
        report.result = await adapter.start();
        assert.equal(report.result, 'restored');
        const ieee = await adapter.getCoordinatorIEEE();
        assert.equal(ieee.toLowerCase(), args['expect-ieee'].toLowerCase());
        const p = await adapter.getNetworkParameters();
        report.parameters = {panID: p.panID, channel: p.channel, extendedPanID: p.extendedPanID};
        assert.equal(p.panID, b.networkOptions.panId);
        assert.equal(p.channel, b.networkOptions.channelList[0]);
        const after = await adapter.backup([]);
        assert.deepEqual(after.networkOptions.networkKey, b.networkOptions.networkKey);
        report.frame_counter_after = after.networkKeyInfo.frameCounter;
        assert.ok(after.networkKeyInfo.frameCounter >= unified.network_key.frame_counter, 'frame counter went back');
        report.version = await adapter.getCoordinatorVersion();
        report.passed = true;
    } finally {
        await adapter.stop().catch(() => {});
        fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(report, null, 2) + '\n');
    }
    console.log(JSON.stringify(report));
}
main().catch(e => { console.error(String(e.stack ?? e)); process.exitCode = 1; });
