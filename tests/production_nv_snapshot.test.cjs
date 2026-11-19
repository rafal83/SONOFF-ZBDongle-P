const {test} = require('node:test');
const assert = require('node:assert/strict');
const {compareNetwork, allowed} = require('../tools/production_nv_snapshot.cjs');
const fixture = () => ({coordinator_ieee:'test-coordinator', pan_id:'test-pan', extended_pan_id:'test-ext-pan',
    channel:25, channel_mask:[25], security_level:5, network_key:{key:'synthetic-key', sequence_number:0, frame_counter:100},
    stack_specific:{zstack:{tclk_seed:'synthetic-seed'}},
    devices:[{ieee_address:'test-peer',link_key:{key:'synthetic-peer-key', rx_counter:10, tx_counter:20}}]});
test('accepts unchanged network with advancing security counters', () => {
    const before=fixture(), after=fixture(); after.network_key.frame_counter++;
    after.devices[0].link_key.tx_counter++; assert.equal(compareNetwork(before,after),true);
});
test('rejects changes to network identity and keys without printing values', () => {
    for (const mutate of [b=>b.channel++, b=>b.network_key.key='other-secret',
        b=>b.stack_specific.zstack.tclk_seed='other-secret', b=>b.devices[0].link_key.key='other-secret']) {
        const after=fixture(); mutate(after);
        assert.throws(()=>compareNetwork(fixture(),after), error=>!error.message.includes('other-secret'));
    }
});
test('rejects missing devices and security counter regression', () => {
    for (const mutate of [b=>b.devices=[], b=>b.network_key.frame_counter--,
        b=>b.devices[0].link_key.tx_counter--, b=>b.devices[0].link_key.rx_counter--]) {
        const after=fixture(); mutate(after); assert.throws(()=>compareNetwork(fixture(),after));
    }
});
test('allowlist excludes all NV writes, reset, startup and association requests', () => {
    for (const command of ['nvWrite','nvCreate','osalNvWriteExt','osalNvDelete','resetReq','startupFromApp','mgmtPermitJoinReq'])
        assert.equal(allowed.has(command),false);
});
