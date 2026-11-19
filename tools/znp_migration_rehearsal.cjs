/* SPDX-License-Identifier: MIT
 * Migration REHEARSAL (no hardware touched): the real zigbee-herdsman 11.2.1
 * ZStackAdapter restores the PRODUCTION Zigbee2MQTT coordinator backup into
 * the FreeRTOS coordinator running in the host simulator, exactly as Z2M
 * would on first start with the new firmware ("restoreBackup" strategy).
 * Then it checks, entry by entry, that the backup produced by the new
 * firmware carries the same network: coordinator IEEE, PAN ID, extended
 * PAN ID, channel, network key and sequence, TC link key seed, every
 * device (network address, IEEE, direct-child flag, link key) and frame
 * counters that never went backwards; finally a restart must "resume".
 *
 * Secrets never leave private/: the report only holds counts and booleans.
 *
 *   node tools/znp_migration_rehearsal.cjs --backup private/<...>/coordinator_backup.json
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const r = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
assert.equal(r('zigbee-herdsman/package.json').version, '11.2.1');
const argv = process.argv.slice(2);
const opt = (n, d) => argv.includes(n) ? argv[argv.indexOf(n) + 1] : d;
const source = path.resolve(opt('--backup', ''));
const privateRoot = fs.realpathSync(path.join(ROOT, 'private'));
assert(fs.realpathSync(source).startsWith(privateRoot + path.sep), 'the production backup must stay under private/');
const dir = path.join(privateRoot, 'znp-rehearsal', 'run-' + Date.now());
fs.mkdirSync(dir, {recursive: true});
const backupPath = path.join(dir, 'coordinator_backup.json');
fs.copyFileSync(source, backupPath);   /* herdsman rewrites its backup file: never the original */
const sim = path.join(ROOT, 'build/freertos/sim', process.platform === 'win32' ? 'znp_sim.exe' : 'znp_sim');
const log = (m, ns) => fs.appendFileSync(path.join(dir, 'herdsman.private.log'), `${new Date().toISOString()} ${ns}: ${typeof m === 'function' ? m() : m}\n`);
r('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
const {ZStackAdapter} = r('zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js');
const {BackupUtils} = r('zigbee-herdsman/dist/utils/index.js');

const report = {passed: false, hardware_touched: false, source_sha256: require('node:crypto').createHash('sha256').update(fs.readFileSync(source)).digest('hex'), checks: {}};
let child, port = 0;
function startSim() {
    child = spawn(sim, ['0', path.join(dir, 'flash.private.bin'), '1'], {stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', d => fs.appendFileSync(path.join(dir, 'sim.log'), d));
    return new Promise((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('simulator did not start')), 5000);
        child.stderr.on('data', d => { const m = /listening on 127\.0\.0\.1:(\d+)/.exec(String(d)); if (m) { port = Number(m[1]); clearTimeout(t); resolve(); } });
    });
}
const check = (name, ok) => { report.checks[name] = Boolean(ok); if (!ok) throw new Error('check failed: ' + name); };

async function run() {
    const original = BackupUtils.fromUnifiedBackup(JSON.parse(fs.readFileSync(source, 'utf8')));
    report.devices_in_backup = original.devices.length;
    report.devices_with_link_key = original.devices.filter(d => d.linkKey).length;
    await startSim();
    const o = original.networkOptions;
    const network = {panID: o.panId, extendedPanID: [...o.extendedPanId], channelList: o.channelList,
        networkKey: [...o.networkKey], networkKeyDistribute: false};
    const make = () => new ZStackAdapter(network, {path: `tcp://127.0.0.1:${port}`, adapter: 'zstack'}, backupPath,
        {disableLED: false, concurrent: 4});
    let adapter = make();
    const ieees = original.devices.map(d => '0x' + d.ieeeAddress.toString('hex'));
    try {
        report.first_start = await adapter.start();
        check('strategy_restored', report.first_start === 'restored');
        await adapter.stop();
        /* The restore ends with a reset: the coordinator IEEE override applies from the next boot. */
        adapter = make();
        report.second_start = await adapter.start();
        check('restart_resumed', report.second_start === 'resumed');
        const after = await adapter.backup(ieees);
        check('coordinator_ieee', after.coordinatorIeeeAddress.equals(original.coordinatorIeeeAddress));
        check('pan_id', after.networkOptions.panId === o.panId);
        check('extended_pan_id', after.networkOptions.extendedPanId.equals(o.extendedPanId));
        check('channel', after.logicalChannel === original.logicalChannel);
        check('network_key', after.networkOptions.networkKey.equals(o.networkKey));
        check('network_key_sequence', after.networkKeyInfo.sequenceNumber === original.networkKeyInfo.sequenceNumber);
        check('nwk_frame_counter_not_decreasing', after.networkKeyInfo.frameCounter >= original.networkKeyInfo.frameCounter + 2500);
        // Fresh store (no counters floor): the firmware adds its restore margin (ZNP_RESTORE_COUNTER_MARGIN)
        // because the counters used since the backup was written are unknown.
        check('nwk_frame_counter_restore_margin', after.networkKeyInfo.frameCounter >= original.networkKeyInfo.frameCounter + 2500 + 4194304);
        check('tclk_seed', Boolean(after.znp?.trustCenterLinkKeySeed?.equals(original.znp.trustCenterLinkKeySeed)));
        check('nwk_update_id', after.networkUpdateId === original.networkUpdateId);
        const byIeee = new Map(after.devices.map(d => [d.ieeeAddress.toString('hex'), d]));
        let same = 0, keys = 0, counters = 0;
        const diff = {missing: 0, network_address: 0, direct_child: 0, rx_counter: 0, tx_counter: 0,
                      missing_kind: {}};
        for (const d of original.devices) {
            const got = byIeee.get(d.ieeeAddress.toString('hex'));
            if (!got) {
                diff.missing++;
                const kind = `nwk=${d.networkAddress === 0xFFFE || d.networkAddress === undefined ? 'none' : 'set'},key=${Boolean(d.linkKey)},child=${d.isDirectChild}`;
                diff.missing_kind[kind] = (diff.missing_kind[kind] || 0) + 1;
                continue;
            }
            if (got.networkAddress !== d.networkAddress) { diff.network_address++; continue; }
            if (got.isDirectChild !== d.isDirectChild) { diff.direct_child++; continue; }
            if (d.linkKey && got.linkKey) {
                if (got.linkKey.rxCounter < d.linkKey.rxCounter) diff.rx_counter++;
                if (got.linkKey.txCounter < d.linkKey.txCounter) diff.tx_counter++;
            }
            same++;
            if (d.linkKey) {
                if (got.linkKey && got.linkKey.key.equals(d.linkKey.key)) keys++;
                if (got.linkKey && got.linkKey.rxCounter >= d.linkKey.rxCounter && got.linkKey.txCounter >= d.linkKey.txCounter) counters++;
            }
        }
        report.devices_identical = same;
        report.differences = diff;
        report.link_keys_identical = keys;
        report.link_key_counters_not_decreasing = counters;
        /* herdsman's restoreBackup writes devices without link key that are not
         * direct children with address-manager user 0, which its own table
         * treats as a free entry: the next device overwrites it before
         * anything reaches the adapter (Z-Stack -> Z-Stack loses them too).
         * They are counted apart; the firmware relearns them from traffic. */
        const notCarried = original.devices.filter(d => !d.linkKey && !d.isDirectChild).length;
        report.not_carried_by_herdsman_restore = notCarried;
        /* Whether such a device is lost depends on its position in herdsman's
         * list (lost only when another device follows it): at most notCarried,
         * and only of that kind. Every device that is carried must be identical. */
        const onlyKeyless = Object.keys(diff.missing_kind).every(k => k === 'nwk=set,key=false,child=false');
        check('all_carried_devices_identical', diff.missing <= notCarried && onlyKeyless &&
              diff.network_address === 0 && diff.direct_child === 0 &&
              same === original.devices.length - diff.missing && after.devices.length === same);
        check('all_link_keys_identical', keys === report.devices_with_link_key);
        check('all_link_key_counters_not_decreasing', counters === report.devices_with_link_key);
        report.passed = true;
    } catch (error) {
        report.error = String(error.message).split('\n')[0];
        process.exitCode = 1;
    } finally {
        try { await adapter.stop(); } catch { /* ignore */ }
        child.kill();
        fs.writeFileSync(path.join(dir, 'report.json'), JSON.stringify(report, null, 2) + '\n');
        console.log(JSON.stringify({...report, dir: path.relative(ROOT, dir)}, null, 1));
        setTimeout(() => process.exit(process.exitCode ?? 0), 200);
    }
}
setTimeout(() => { console.log('timeout'); child && child.kill(); process.exit(2); }, 600000).unref();
run();
