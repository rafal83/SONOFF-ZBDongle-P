/* SPDX-License-Identifier: MIT
 * Read-only ZNP network backup. No adapter.start(), commissioning or writes.
 * All sensitive output is confined to an explicitly private directory.
 */
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const lab = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
const allowed = new Set(['ping', 'version', 'getExtAddr', 'osalNvLength', 'osalNvReadExt', 'nvLength', 'nvRead']);

function compareNetwork(before, after) {
    const same = (a, b, field) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw Error(`Network mismatch: ${field}`); };
    for (const field of ['coordinator_ieee', 'pan_id', 'extended_pan_id', 'channel', 'channel_mask', 'security_level'])
        same(before[field], after[field], field);
    same(before.network_key.key, after.network_key.key, 'network key');
    same(before.network_key.sequence_number, after.network_key.sequence_number, 'key sequence');
    same(before.stack_specific?.zstack?.tclk_seed, after.stack_specific?.zstack?.tclk_seed, 'TCLK seed');
    if (after.network_key.frame_counter < before.network_key.frame_counter) throw Error('Network frame counter decreased');
    const devices = new Map(after.devices.map(d => [d.ieee_address, d]));
    for (const previous of before.devices) {
        const current = devices.get(previous.ieee_address);
        if (!current) throw Error('A previous network device is missing');
        if (previous.link_key) {
            same(previous.link_key.key, current.link_key?.key, 'device link key');
            if (current.link_key.rx_counter < previous.link_key.rx_counter || current.link_key.tx_counter < previous.link_key.tx_counter)
                throw Error('A device security counter decreased');
        }
    }
    return true;
}

async function main() {
    const args = {};
    for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].slice(2)] = process.argv[i + 1];
    if (!args.port || !args.output || !args.database || !args['old-backup'] || !args['expect-ieee']) throw Error('Missing arguments');
    const output = path.resolve(args.output);
    const privateRoot = fs.realpathSync(path.join(ROOT, 'private'));
    if (!fs.realpathSync(path.dirname(output)).startsWith(privateRoot + path.sep) || fs.existsSync(output))
        throw Error('Output must be new and private');
    const logPath = output + '.private.log';
    const log = message => fs.appendFileSync(logPath, `${typeof message === 'function' ? message() : message}\n`);
    lab('zigbee-herdsman/dist/utils/logger.js').setLogger({debug: log, info: log, warning: log, error: log});
    const {AdapterTransport} = lab('zigbee-herdsman/dist/adapter/transport.js');
    const {Znp} = lab('zigbee-herdsman/dist/adapter/z-stack/znp/index.js');
    const {AdapterNvMemory} = lab('zigbee-herdsman/dist/adapter/z-stack/adapter/adapter-nv-memory.js');
    const {AdapterBackup} = lab('zigbee-herdsman/dist/adapter/z-stack/adapter/adapter-backup.js');
    const {BackupUtils} = lab('zigbee-herdsman/dist/utils/index.js');
    const {Subsystem} = lab('zigbee-herdsman/dist/adapter/z-stack/unpi/constants.js');
    const baudrate = Number(args.baudrate ?? 115200);
    if (![115200, 1000000].includes(baudrate)) throw Error('Unexpected baudrate');
    const znp = new Znp(new AdapterTransport({path: args.port, baudRate: baudrate, rtscts: false}));
    const request = znp.request.bind(znp), counts = {};
    znp.request = async (subsystem, name, ...rest) => {
        if (subsystem !== Subsystem.SYS || !allowed.has(name)) throw Error('Non-read-only ZNP command blocked');
        counts[name] = (counts[name] ?? 0) + 1;
        return request(subsystem, name, ...rest);
    };
    try {
        await znp.open();
        const version = (await znp.requestWithReply(Subsystem.SYS, 'version', {})).payload;
        if (version.revision !== Number(args['expected-revision']) || version.product !== 1) throw Error('Firmware identity mismatch');
        const nv = new AdapterNvMemory(znp);
        await nv.init();
        const rows = fs.readFileSync(args.database, 'utf8').split(/\r?\n/).filter(x => x.trim()).map(x => JSON.parse(x));
        const backup = new AdapterBackup(znp, nv, args['old-backup']);
        const data = BackupUtils.toUnifiedBackup(await backup.createBackup(rows.map(d => d.ieeeAddr)));
        if ('0x' + data.coordinator_ieee !== args['expect-ieee'].toLowerCase()) throw Error('Coordinator identity mismatch');
        const previous = JSON.parse(fs.readFileSync(args.compare ?? args['old-backup'], 'utf8'));
        if (!args.compare) {
            const active = new Set(rows.filter(d => !['Coordinator', 'Group'].includes(d.type)).map(d => d.ieeeAddr));
            previous.devices = previous.devices.filter(d => active.has('0x' + d.ieee_address));
        }
        compareNetwork(previous, data);
        fs.writeFileSync(output, JSON.stringify(data, null, 2) + '\n', {flag: 'wx'});
        const dbDevices = rows.filter(d => !['Coordinator', 'Group'].includes(d.type));
        const known = new Set(data.devices.map(d => '0x' + d.ieee_address));
        const report = {passed: true, revision: version.revision, baudrate, read_only: true,
            adapter_start_called: false, network_parameters_and_keys_preserved: true,
            previous_devices_preserved: true, security_counters_non_decreasing: true,
            database_devices: dbDevices.length, backup_devices: data.devices.length,
            database_devices_absent_from_backup: dbDevices.filter(d => !known.has(d.ieeeAddr)).length,
            memory_alignment: nv.memoryAlignment, command_counts: counts};
        fs.writeFileSync(output + '.report.json', JSON.stringify(report, null, 2) + '\n', {flag: 'wx'});
        console.log(JSON.stringify(report));
    } catch (error) {
        log(error.stack);
        throw Error('Read-only network verification failed; diagnostics retained privately');
    } finally { await znp.close(); }
}

if (require.main === module) main().catch(error => { console.error(error.message); process.exitCode = 1; });
module.exports = {compareNetwork, allowed};
