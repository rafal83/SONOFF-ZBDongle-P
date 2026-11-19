/* SPDX-License-Identifier: MIT
 * Real radio qualification with one explicitly reserved HG06106C lamp.
 * Private fresh network only. Run under the Python flash/restore supervisor.
 */
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const requireLab = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
const {ZStackAdapter} = requireLab('zigbee-herdsman/dist/adapter/z-stack/adapter/zStackAdapter.js');
const Zdo = requireLab('zigbee-herdsman/dist/zspec/zdo/index.js');
const Zcl = requireLab('zigbee-herdsman/dist/zspec/zcl/index.js');
const {BackupUtils} = requireLab('zigbee-herdsman/dist/utils/index.js');
const {Type, Subsystem} = requireLab('zigbee-herdsman/dist/adapter/z-stack/unpi/constants.js');
const Definition = requireLab('zigbee-herdsman/dist/adapter/z-stack/znp/definition.js').default;
const ParameterType = requireLab('zigbee-herdsman/dist/adapter/z-stack/znp/parameterType.js').default;
const diagnosticKeys = ['format', 'radio_rejected', 'last_radio_rejection', 'rx_allocation_failures',
    'tx_driver_retries', 'tx_dropped_normal', 'tx_dropped_control', 'uart_rx_errors'];
