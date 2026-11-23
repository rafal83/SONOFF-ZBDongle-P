/* SPDX-License-Identifier: MIT
 * Zigbee2MQTT external extension: diagnostics of the FreeRTOS coordinator
 * (SONOFF ZBDongle-P) read while Zigbee2MQTT runs.
 *
 * The firmware serves its diagnostics blocks as read-only NV items
 * 0x0FA0..0x0FA8, read with the standard ZNP command SYS osalNvRead through
 * herdsman's own request queue (no collision on the serial link, nothing
 * written). Another coordinator firmware answers "item missing": the
 * extension then stays idle.
 *
 * Install: copy this file to <zigbee2mqtt data>/external_extensions/, restart
 * Zigbee2MQTT (2.x API: mqtt.publish(topic, payload, {clientOptions, baseTopic})). Published on <base>/bridge/coordinator_diag every
 * READ_PERIOD_S seconds and on request: publish anything to
 * <base>/bridge/request/coordinator_diag. With Home Assistant discovery
 * enabled in Zigbee2MQTT, a few sensors are announced. Network addresses in
 * the events and link tables are given with the device's friendly name.
 */
'use strict';

const NV_FIRST = 0x0FA0;
const SYS = 1;                 /* MT subsystem SYS */
const READ_PERIOD_S = 300;
const FIRST_READ_S = 30;
/* Radio quality (frames without ACK, channel access failures): over the
 * last RADIO_WINDOW_S, never counting the first RADIO_WARMUP_S after a
 * restart of the dongle (routes being rebuilt, offline routers tried). */
const RADIO_WARMUP_S = 600;
const RADIO_WINDOW_S = 1800;
const RADIO_MIN_FRAMES = 20;

const PERF_NAMES = ['aes_sw_block_cycles', 'ccm_sw_frame_cycles', 'ccm_hw_frame_cycles', 'hw_aes_state',
    'znp_last_cycles', 'znp_max_cycles', 'rx_last_cycles', 'rx_max_cycles', 'tick_max_cycles',
    'rx_latency_last_us', 'rx_latency_max_us', 'rx_measured', 'cpu_load_permille', 'cpu_load_peak_permille',
    'radio_stack_free_words', 'host_stack_free_words', 'supervisor_stack_free_words', 'idle_stack_free_words',
    'timer_stack_free_words_unused', 'ccm_hw_ops', 'ccm_hw_faults', 'ccm_hw_mismatches', 'ccm_sw_redo', 'ccm_busy',
    'ccm_hw_audits', 'ccm_hw_audit_failures', 'ccm_hw_disabled', 'aes_timeouts', 'aes_dma_errors', 'aes_key_errors',
    'aes_resets', 'aes_reset_timeouts', 'ccm_hw_block_frame_cycles', 'ccm_native_ops', 'ccm_tag_suspect',
    'aes_ccm_ops', 'aes_key_check_failures', 'gp_tx_started', 'gp_tx_ok', 'gp_tx_failed', 'gp_tx_missed',
    'gp_coord_tx_deferred', 'gp_channel_moves', 'gp_channel_failed', 'ccm_prof_setup_cycles_sum',
    'ccm_prof_aad_cycles_sum', 'ccm_prof_data_cycles_sum', 'ccm_prof_tail_cycles_sum', 'touchlink_idle_timeouts',
    'touchlink_rx_dropped', 'touchlink_restore_failed', 'touchlink_restore_retries', 'touchlink_restore_recovered',
    'touchlink_radio_restarts', 'touchlink_reboots', 'touchlink_recovering'];
const MESH_NAMES = ['nwk_commands', 'link_status_rx', 'link_status_tx', 'route_records', 'rreq_rx', 'rrep_rx', 'rreq_tx',
    'mtorr_tx', 'network_status_rx', 'relayed', 'relay_dropped', 'not_for_us', 'broadcast_duplicates', 'no_route',
    'update_device', 'tunnels_sent', 'rejoins', 'tc_rejoins', 'leaves', 'timeout_requests', 'neighbors',
    'znp_out_dropped', 'znp_in_refused', 'route_failures', 'neighbors_dropped', 'routes_expired', 'early_mtorr',
    'early_mtorr_deferred', 'neighbor_restarts', 'tx_not_link_failures', 'bcast_queue_full', 'link_status_failures',
    'aps_duplicates', 'mac_queue_full', 'mac_pool_full', 'mac_tx_busy', 'mac_kicks_deferred', 'mac_tx_rounds',
    'disc_buffered', 'disc_sent', 'disc_expired', 'rreq_coalesced', 'aps_counter_reuse', 'rreq_suppressed',
    'bcast_dup_unopened', 'net_status_retries'];
