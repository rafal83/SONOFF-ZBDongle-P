/* SPDX-License-Identifier: MIT
 * Real ZStackAdapter on a blank experimental NV. No Z2M/MQTT process, no
 * production backup, no permit-join opening, and no simulated MT replies.
 * Invoke through run_dongle_session.py so rollback is guaranteed by Python.
 */
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const labRequire = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
const EXPECTED_INTEGRITY = 'sha512-fEBk4IwOR38BLJqx5XpGhW8Pyyk5Jak298/BlHRMRk9TUCm/swB6emyRwPYEGqcBMVSpOYvEY94qE+zAJDk2wg==';
function sha(file) { return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'); }
function checkRuntime() {
    const lock = JSON.parse(fs.readFileSync(path.join(__dirname, 'herdsman-lab-lock.json')));
    const installed = JSON.parse(fs.readFileSync(path.join(ROOT, '.tooling/herdsman-lab/package-lock.json')));
    assert.deepEqual(installed.packages, lock.packages);
    assert.equal(lock.packages['node_modules/zigbee-herdsman'].integrity, EXPECTED_INTEGRITY);
    assert.equal(labRequire('zigbee-herdsman/package.json').version, '11.2.1');
    labRequire('@serialport/bindings-cpp').autoDetect(); // Load native driver without opening a port.
    const {ZStackAdapter} = labRequire('zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js');
    assert.equal(typeof ZStackAdapter.prototype.start, 'function');
    const files = ['adapter/z-stack/adapter/zStackAdapter.js', 'adapter/z-stack/adapter/manager.js', 'adapter/z-stack/znp/znp.js'];
    return {version: '11.2.1', npm_integrity: EXPECTED_INTEGRITY,
        runtime_sha256: Object.fromEntries(files.map(f => [f, sha(labRequire.resolve('zigbee-herdsman/dist/' + f))])),
        harness_sha256: sha(__filename), dependency_lock_sha256: sha(path.join(__dirname, 'herdsman-lab-lock.json'))};
}
async function run() {
    const runtime = checkRuntime();
    if (process.argv.includes('--check-runtime')) { console.log(JSON.stringify({passed: true, serial_opened: false, runtime})); return; }
    const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, arg, i, all) => {
        if (arg.startsWith('--')) pairs.push([arg.slice(2), all[i + 1]]); return pairs;
    }, []));
    assert(args.port && args['expect-ieee'] && args['state-dir']);
    const dir = path.resolve(args['state-dir']);
    const privateRoot = fs.realpathSync(path.join(ROOT, 'private'));
    assert(dir.startsWith(privateRoot + path.sep), 'Lab state must remain private');
    fs.mkdirSync(dir, {recursive: true});
    assert(fs.realpathSync(dir).startsWith(privateRoot + path.sep));
    const reportPath = path.join(dir, 'result.json');
    const report = {passed: false, runtime, Z2M_used: false, MQTT_used: false,
        radio_peer_exchange_tested: false, associations_opened: false, production_NV_loaded: false};
    const save = () => fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n');
    const log = (message, ns) => fs.appendFileSync(path.join(dir, 'host-private.log'),
        `${new Date().toISOString()} ${ns}: ${typeof message === 'function' ? message() : message}\n`);
    labRequire('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
    const {ZStackAdapter} = labRequire('zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js');
    const Zdo = labRequire('zigbee-herdsman/dist/zspec/zdo/index.js');
    const Zcl = labRequire('zigbee-herdsman/dist/zspec/zcl/index.js');
    const {BackupUtils} = labRequire('zigbee-herdsman/dist/utils/index.js');
    const network = {panID: crypto.randomInt(1, 65535), extendedPanID: [...crypto.randomBytes(8)],
        networkKey: [...crypto.randomBytes(16)], networkKeyDistribute: false, channelList: [25]};
    fs.writeFileSync(path.join(dir, 'network-private.json'), JSON.stringify(network), {flag: 'wx'});
    const backupPath = path.join(dir, 'coordinator-backup-private.json');
    const makeAdapter = () => new ZStackAdapter(network, {path: args.port, adapter: 'zstack', baudRate: 115200, rtscts: false},
        backupPath, {disableLED: false, transmitPower: 0, concurrent: 2});
    let adapter;
    const deadline = setTimeout(() => {
        report.timeout = true; save(); process.exit(2);
    }, 170000);
    const step = async (name, fn) => { report.current_step = name; save(); const value = await fn(); report[name] = true; save(); return value; };
    const query = async (id, ...values) => {
        const response = await adapter.sendZdo(args['expect-ieee'], 0, id, Zdo.Buffalo.buildRequest(false, id, ...values), false);
        assert(response && response[0] === 0, 'Local ZDO response must succeed');
        return response[1];
    };
    try {
        adapter = makeAdapter();
        report.first_start_result = await step('adapter_start', () => adapter.start());
        assert.equal(report.first_start_result, 'reset'); // Fresh experimental NV, not a production resume.
        await step('permit_join_closed', () => adapter.permitJoin(0, 0));
        const version = await adapter.getCoordinatorVersion();
        assert.equal(Number(version.meta.revision), 20261114);
        assert.equal(await adapter.getCoordinatorIEEE(), args['expect-ieee']);
        const parameters = await step('network_parameters', () => adapter.getNetworkParameters());
        assert.equal(parameters.panID, network.panID); assert.equal(parameters.channel, 25);
        assert.equal(parameters.extendedPanID, '0x' + Buffer.from(network.extendedPanID).toString('hex'));
        const node = await step('local_node_descriptor', () => query(Zdo.ClusterId.NODE_DESCRIPTOR_REQUEST, 0));
        assert.equal(node.logicalType, 0);
        const endpoints = await step('local_active_endpoints', () => query(Zdo.ClusterId.ACTIVE_ENDPOINTS_REQUEST, 0));
        assert(endpoints.endpointList.includes(1) && endpoints.endpointList.includes(242));
        report.endpoint_count = endpoints.endpointList.length;
        for (const endpoint of [1, 242]) {
            const descriptor = await step('local_simple_descriptor_' + endpoint, () => query(Zdo.ClusterId.SIMPLE_DESCRIPTOR_REQUEST, 0, endpoint));
            assert.equal(descriptor.endpoint, endpoint);
        }
        const backup = await step('network_backup', () => adapter.backup([]));
        assert.equal(backup.networkOptions.panId, network.panID);
        assert.deepEqual(backup.networkOptions.networkKey, Buffer.from(network.networkKey));
        assert.equal(backup.devices.length, 0);
        fs.writeFileSync(backupPath, JSON.stringify(BackupUtils.toUnifiedBackup(backup), null, 2) + '\n');
        await step('soft_reset', () => adapter.reset('soft'));
        await adapter.stop(); adapter = undefined;
        await new Promise(resolve => setTimeout(resolve, 1000));
        adapter = makeAdapter();
        report.second_start_result = await step('adapter_restart', () => adapter.start());
        assert.equal(report.second_start_result, 'resumed');
        await adapter.permitJoin(0, 0);
        const resumed = await adapter.getNetworkParameters();
        assert.deepEqual(resumed, parameters);
        const after = await adapter.backup([]);
        assert.deepEqual(after.networkOptions.networkKey, backup.networkOptions.networkKey);
        assert(after.networkKeyInfo.frameCounter >= backup.networkKeyInfo.frameCounter);
        report.network_key_and_parameters_persisted = true;
        report.frame_counter_non_decreasing_after_reset = true;
        await step('local_AF_data_confirm_and_incoming_ZCL', async () => {
            const frame = Zcl.Frame.create(Zcl.FrameType.GLOBAL, Zcl.Direction.CLIENT_TO_SERVER,
                true, undefined, 173, 'read', 0x0000, [{attrId: 0x0004}], {});
            let received = false;
            const listener = payload => {
                if (payload.address === 0 && payload.clusterID === 0 && payload.endpoint === 1 &&
                    payload.destinationEndpoint === 1 && payload.data.equals(frame.toBuffer())) received = true;
            };
            adapter.on('zclPayload', listener);
            try {
                await adapter.sendZclFrameToEndpoint(args['expect-ieee'], 0, 1, frame, 5000, true, true, 1);
                for (let i = 0; i < 100 && !received; i++) await new Promise(resolve => setTimeout(resolve, 20));
                assert(received, 'Local APS delivery must produce the matching incoming ZCL payload');
            } finally { adapter.removeListener('zclPayload', listener); }
        });
        // Exercise the REAL manager restore strategy, rather than merely
        // decoding a backup on the host. Only this fresh lab NV is cleared.
        // These internals are pinned by the runtime hashes above.
        await step('clear_experimental_network', () => adapter.adapterManager.clearAdapter());
        await adapter.stop(); adapter = undefined;
        await new Promise(resolve => setTimeout(resolve, 1000));
        adapter = makeAdapter();
        report.backup_restore_start_result = await step('adapter_backup_restore', () => adapter.start());
        assert.equal(report.backup_restore_start_result, 'restored');
        await adapter.permitJoin(0, 0);
        const restored = await adapter.getNetworkParameters();
        assert.deepEqual(restored, parameters);
        const restoredBackup = await adapter.backup([]);
        assert.deepEqual(restoredBackup.networkOptions.networkKey, backup.networkOptions.networkKey);
        assert(restoredBackup.networkKeyInfo.frameCounter >= after.networkKeyInfo.frameCounter);
        report.backup_restored_key_and_parameters = true;
        report.frame_counter_non_decreasing_after_backup_restore = true;
        report.passed = true;
    } catch (error) {
        fs.writeFileSync(path.join(dir, 'failure-private.txt'), String(error.stack ?? error));
        report.error_name = error.name;
        process.exitCode = 1;
    } finally {
        clearTimeout(deadline);
        if (adapter) {
            try { await adapter.stop(); } catch { report.close_error = true; process.exitCode = 1; }
        }
        save(); console.log(JSON.stringify(report));
    }
}
run().catch(error => { console.error('Lab preflight failed: ' + error.name); process.exitCode = 1; });