Definition[Subsystem.SYS].push({name: 'labReadStats', ID: 0xf0, type: Type.SREQ, request: [],
    response: diagnosticKeys.map(name => ({name, parameterType: ParameterType.UINT32}))});
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
async function run() {
    if (process.argv.includes('--check-runtime')) {
        // Module imports load the native driver; constructing/opening adapters
        // and generating any lab state are intentionally below this branch.
        assert.equal(typeof ZStackAdapter.prototype.sendZclFrameToEndpoint, 'function');
        const {Frame} = requireLab('zigbee-herdsman/dist/adapter/z-stack/unpi/index.js');
        const {ZpiObject} = requireLab('zigbee-herdsman/dist/adapter/z-stack/znp/zpiObject.js');
        const raw = Buffer.alloc(32);
        diagnosticKeys.forEach((_, i) => raw.writeUInt32LE(i + 1, i * 4));
        const decoded = ZpiObject.fromUnpiFrame(new Frame(Type.SRSP, Subsystem.SYS, 0xf0, raw));
        diagnosticKeys.forEach((key, i) => assert.equal(decoded.payload[key], i + 1));
        console.log(JSON.stringify({passed: true, diagnostic_codec_tested: true, serial_opened: false,
            harness_sha256: sha(__filename)})); return;
    }
    const args = {};
    for (let i = 2; i < process.argv.length; i += 2) {
        assert(process.argv[i].startsWith('--') && process.argv[i + 1]);
        args[process.argv[i].slice(2)] = process.argv[i + 1];
    }
    assert(args.port && args['expect-ieee'] && args['state-dir']);
    const expectedRevision = Number(args['expected-revision'] ?? 20261114);
    assert([20240710, 20250321, 20261114].includes(expectedRevision));
    const baudrate = Number(args.baudrate ?? 115200);
    assert([115200, 460800, 1000000].includes(baudrate));
    const privateRoot = fs.realpathSync(path.join(ROOT, 'private'));
    const dir = path.resolve(args['state-dir']);
    assert(dir.startsWith(privateRoot + path.sep));
    fs.mkdirSync(dir, {recursive: true});
    assert(fs.realpathSync(dir).startsWith(privateRoot + path.sep));
    const report = {passed: false, Z2M_used: false, MQTT_used: false, production_NV_loaded: false,
        hardware_peer_model: 'HG06106C', radio_peer_exchange_tested: false,
        associations_opened: false, association_window_seconds: 180,
        harness_sha256: sha(__filename), herdsman_version: requireLab('zigbee-herdsman/package.json').version};
    report.baudrate = baudrate;
    const save = () => fs.writeFileSync(path.join(dir, 'result.json'), JSON.stringify(report, null, 2) + '\n');
    const log = (message, ns) => fs.appendFileSync(path.join(dir, 'host-private.log'),
        `${new Date().toISOString()} ${ns}: ${typeof message === 'function' ? message() : message}\n`);
    requireLab('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
    let network = {panID: crypto.randomInt(1, 65535), extendedPanID: [...crypto.randomBytes(8)],
        networkKey: [...crypto.randomBytes(16)], networkKeyDistribute: false, channelList: [25]};
    if (args['resume-dir']) {
        args['resume-dir'] = fs.realpathSync(args['resume-dir']);
        assert(args['resume-dir'].startsWith(privateRoot + path.sep));
        network = JSON.parse(fs.readFileSync(path.join(args['resume-dir'], 'network-private.json')));
        assert(network.networkKey.length === 16 && network.extendedPanID.length === 8 && network.channelList[0] === 25);
    }
    fs.writeFileSync(path.join(dir, 'network-private.json'), JSON.stringify(network), {flag: 'wx'});
    const backupPath = path.join(dir, 'coordinator-backup-private.json');
    const makeAdapter = () => new ZStackAdapter(network,
        {path: args.port, adapter: 'zstack', baudRate: baudrate, rtscts: false}, backupPath,
        {disableLED: false, transmitPower: 0, concurrent: 2});
    let adapter, peer, sequence = 1, initialState, lightEndpoint;
    const deadline = setTimeout(() => { report.timeout = true; save(); process.exit(2); }, 570000);
    const snapshot = async label => {
        if (expectedRevision !== 20261114) return;
        if (!adapter?.transport?.isOpen) return;
        try {
            const {payload} = await adapter.znp.requestWithReply(Subsystem.SYS, 'labReadStats', {});
            assert.equal(payload.format, 1);
            report.diagnostics ??= {};
            report.diagnostics[label] = Object.fromEntries(diagnosticKeys.map(key => [key, payload[key]]));
        } catch { report.diagnostic_unavailable = true; }
        save();
    };
    const step = async (name, fn) => {
        report.current_step = name; save(); await snapshot(name + '_before');
        const value = await fn(); report[name] = true; await snapshot(name + '_after'); save(); return value;
    };
    const request = async (cluster, ...values) => {
        let error;
        for (let attempt = 0; attempt < 3; attempt++) {
            try {
                const response = await adapter.sendZdo(peer.ieeeAddr, peer.networkAddress, cluster,
                    Zdo.Buffalo.buildRequest(false, cluster, ...values), false);
                assert(response && response[0] === 0, 'Remote ZDO must return SUCCESS'); return response[1];
            } catch (e) {
                error = e; report.zdo_retry_count = (report.zdo_retry_count ?? 0) + 1; save(); await delay(500);
            }
        }
        throw error;
    };
    const read = async (endpoint, cluster, attributes) => {
        const start = performance.now();
        const frame = Zcl.Frame.create(Zcl.FrameType.GLOBAL, Zcl.Direction.CLIENT_TO_SERVER,
            true, undefined, sequence++ & 255, 'read', cluster, attributes.map(attrId => ({attrId})), {});
        const response = await adapter.sendZclFrameToEndpoint(peer.ieeeAddr, peer.networkAddress,
            endpoint, frame, 10000, false, true, 1);
        assert(response && response.address === peer.networkAddress && response.clusterID === cluster);
        const decoded = Zcl.Frame.fromBuffer(cluster, response.header, response.data, {});
        assert.equal(decoded.command.ID, Zcl.Foundation.readRsp.ID);
        report.application_read_latencies ??= [];
        report.application_read_latencies.push({phase: report.current_step, latency_ms: performance.now() - start});
        return decoded.payload;
    };
    const state = async () => {
        const attributes = await read(lightEndpoint, 6, [0]);
        const on = attributes.find(attr => attr.attrId === 0 && attr.status === 0);
        assert(on && (on.attrData === 0 || on.attrData === 1)); return on.attrData;
    };
    const setState = async value => {
        const frame = Zcl.Frame.create(Zcl.FrameType.SPECIFIC, Zcl.Direction.CLIENT_TO_SERVER,
            true, undefined, sequence++ & 255, value ? 'on' : 'off', 6, {}, {});
        await adapter.sendZclFrameToEndpoint(peer.ieeeAddr, peer.networkAddress, lightEndpoint, frame, 10000, true, true, 1);
        await delay(300); assert.equal(await state(), value);
    };
    const retryState = async () => {
        let error;
        for (let i = 0; i < 5; i++) {
            try { return await state(); } catch (e) { error = e; await delay(2000); }
        }
        throw error;
    };
    try {
        adapter = makeAdapter();
        assert.equal(await step('adapter_start', () => adapter.start()), args['resume-dir'] ? 'resumed' : 'reset');
        assert.equal(await adapter.getCoordinatorIEEE(), args['expect-ieee']);
        assert.equal(Number((await adapter.getCoordinatorVersion()).meta.revision), expectedRevision);
        report.firmware_revision = expectedRevision;
        await adapter.permitJoin(0, 0);
        if (!args['resume-dir'] || !fs.existsSync(path.join(args['resume-dir'], 'peer-private.json'))) {
        let acceptJoin;
        const joined = new Promise(resolve => { acceptJoin = resolve; });
        const listener = payload => {
            if (!peer && payload.ieeeAddr && payload.ieeeAddr !== args['expect-ieee']) {
                peer = {...payload}; acceptJoin();
            } else if (peer && payload.ieeeAddr === peer.ieeeAddr) peer.networkAddress = payload.networkAddress;
        };
        adapter.on('deviceJoined', listener);
        let announced = false;
        adapter.on('zdoResponse', (cluster, response) => {
            if (cluster === Zdo.ClusterId.END_DEVICE_ANNOUNCE && response[0] === 0 &&
                peer && response[1].eui64 === peer.ieeeAddr) {
                announced = true; peer.networkAddress = response[1].nwkAddress;
            }
        });
        report.current_step = 'opening_association_window'; save();
        await adapter.permitJoin(report.association_window_seconds, 0);
        report.associations_opened = true; report.current_step = 'waiting_for_pairing'; save();
        let joinTimer;
        try {
            await Promise.race([joined, new Promise((_, reject) => {
                joinTimer = setTimeout(() => reject(new Error('No test lamp joined during the bounded window')), 180000);
            })]);
            // tcDeviceInd precedes the device announcement/key exchange. Avoid
            // treating its provisional address as a finished interview state.
            for (let i = 0; i < 40 && !announced; i++) await delay(100);
        } finally { clearTimeout(joinTimer); await adapter.permitJoin(0, 0); }
        report.association_window_closed = true; report.peer_joined = true; save();
        } else {
            peer = JSON.parse(fs.readFileSync(path.join(args['resume-dir'], 'peer-private.json')));
            assert(/^0x[0-9a-f]{16}$/i.test(peer.ieeeAddr) && Number.isInteger(peer.networkAddress));
            report.peer_joined = true; report.peer_resumed_from_private_lab_NV = true;
            report.association_window_closed = true; save();
        }
        fs.writeFileSync(path.join(dir, 'peer-private.json'), JSON.stringify(peer));
        const node = await step('remote_node_descriptor', () => request(Zdo.ClusterId.NODE_DESCRIPTOR_REQUEST, peer.networkAddress));
        assert.equal(node.logicalType, 1, 'The mains-powered HG06106C must be a router');
        report.remote_manufacturer_code = node.manufacturerCode;
        // The upstream Device.interviewInternal() performs this early read for
        // Tuya manufacturer codes 0/4098, before requesting simple descriptors.
        if (node.manufacturerCode === 0 || node.manufacturerCode === 4098) {
            await step('tuya_early_basic_read', () => read(1, 0, [5, 4]));
        }
        const active = await step('remote_active_endpoints', () => request(Zdo.ClusterId.ACTIVE_ENDPOINTS_REQUEST, peer.networkAddress));
        report.remote_active_endpoint_list = active.endpointList; save();
        const descriptors = [];
        for (const endpoint of active.endpointList) {
            descriptors.push(await step('remote_simple_descriptor_' + endpoint,
                () => request(Zdo.ClusterId.SIMPLE_DESCRIPTOR_REQUEST, peer.networkAddress, endpoint)));
        }
        report.remote_simple_descriptors = true; report.remote_endpoint_count = descriptors.length;
        fs.writeFileSync(path.join(dir, 'descriptors-private.json'), JSON.stringify(descriptors));
        const basic = descriptors.find(d => d.inClusterList.includes(0));
        const light = descriptors.find(d => d.inClusterList.includes(6));
        assert(basic && light); lightEndpoint = light.endpoint;
        const identity = await step('remote_basic_attributes', () => read(basic.endpoint, 0, [4, 5]));
        assert(identity.some(attr => attr.attrId === 5 && attr.status === 0 && typeof attr.attrData === 'string'));
        fs.writeFileSync(path.join(dir, 'identity-private.json'), JSON.stringify(identity));
        initialState = await state();
        await step('remote_on_off_command_and_readback', async () => {
            await setState(initialState ? 0 : 1); await delay(1000); await setState(initialState);
        });
        await step('remote_repeated_attribute_reads', async () => {
            for (let i = 0; i < 30; i++) assert.equal(await state(), initialState);
        });
        report.repeated_read_count = 30; report.radio_peer_exchange_tested = true;
        const stressWrites = Number(args['stress-writes'] ?? 0);
        assert(Number.isInteger(stressWrites) && stressWrites >= 0 && stressWrites <= 200);
        if (stressWrites) await step('radio_and_NV_load_test', async () => {
            const template = Buffer.alloc(240, 0x5a);
            const writer = async () => {
                for (let i = 0; i < stressWrites; i++) {
                    template[0] = i;
                    await adapter.adapterManager.nv.writeExtendedTableEntry(13, 44, 1, template);
                }
            };
            const reader = async () => {
                for (let i = 0; i < 40; i++) assert.equal(await state(), initialState);
            };
            await Promise.all([writer(), reader()]);
            assert.deepEqual(await adapter.adapterManager.nv.readExtendedTableEntry(13, 44, 1), template);
            report.load_test_changed_NV_writes = stressWrites;
            report.load_test_radio_reads = 40;
        });
        const backup = await step('backup_with_radio_peer', () => adapter.backup([peer.ieeeAddr]));
        report.configured_security_level = backup.securityLevel;
        assert(backup.devices.some(d => d.ieeeAddress.toString('hex') === peer.ieeeAddr.slice(2)));
        fs.writeFileSync(backupPath, JSON.stringify(BackupUtils.toUnifiedBackup(backup), null, 2));
        await adapter.reset('soft'); await adapter.stop(); adapter = undefined; await delay(1000);
        adapter = makeAdapter();
        assert.equal(await step('adapter_resume_with_peer', () => adapter.start()), 'resumed');
        await adapter.permitJoin(0, 0);
        assert.equal(await step('radio_exchange_after_coordinator_reset', retryState), initialState);
        await step('clear_experimental_network', () => adapter.adapterManager.clearAdapter());
        await adapter.stop(); adapter = undefined; await delay(1000);
        adapter = makeAdapter();
        assert.equal(await step('restore_backup_with_radio_peer', () => adapter.start()), 'restored');
        await adapter.permitJoin(0, 0);
        assert.equal(await step('radio_exchange_after_backup_restore', retryState), initialState);
        const restored = await adapter.backup([peer.ieeeAddr]);
        assert.deepEqual(restored.networkOptions.networkKey, backup.networkOptions.networkKey);
        assert(restored.networkKeyInfo.frameCounter >= backup.networkKeyInfo.frameCounter);
        report.frame_counter_non_decreasing_after_restore = true;
        report.passed = true;
    } catch (error) {
        await snapshot('failure');
        fs.writeFileSync(path.join(dir, 'failure-private.txt'), String(error.stack ?? error));
        report.error_name = error.name; process.exitCode = 1;
    } finally {
        if (adapter) {
            try { await adapter.permitJoin(0, 0); report.association_window_closed = true; } catch { report.close_join_error = true; }
            try { await adapter.stop(); } catch { report.close_error = true; process.exitCode = 1; }
        }
        clearTimeout(deadline); save(); console.log(JSON.stringify(report));
    }
}
run().catch(error => { console.error('Radio lab preflight failed: ' + error.name); process.exitCode = 1; });