const COORD_NAMES = ['tx_frames', 'tx_retries', 'tx_no_ack', 'tx_busy', 'tx_error', 'rx_frames', 'rx_filtered',
    'rx_duplicates', 'rx_malformed', 'beacons', 'assoc_requests', 'data_requests', 'polls_served', 'polls_empty',
    'indirect_expired', 'queue_full', 'pool_full', 'associations_accepted', 'associations_denied',
    'transport_keys_sent', 'transport_key_failures', 'announces', 'joined', 'nwk_rx', 'nwk_malformed',
    'nwk_unsecured_dropped', 'nwk_unknown_source', 'nwk_auth_failures', 'aps_malformed', 'aps_data_up',
    'aps_auth_failures', 'aps_replays', 'key_requests', 'link_keys_sent', 'keys_verified', 'key_verify_failures',
    'aps_acks_sent', 'aps_acks_rx', 'zdo_requests', 'zdo_responses_sent', 'aps_tx', 'aps_tx_failures',
    'aps_events_dropped', 'devices', 'permit', 'pan_id', 'channel', 'store_writes', 'store_failures',
    'restored_devices', 'restored'];
const COUNTER_NAMES = ['uart_rx_bytes', 'uart_tx_bytes', 'uart_rx_overflow', 'uart_hw_overrun', 'uart_framing',
    'uart_parity', 'uart_break', 'uart_isr_count', 'uart_isr_max_cycles', 'uart_rx_high_water', 'uart_tx_high_water',
    'mt_accepted', 'mt_bad_fcs', 'mt_oversized', 'mt_timeouts', 'mt_invalid_type', 'mt_resync_discarded',
    'mt_rx_errors', 'tx_dropped', 'tx_dropped_normal', 'tx_dropped_control', 'tx_high_water', 'wd_feeds', 'wd_misses',
    'wd_last_missing', 'wd_worst_consecutive', 'radio_frames_dropped'];
const RELIABILITY_NAMES = ['zcl_resends', 'zcl_resends_deferred', 'zcl_route_rebuilds', 'zcl_answers_dropped',
    'zcl_resends_sleepy_skipped', 'capabilities_learnt', 'poll_children', 'children_aged', 'children_moved',
    'nwk_counters_stored', 'nwk_counters_restored', 'bcast_seq_skipped', 'learned_members', 'addresses_reassigned',
    'leaves', 'update_device', 'rejoins', 'tc_rejoins', 'interpan_rx', 'interpan_tx', 'store_writes',
    'store_failures', 'reads_unanswered', 'relays_flagged', 'parent_annce_sent', 'parent_annce_rsp_sent',
    'parent_annce_children_released', 'children', 'sleepy_children', 'indirect_short', 'indirect_ext',
    'indirect_short_peak', 'indirect_ext_peak', 'parent_annce_rsp_ignored'];
const EVENT_KINDS = {1: 'leave', 2: 'update_device', 3: 'rejoin', 4: 'association', 5: 'address_conflict',
    6: 'child_aged', 7: 'child_moved', 8: 'removed', 9: 'member_learned', 10: 'aps_auth_failure'};
const APS_KEY_ID = ['link key', 'network key', 'key-transport key', 'key-load key'];
const STORE_STATUS = {0: 'mounted', 1: 'formatted (was blank)', 2: 'CORRUPT', 3: 'I/O error', 4: 'bad parameters',
    5: 'not mounted', 6: 'migrated (layout 1 -> 2)'};
const RESET_SOURCE = {0: 'power_on', 1: 'reset_pin', 2: 'vdds_brownout', 3: 'vdd_brownout', 4: 'vddr_brownout',
    5: 'clock_loss', 6: 'system_reset', 7: 'warm_reset'};
