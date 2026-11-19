/* SPDX-License-Identifier: MIT
 * Generate firmware/freertos/transport/znp/znp_structs.h from the Z-Stack NV
 * structure definitions of the pinned zigbee-herdsman 11.2.1 (the host that
 * will read and write these items). Each field offset is MEASURED by
 * serializing the herdsman struct in its "aligned" (CC26xx) variant with a
 * marker value, so the firmware layout is herdsman's layout by construction.
 *
 *   node tools/gen_znp_structs.cjs
 */
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const ROOT = path.resolve(__dirname, '..');
const r = createRequire(path.join(ROOT, '.tooling/herdsman-lab/package.json'));
if (r('zigbee-herdsman/package.json').version !== '11.2.1') throw new Error('herdsman version mismatch');
const S = r('zigbee-herdsman/dist/adapter/z-stack/structs/index.js');

const SIZES = {uint8: 1, uint16: 2, uint32: 4};
function measure(factory, prefix) {
    const base = factory();
    const zero = base.serialize('aligned');
    const out = [`#define ${prefix}_SIZE ${zero.length}u`];
    for (const m of base.members) {
        if (m.type === 'struct') continue;
        const obj = factory();
        let width;
        if (SIZES[m.type]) {
            width = SIZES[m.type];
            obj[m.key] = width === 1 ? 0xA5 : width === 2 ? 0xA55A : 0xA55A5AA5;
        } else if (m.type.startsWith('uint8array')) {
            width = m.length;
            obj[m.key] = Buffer.alloc(width, 0xA5);
        } else {
            continue;
        }
        const buf = obj.serialize('aligned');
        let offset = -1;
        for (let i = 0; i < buf.length; i++) if (buf[i] !== zero[i]) { offset = i; break; }
        if (offset < 0) throw new Error(`cannot locate ${prefix}.${m.key}`);
        const reversed = m.type === 'uint8array-reversed' ? ' /* reversed on the wire */' : '';
        out.push(`#define ${prefix}_${m.key.toUpperCase()} ${offset}u /* ${m.type}${m.length ? '[' + m.length + ']' : ''} */${reversed}`);
    }
    // nested structs (NIB spare key descriptors) are located by offset of the next member
    return out.join('\n');
}

const sections = [
    ['ZNP_NIB', S.nib], ['ZNP_KEYDESC', S.nwkKeyDescriptor], ['ZNP_HASCFG', S.hasConfigured],
    ['ZNP_AME', S.addressManagerEntry], ['ZNP_TCLK', S.apsTcLinkKeyEntry],
    ['ZNP_SECMAT', S.nwkSecMaterialDescriptorEntry], ['ZNP_APSKEY', S.apsLinkKeyDataEntry],
    ['ZNP_SME', S.securityManagerEntry],
];
const header = [
    '/* SPDX-License-Identifier: MIT',
    ' * Generated from zigbee-herdsman 11.2.1 (aligned layout).',
    ' * Copyright (c) 2019 Jack Wu, Simen Li, Hedy Wang and Koen Kanters.',
    ' * See LICENSES/zigbee-herdsman-MIT.txt for the complete upstream notice.',
    ' */',
    '#ifndef ZNP_STRUCTS_H', '#define ZNP_STRUCTS_H',
    ...sections.map(([p, f]) => measure(f, p)),
    '#endif', '',
].join('\n');
const target = path.join(ROOT, 'firmware/freertos/transport/znp/znp_structs.h');
fs.writeFileSync(target, header);
console.log('wrote', path.relative(ROOT, target));