const CRASH_KIND = {0: 'none', 1: 'assert', 2: 'stack_overflow', 3: 'hard_fault', 4: 'unexpected_irq', 5: 'watchdog',
    6: 'requested_reset'};
const RESTORE_STATUS = {0: 'no network', 1: 'restored', 2: 'network record invalid', 3: 'counters missing',
    4: 'counters invalid', 5: 'counters of another key', 6: 'counters incoherent'};

const hex16 = (v) => '0x' + v.toString(16).padStart(4, '0');
const ieee = (b) => '0x' + Buffer.from(b).reverse().toString('hex');

/* Count-prefixed little-endian u32 list. */
function counters(b, names) {
    const n = b[0], out = {};
    for (let i = 0; i < n && 1 + 4 * (i + 1) <= b.length; i++) out[names[i] || `value_${i}`] = b.readUInt32LE(1 + 4 * i);
    return out;
}
function info(b) {
    if (b.length < 74) return {raw: b.toString('hex')};
    const out = {format: b[0], version: `${b[1]}.${b[2]}.${b[3]}`, build: b.readUInt32LE(4), uptime_s: Math.round(b.readUInt32LE(8) / 1000),
        reset_source: RESET_SOURCE[b[12]] ?? b[12], previous_crash_kind: CRASH_KIND[b[13]] ?? b[13],
        previous_crash_where: b.subarray(30, 46).toString('latin1').replace(/\0.*$/s, ''),
        boots_with_crash_record: b.readUInt32LE(46),
        cpu_load_permille: b.readUInt16LE(50), cpu_peak_permille: b.readUInt16LE(52),
        sram_static: b.readUInt32LE(59), flash_used: b.readUInt32LE(63), uart_baud: b.readUInt32LE(70)};
    if (b.length >= 79) { out.source_commit = b.readUInt32LE(74).toString(16).padStart(8, '0'); out.source_modified = !!b[78]; }
    return out;
}
function store(b) {
    const out = {mount: STORE_STATUS[b[1]] ?? b[1], mounted: !!b[2], restore: RESTORE_STATUS[b[3]] ?? b[3],
        generation: b.readUInt32LE(4), write_offset: b.readUInt32LE(8), compactions: b.readUInt32LE(20)};
    if (b[0] >= 2 && b.length >= 49) out.counter_floor_from_store = !!b[40];
    return out;
}
function events(b) {
    const total = b.readUInt32LE(1), n = b[5], list = [];
    for (let i = 0, k = 6; i < n && k + 18 <= b.length; i++, k += 18) {
        const e = {age_s: Math.round(b.readUInt32LE(k) / 1000), kind: EVENT_KINDS[b[k + 4]] ?? b[k + 4], detail: b[k + 5],
            nwk: hex16(b.readUInt16LE(k + 6)), from: hex16(b.readUInt16LE(k + 8)), ieee: ieee(b.subarray(k + 10, k + 18))};
        if (e.kind === 'aps_auth_failure') {   /* detail: key id, reason */
            e.key = APS_KEY_ID[e.detail & 3];
            e.reason = e.detail & 0x40 ? 'security header refused' : e.detail & 0x10 ? 'no link key for the source' : 'MIC check failed';
        }
        list.push(e);
    }
    return {total, recent: list};
}
/* Destinations whose reads stay unanswered (and the first relay of their
 * route), relays whose reads needed a re-send; worst first. */
function links(b) {
    const out = {format: b[0], destinations: [], relays: []};
    let k = 1;
    const n = b[k++];
    for (let i = 0; i < n && k + 12 <= b.length; i++, k += 12)
        out.destinations.push({age_s: Math.round(b.readUInt32LE(k) / 1000), nwk: hex16(b.readUInt16LE(k + 4)),
            via: hex16(b.readUInt16LE(k + 6)), unanswered: b.readUInt16LE(k + 8), answered: b.readUInt16LE(k + 10)});
    const m = k < b.length ? b[k++] : 0;
    for (let i = 0; i < m && k + 16 <= b.length; i++, k += 16)
        out.relays.push({age_s: Math.round(b.readUInt32LE(k) / 1000), nwk: hex16(b.readUInt16LE(k + 4)),
            unanswered: b.readUInt16LE(k + 6), answered: b.readUInt16LE(k + 8), resent: b.readUInt16LE(k + 10),
            flagged_s: Math.round(b.readUInt32LE(k + 12) / 1000)});
    out.destinations.sort((x, y) => y.unanswered - x.unanswered);
    out.relays.sort((x, y) => y.flagged_s - x.flagged_s || y.resent - x.resent);
    out.flagged_now = out.relays.filter((r) => r.flagged_s > 0).length;
    return out;
}
const BLOCKS = [['info', info], ['uart', (b) => counters(b, COUNTER_NAMES)], ['coordinator', (b) => counters(b, COORD_NAMES)],
    ['mesh', (b) => counters(b, MESH_NAMES)], ['store', store], ['perf', (b) => counters(b, PERF_NAMES)], ['events', events],
    ['reliability', (b) => counters(b, RELIABILITY_NAMES)], ['links', links]];

/* One NV item, in chunks (osalNvRead offset is one byte). null: not served. */
async function readItem(znp, id) {
    const parts = [];
    for (let offset = 0; offset < 256;) {
        const r = (await znp.request(SYS, 'osalNvRead', {id, offset}, undefined, undefined, [0, 0x0a])).payload;
        if (r.status !== 0 || !r.len) break;
        parts.push(Buffer.from(r.value));
        offset += r.len;
        if (r.len < 246) break;
    }
    return parts.length ? Buffer.concat(parts) : null;
}

/* All blocks decoded; {supported: false} on another firmware. */
async function readDiagnostics(znp) {
    const out = {supported: true, time: new Date().toISOString()};
    for (let i = 0; i < BLOCKS.length; i++) {
        const raw = await readItem(znp, NV_FIRST + i);
        if (!raw) { if (i === 0) return {supported: false}; continue; }
        out[BLOCKS[i][0]] = BLOCKS[i][1](raw);
    }
    return out;
}

class CoordinatorDiagnostics {
    constructor(zigbee, mqtt, state, publishEntityState, eventBus, enableDisableExtension, restartCallback, addExtension,
        settings, logger) {
        this.zigbee = zigbee;
        this.mqtt = mqtt;
        this.eventBus = eventBus;
        this.settings = settings;
        this.logger = logger;
        this.timer = null;
        this.base = settings?.get?.().mqtt?.base_topic ?? 'zigbee2mqtt';
        this.samples = [];   /* MAC transmit counters of the last reads (radio quality window) */
    }

    znp() {
        const herdsman = this.zigbee?.herdsman ?? this.zigbee?.zhController;
        return herdsman?.adapter?.znp;   /* Z-Stack adapter only */
    }

    async start() {
        const haStatus = this.settings?.get?.().homeassistant?.status_topic ?? 'homeassistant/status';
        this.eventBus.onMQTTMessage(this, (data) => {
            /* A request, or Home Assistant coming back online: read now and
             * announce the sensors again (entities deleted in HA come back). */
            const request = data.topic === `${this.base}/bridge/request/coordinator_diag`;
            if (request || (data.topic === haStatus && String(data.message).trim() === 'online')) {
                this.discovered = false;
                this.publish().catch(() => {});
            }
        });
        this.first = setTimeout(() => this.publish().catch(() => {}), FIRST_READ_S * 1000);
        this.timer = setInterval(() => this.publish().catch(() => {}), READ_PERIOD_S * 1000);
    }

    async stop() {
        clearTimeout(this.first);
        clearInterval(this.timer);
        this.eventBus.removeListeners(this);
    }

    /* Shares of the frames sent between the oldest read of the window taken
     * after the warm-up and this one; null (unknown in Home Assistant) until
     * such a read exists. Without one (Zigbee2MQTT just restarted), a dongle
     * up for an hour or more gives its counts since boot. */
    radioQuality(diag) {
        const c = diag.coordinator ?? {}, up = diag.info?.uptime_s ?? 0;
        const s = {up, frames: c.tx_frames ?? 0, no_ack: c.tx_no_ack ?? 0, busy: c.tx_busy ?? 0, retries: c.tx_retries ?? 0};
        const last = this.samples.at(-1);
        if (last && (up < last.up || s.frames < last.frames)) this.samples = [];   /* the dongle restarted */
        this.samples.push(s);
        while (this.samples.length > 1 && up - this.samples[0].up > RADIO_WINDOW_S) this.samples.shift();
        let base = this.samples.find((x) => x !== s && x.up >= RADIO_WARMUP_S);
        if (!base && up >= 3600) base = {up: 0, frames: 0, no_ack: 0, busy: 0, retries: 0};
        const frames = base ? s.frames - base.frames : 0;
        const pct = (k) => (base && frames >= RADIO_MIN_FRAMES ? Math.round(1000 * (s[k] - base[k]) / frames) / 10 : null);
        return {window_s: base ? up - base.up : 0, frames, no_ack_pct: pct('no_ack'), channel_busy_pct: pct('busy'),
            retries_per_100: pct('retries')};
    }

    /* NWK address -> friendly name (Zigbee2MQTT 2.x devicesIterator). */
    names() {
        const map = new Map([[0, 'Coordinator']]);
        try {
            for (const d of this.zigbee?.devicesIterator?.() ?? []) {
                const nwk = d?.zh?.networkAddress;
                if (nwk !== undefined) map.set(nwk, d.name ?? d.ieeeAddr);
            }
        } catch { /* names are a convenience */ }
        return map;
    }

    /* Names next to the addresses of the events and link tables. */
    annotate(diag) {
        const names = this.names();
        const name = (h) => names.get(parseInt(h, 16));
        for (const e of diag.events?.recent ?? []) { e.name = name(e.nwk); if (e.from !== '0xffff') e.from_name = name(e.from); }
        for (const d of diag.links?.destinations ?? []) { d.name = name(d.nwk); d.via_name = name(d.via); }
        for (const r of diag.links?.relays ?? []) r.name = name(r.nwk);
    }

    async publish() {
        const znp = this.znp();
        if (!znp) return;
        let diag;
        try {
            diag = await readDiagnostics(znp);
        } catch (error) {
            this.logger?.warning?.(`coordinator-diag: read failed (${error.message})`);
            return;
        }
        if (!diag.supported) {   /* not the FreeRTOS firmware: stop polling */
            clearInterval(this.timer);
            this.logger?.info?.('coordinator-diag: coordinator firmware without diagnostics items, extension idle');
            return;
        }
        diag.links ??= {destinations: [], relays: [], flagged_now: 0};   /* image without the block */
        this.annotate(diag);
        diag.radio = this.radioQuality(diag);
        await this.mqtt.publish('bridge/coordinator_diag', JSON.stringify(diag), {clientOptions: {retain: true, qos: 0}});
        if (!this.discovered && this.settings?.get?.().homeassistant?.enabled) await this.discover(diag);
    }

    async discover(diag) {
        this.discovered = true;
        const prefix = this.settings.get().homeassistant?.discovery_topic ?? 'homeassistant';
        const commit = diag.info?.source_commit ? diag.info.source_commit + (diag.info.source_modified ? ' (modifié)' : '') : undefined;
        const device = {identifiers: ['zigbee2mqtt_coordinator_diag'], name: 'Coordinateur Zigbee (diagnostic)',
            manufacturer: 'ZBDongle-P FreeRTOS', model: 'CC2652P, coordinateur FreeRTOS',
            ...(diag.info?.build ? {sw_version: `${diag.info.build}${commit ? ' / ' + commit : ''}`} : {})};
        const total = {state_class: 'total_increasing'};
        const attrs = (path) => ({json_attributes_template: `{{ value_json.${path} | tojson }}`});
        /* [id, name, value template, unit, extra discovery fields] */
        const sensors = [
            ['cpu_load', 'Charge CPU', '{{ value_json.perf.cpu_load_permille / 10 }}', '%', {state_class: 'measurement'}],
            ['radio_stack_free', 'Pile radio libre', '{{ value_json.perf.radio_stack_free_words * 4 }}', 'B',
                {device_class: 'data_size', state_class: 'measurement'}],
            ['uptime', 'Uptime', '{{ value_json.info.uptime_s }}', 's',
                {device_class: 'duration', state_class: 'measurement', suggested_display_precision: 0}],
            ['leaves', 'Départs annoncés', '{{ value_json.reliability.leaves }}', null, {state_class: 'total_increasing'}],
            ['events', 'Événements réseau', '{{ value_json.events.total }}', null, {...total, ...attrs('events')}],
            /* 1. restarts */
            ['reset_source', 'Dernier redémarrage', '{{ value_json.info.reset_source }}', null, attrs('info')],
            ['previous_crash', 'Dernier plantage',
                "{{ value_json.info.previous_crash_kind ~ ((' : ' ~ value_json.info.previous_crash_where) if value_json.info.previous_crash_where else '') }}",
                null, {}],
            ['boots_with_crash', 'Redémarrages sur plantage', '{{ value_json.info.boots_with_crash_record }}', null, {}],
            /* 2. mesh */
            ['neighbors', 'Voisins routeurs', '{{ value_json.mesh.neighbors }}', null, {state_class: 'measurement'}],
            ['children', 'Enfants directs', '{{ value_json.reliability.children }}', null, {state_class: 'measurement'}],
            ['sleepy_children', 'Enfants endormis', '{{ value_json.reliability.sleepy_children }}', null,
                {state_class: 'measurement'}],
            ['indirect_queue', 'File indirecte (trames en attente)',
                '{{ value_json.reliability.indirect_short + value_json.reliability.indirect_ext }}', null,
                {state_class: 'measurement', json_attributes_template:
                    '{{ {"short": value_json.reliability.indirect_short, "ieee": value_json.reliability.indirect_ext, ' +
                    '"short_peak": value_json.reliability.indirect_short_peak, "ieee_peak": value_json.reliability.indirect_ext_peak} | tojson }}'}],
            /* 3. radio */
            ['no_ack', 'Trames sans ACK (30 min)', '{{ value_json.radio.no_ack_pct }}', '%', {state_class: 'measurement'}],
            ['channel_busy', 'Échecs d’accès au canal (30 min)', '{{ value_json.radio.channel_busy_pct }}', '%', {state_class: 'measurement'}],
            /* 4. security */
            ['nwk_auth_failures', 'Échecs d’authentification NWK', '{{ value_json.coordinator.nwk_auth_failures }}', null, total],
            ['aps_auth_failures', 'Échecs d’authentification APS', '{{ value_json.coordinator.aps_auth_failures }}', null, total],
            ['aps_replays', 'Rejeux APS refusés', '{{ value_json.coordinator.aps_replays }}', null, total],
            ['tc_rejoins', 'Rejoins Trust Center', '{{ value_json.reliability.tc_rejoins }}', null, total],
            /* 5. links */
            ['reads_unanswered', 'Lectures sans réponse', '{{ value_json.reliability.reads_unanswered }}', null,
                {...total, ...attrs('links.destinations')}],
            ['relays_flagged', 'Relais qui perdent des réponses', '{{ value_json.links.flagged_now }}', null,
                {state_class: 'measurement', ...attrs('links.relays')}],
            ['parent_annce_released', 'Enfants repris par un routeur (Parent_annce)',
                '{{ value_json.reliability.parent_annce_children_released }}', null, total],
            ['store_offset', 'Store : octets écrits', '{{ value_json.store.write_offset }}', 'B',
                {device_class: 'data_size', state_class: 'measurement'}],
            ['touchlink_recovering', 'Touchlink : récupération radio', '{{ value_json.perf.touchlink_recovering }}', null, {}],
            ['source_commit', 'Commit du firmware',
                "{{ value_json.info.source_commit ~ (' (modifié)' if value_json.info.source_modified else '') }}", null, {}],
        ];
        for (const [id, name, template, unit, extra] of sensors) {
            const topic = `${this.base}/bridge/coordinator_diag`;
            const config = {name, unique_id: `zigbee2mqtt_coordinator_diag_${id}`, state_topic: topic,
                value_template: template, device, entity_category: 'diagnostic',
                ...(unit ? {unit_of_measurement: unit} : {}), ...extra,
                ...(extra.json_attributes_template ? {json_attributes_topic: topic} : {})};
            await this.mqtt.publish(`sensor/zigbee2mqtt_coordinator_diag/${id}/config`, JSON.stringify(config),
                {clientOptions: {retain: true, qos: 1}, baseTopic: prefix});
        }
    }
}

module.exports = CoordinatorDiagnostics;
module.exports.readDiagnostics = readDiagnostics;
module.exports.decode = {info, store, events, counters, links};
