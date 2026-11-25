/* SPDX-License-Identifier: MIT
 * Coordinator MAC written from IEEE 802.15.4-2006 clauses 7.1, 7.3, 7.5.
 * Bounded work: slot allocation and expiry scan MAC_POOL_SLOTS entries, the
 * extended-address transaction table MAC_EXT_INDIRECT entries; short-address
 * transactions and the duplicate cache use the Patricia index (<= 64 steps).
 */
#include "mac_core.h"

#define NO_SLOT (-1)
#define DUP_SHORT_TAG 0xFFFF000000000000ull

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int alloc_slot(mac_t *m) {
    for (unsigned i = 0; i < MAC_POOL_SLOTS; i++) {
        if (m->slots[i].kind == SLOT_FREE) {
            m->slots[i] = (mac_slot_t){0};
            m->slots[i].token = CORE_INDIRECT_NONE;
            return (int)i;
        }
    }
    m->stats.pool_full++;
    return NO_SLOT;
}

static void free_slot(mac_t *m, int s) { m->slots[s].kind = SLOT_FREE; }

static bool push(uint8_t *q, uint8_t cap, uint8_t *head, uint8_t *count, int s, bool front) {
    if (*count >= cap) return false;
    if (front) {
        *head = (uint8_t)((*head + cap - 1u) % cap);
        q[*head] = (uint8_t)s;
    } else {
        q[(*head + *count) % cap] = (uint8_t)s;
    }
    (*count)++;
    return true;
}

static int pop(uint8_t *q, uint8_t cap, uint8_t *head, uint8_t *count) {
    if (!*count) return NO_SLOT;
    int s = q[*head];
    *head = (uint8_t)((*head + 1u) % cap);
    (*count)--;
    return s;
}

/* dst_pan 0: our PAN (PAN ID compression); else an inter-PAN frame from our
 * extended address (Touchlink). Never inlined: inside mac_data_request its
 * frame descriptor would add 80 bytes to the radio task's deepest stack
 * path (receive -> ZDO answer -> fragmented send -> MAC). */
__attribute__((noinline)) static size_t build(mac_t *m, mac_slot_t *slot, uint8_t type, uint8_t dst_mode, uint16_t dst_short,
                    uint64_t dst_ext, bool ack, const uint8_t *payload, size_t len, uint16_t dst_pan) {
    mac_frame_t f = {0};
    f.type = type;
    f.ack_request = ack;
    f.pan_id_compression = !dst_pan || dst_pan == m->pan_id;
    f.seq = m->dsn++;
    f.dst_mode = dst_mode;
    f.dst_pan = f.pan_id_compression ? m->pan_id : dst_pan;
    f.dst_short = dst_short;
    f.dst_ext = dst_ext;
    if (f.pan_id_compression) {
        f.src_mode = MAC_ADDR_SHORT;
        f.src_short = m->short_addr;
    } else {
        f.src_mode = MAC_ADDR_EXT;
        f.src_pan = m->pan_id;
        f.src_ext = m->ext_addr;
    }
    size_t n = mac_frame_build(&f, payload, len, slot->mpdu, sizeof slot->mpdu);
    slot->length = (uint8_t)n;
    slot->ack = ack;
    slot->dst_short = dst_short;
    slot->dst_ext = dst_ext;
    return n;
}

static void set_pending_bit(mac_slot_t *slot, bool pending) {
    if (pending) slot->mpdu[0] |= 0x10u;
    else slot->mpdu[0] &= (uint8_t)~0x10u;
}

static unsigned ext_pending(mac_t *m, uint64_t device) {
    unsigned n = 0;
    for (unsigned i = 0; i < MAC_EXT_INDIRECT; i++)
        if (m->ext_indirect[i] != NO_SLOT && m->slots[m->ext_indirect[i]].dst_ext == device) n++;
    return n;
}

static void ext_remove(mac_t *m, int s) {
    for (unsigned i = 0; i < MAC_EXT_INDIRECT; i++)
        if (m->ext_indirect[i] == s) m->ext_indirect[i] = NO_SLOT;
}

/* Final outcome of a slot: confirm upwards, update source matching, free. */
static void complete(mac_t *m, int s, uint8_t status) {
    mac_slot_t *slot = &m->slots[s];
    switch (slot->kind) {
    case SLOT_INDIRECT_SHORT:
        (void)core_indirect_release(&m->indirect, slot->token, 0);
        if (!core_indirect_count(&m->indirect, slot->dst_short) && m->radio.source_match)
            m->radio.source_match(m->radio.ctx, false, slot->dst_short, false);
        break;
    case SLOT_INDIRECT_EXT:
        ext_remove(m, s);
        if (!ext_pending(m, slot->dst_ext) && m->radio.source_match)
            m->radio.source_match(m->radio.ctx, true, slot->dst_ext, false);
        break;
    default:
        break;
    }
    /* A confirm callback may queue a new frame (route repair, Network Status,
     * transport key...): kick() is deferred until the callback returned, so a
     * transmission that fails at once cannot recurse back here through the
     * upper layers (completion -> callback -> send -> kick -> completion...).
     * Every caller of complete() kicks the queues afterwards. */
    m->in_callback++;
    if (slot->confirm && m->upper.data_confirm) m->upper.data_confirm(m->upper.ctx, slot->handle, status);
    if (slot->comm_status && m->upper.comm_status) m->upper.comm_status(m->upper.ctx, slot->dst_ext, status);
    m->in_callback--;
    free_slot(m, s);
}

/* Indirect delivery failed: the transaction stays queued for the next poll
 * until its persistence time elapses (no immediate retransmission). */
static void keep_pending(mac_t *m, int s) {
    mac_slot_t *slot = &m->slots[s];
    slot->in_flight = false;
    slot->queued = false;
    if (slot->kind == SLOT_INDIRECT_SHORT) (void)core_indirect_retry(&m->indirect, slot->token);
}

static void kick(mac_t *m) {
    if (m->in_callback) { m->stats.kicks_deferred++; return; }   /* the caller of complete() kicks */
    while (m->active == NO_SLOT) {
        int s = pop(m->urgent_q, MAC_URGENT_QUEUE, &m->urgent_head, &m->urgent_count);
        if (s == NO_SLOT) s = pop(m->direct_q, MAC_DIRECT_QUEUE, &m->direct_head, &m->direct_count);
        if (s == NO_SLOT) return;
        mac_slot_t *slot = &m->slots[s];
        slot->queued = false;
        slot->in_flight = true;
        m->active = (int8_t)s;
        m->stats.tx_frames++;
        if (m->radio.tx(m->radio.ctx, slot->mpdu, slot->length, slot->ack) != 0) {
            m->active = NO_SLOT;
            m->stats.tx_error++;
            if (slot->kind == SLOT_INDIRECT_SHORT || slot->kind == SLOT_INDIRECT_EXT) keep_pending(m, s);
            else complete(m, s, MAC_STATUS_RADIO_ERROR);
        }
    }
}

void mac_init(mac_t *m, const mac_radio_ops_t *radio, const mac_upper_t *upper, uint64_t ext_addr) {
    *m = (mac_t){0};
    m->radio = *radio;
    if (upper) m->upper = *upper;
    m->ext_addr = ext_addr;
    m->short_addr = 0xFFFE;
    m->pan_id = 0xFFFF;
    m->active = NO_SLOT;
    for (unsigned i = 0; i < MAC_EXT_INDIRECT; i++) m->ext_indirect[i] = NO_SLOT;
    core_indirect_init(&m->indirect);
    core_index_init(&m->dup_index, m->dup_branches, m->dup_keys, m->dup_occupied, MAC_DUP_ENTRIES);
    m->beacon.stack_profile = 2;
    m->beacon.protocol_version = 2;
    m->beacon.router_capacity = true;
    m->beacon.end_device_capacity = true;
}

void mac_start(mac_t *m, uint16_t pan_id, uint8_t channel, const uint8_t ext_pan_id[8]) {
    m->pan_id = pan_id;
    m->channel = channel;
    m->short_addr = 0x0000;
    for (unsigned i = 0; i < 8; i++) m->beacon.ext_pan_id[i] = ext_pan_id[i];
    m->started = true;
}

void mac_set_association_permit(mac_t *m, bool permit) { m->association_permit = permit; }

void mac_set_beacon_capacity(mac_t *m, bool router, bool end_device, uint8_t update_id) {
    m->beacon.router_capacity = router;
    m->beacon.end_device_capacity = end_device;
    m->beacon.update_id = update_id;
}

uint8_t mac_data_request(mac_t *m, const mac_data_request_t *r) {
    if (!m->started || !r || (r->length && !r->payload)) return MAC_STATUS_INVALID_PARAMETER;
    if (r->dst_mode != MAC_ADDR_SHORT && r->dst_mode != MAC_ADDR_EXT) return MAC_STATUS_INVALID_PARAMETER;
    if (r->length > MAC_MAX_PAYLOAD) return MAC_STATUS_FRAME_TOO_LONG;
    bool broadcast = r->dst_mode == MAC_ADDR_SHORT && r->dst_short == MAC_BROADCAST_SHORT;
    if (broadcast && r->indirect) return MAC_STATUS_INVALID_PARAMETER;
    if (r->indirect && r->dst_mode == MAC_ADDR_SHORT && r->dst_short >= 0xFFF8u) return MAC_STATUS_INVALID_PARAMETER;
    if (r->indirect) {
        /* Data for sleepy children only. IEEE-addressed transactions (Rejoin
         * Responses, like Association Responses) have their own
         * MAC_EXT_INDIRECT slots: queued data never makes a device's
         * (re)join fail (production, 21 November: rejoin responses refused
         * while 16 frames waited for children that had lost their parent). */
        unsigned waiting = 0;
        for (unsigned i = 0; i < MAC_POOL_SLOTS; i++) waiting += m->slots[i].kind == SLOT_INDIRECT_SHORT;
        if (waiting >= MAC_INDIRECT_SLOTS && r->dst_mode == MAC_ADDR_SHORT) {
            m->stats.queue_full++;
            return MAC_STATUS_TRANSACTION_OVERFLOW;
        }
    }
    int s = alloc_slot(m);
    if (s == NO_SLOT) return MAC_STATUS_TRANSACTION_OVERFLOW;
    mac_slot_t *slot = &m->slots[s];
    bool ack = !broadcast && (r->ack_request || r->indirect);
    if (r->dst_pan && r->dst_pan != m->pan_id && r->indirect) { free_slot(m, s); return MAC_STATUS_INVALID_PARAMETER; }
    if (!build(m, slot, MAC_TYPE_DATA, r->dst_mode, r->dst_short, r->dst_ext, ack, r->payload, r->length, r->dst_pan)) {
        free_slot(m, s);
        return MAC_STATUS_FRAME_TOO_LONG;
    }
    slot->handle = r->handle;
    slot->confirm = true;
    slot->enqueued_ms = m->now_ms;
    if (!r->indirect) {
        slot->kind = SLOT_DIRECT;
        if (!push(m->direct_q, MAC_DIRECT_QUEUE, &m->direct_head, &m->direct_count, s, r->priority)) {
            free_slot(m, s);
            m->stats.queue_full++;
            return MAC_STATUS_TRANSACTION_OVERFLOW;
        }
        slot->queued = true;
        kick(m);
        return MAC_STATUS_SUCCESS;
    }
    if (r->dst_mode == MAC_ADDR_SHORT) {
        slot->kind = SLOT_INDIRECT_SHORT;
        slot->token = core_indirect_enqueue(&m->indirect, r->dst_short, slot);
        if (slot->token == CORE_INDIRECT_NONE) {
            free_slot(m, s);
            m->stats.queue_full++;
            return MAC_STATUS_TRANSACTION_OVERFLOW;
        }
        if (core_indirect_count(&m->indirect, r->dst_short) == 1 && m->radio.source_match)
            m->radio.source_match(m->radio.ctx, false, r->dst_short, true);
        return MAC_STATUS_SUCCESS;
    }
    for (unsigned i = 0; i < MAC_EXT_INDIRECT; i++) {
        if (m->ext_indirect[i] == NO_SLOT) {
            slot->kind = SLOT_INDIRECT_EXT;
            m->ext_indirect[i] = (int8_t)s;
            if (ext_pending(m, r->dst_ext) == 1 && m->radio.source_match)
                m->radio.source_match(m->radio.ctx, true, r->dst_ext, true);
            return MAC_STATUS_SUCCESS;
        }
    }
    free_slot(m, s);
    m->stats.queue_full++;
    return MAC_STATUS_TRANSACTION_OVERFLOW;
}

uint8_t mac_association_response(mac_t *m, uint64_t device, uint16_t short_addr, uint8_t status) {
    if (!m->started) return MAC_STATUS_INVALID_PARAMETER;
    int slot_index = NO_SLOT;
    for (unsigned i = 0; i < MAC_EXT_INDIRECT; i++) {
        if (m->ext_indirect[i] == NO_SLOT) {
            slot_index = alloc_slot(m);
            if (slot_index == NO_SLOT) return MAC_STATUS_TRANSACTION_OVERFLOW;
            mac_slot_t *slot = &m->slots[slot_index];
            slot->length = (uint8_t)mac_build_assoc_response(m->dsn++, m->pan_id, device, m->ext_addr,
                                                             short_addr, status, slot->mpdu, sizeof slot->mpdu);
            slot->kind = SLOT_INDIRECT_EXT;
            slot->ack = true;
            slot->dst_ext = device;
            slot->dst_short = short_addr;
            slot->comm_status = true;
            slot->enqueued_ms = m->now_ms;
            m->ext_indirect[i] = (int8_t)slot_index;
            if (ext_pending(m, device) == 1 && m->radio.source_match)
                m->radio.source_match(m->radio.ctx, true, device, true);
            return MAC_STATUS_SUCCESS;
        }
    }
    m->stats.queue_full++;
    return MAC_STATUS_TRANSACTION_OVERFLOW;
}

uint8_t mac_orphan_response(mac_t *m, uint64_t device, uint16_t short_addr) {
    if (!m->started) return MAC_STATUS_INVALID_PARAMETER;
    int s = alloc_slot(m);
    if (s == NO_SLOT) return MAC_STATUS_TRANSACTION_OVERFLOW;
    mac_slot_t *slot = &m->slots[s];
    mac_frame_t f = {0};
    f.type = MAC_TYPE_COMMAND;
    f.ack_request = true;
    f.seq = m->dsn++;
    f.dst_mode = MAC_ADDR_EXT;
    f.dst_pan = MAC_BROADCAST_PAN;
    f.dst_ext = device;
    f.src_mode = MAC_ADDR_EXT;
    f.src_pan = m->pan_id;
    f.src_ext = m->ext_addr;
    uint8_t p[8] = {MAC_CMD_COORD_REALIGN};
    wr16(p + 1, m->pan_id);
    wr16(p + 3, m->short_addr);
    p[5] = m->channel;
    wr16(p + 6, short_addr);
    slot->length = (uint8_t)mac_frame_build(&f, p, sizeof p, slot->mpdu, sizeof slot->mpdu);
    slot->kind = SLOT_DIRECT;
    slot->ack = true;
    slot->dst_ext = device;
    slot->comm_status = true;
    if (!push(m->direct_q, MAC_DIRECT_QUEUE, &m->direct_head, &m->direct_count, s, false)) {
        free_slot(m, s);
        m->stats.queue_full++;
        return MAC_STATUS_TRANSACTION_OVERFLOW;
    }
    slot->queued = true;
    kick(m);
    return MAC_STATUS_SUCCESS;
}

bool mac_purge(mac_t *m, uint8_t handle) {
    for (unsigned i = 0; i < MAC_POOL_SLOTS; i++) {
        mac_slot_t *slot = &m->slots[i];
        if ((slot->kind == SLOT_INDIRECT_SHORT || slot->kind == SLOT_INDIRECT_EXT) && slot->confirm &&
            slot->handle == handle && !slot->in_flight) {
            slot->confirm = false;  /* MCPS-PURGE: no data confirm */
            complete(m, (int)i, MAC_STATUS_PURGED);
            kick(m);                /* a comm-status callback may have queued a frame */
            return true;
        }
    }
    return false;
}

/* Another round for a direct frame that failed: back to the END of the
 * queue (see MAC_TX_ROUNDS). false when the rounds are used up. */
static bool next_round(mac_t *m, int s) {
    mac_slot_t *slot = &m->slots[s];
    if (slot->rounds + 1u >= MAC_TX_ROUNDS || !push(m->direct_q, MAC_DIRECT_QUEUE, &m->direct_head, &m->direct_count, s, false))
        return false;
    slot->rounds++;
    slot->retries = 0;
    slot->in_flight = false;
    slot->queued = true;
    m->stats.tx_rounds++;
    return true;
}

void mac_tx_done(mac_t *m, mac_tx_result_t result, bool frame_pending) {
    (void)frame_pending;
    int s = m->active;
    if (s == NO_SLOT) return;
    m->active = NO_SLOT;
    mac_slot_t *slot = &m->slots[s];
    bool indirect = slot->kind == SLOT_INDIRECT_SHORT || slot->kind == SLOT_INDIRECT_EXT;
    if (m->upper.tx_attempt && slot->ack && !slot->dst_ext && slot->dst_short < 0xFFF8u &&
        (result == MAC_TX_OK || result == MAC_TX_NO_ACK))
        m->upper.tx_attempt(m->upper.ctx, slot->dst_short, result == MAC_TX_OK);
    switch (result) {
    case MAC_TX_OK:
        complete(m, s, MAC_STATUS_SUCCESS);
        break;
    case MAC_TX_NO_ACK:
        m->stats.tx_no_ack++;
        if (indirect) {
            keep_pending(m, s);
        } else if (slot->retries < MAC_MAX_FRAME_RETRIES &&
                   push(m->direct_q, MAC_DIRECT_QUEUE, &m->direct_head, &m->direct_count, s, true)) {
            slot->retries++;
            slot->in_flight = false;
            slot->queued = true;
            m->stats.tx_retries++;
        } else if (!next_round(m, s)) {
            complete(m, s, MAC_STATUS_NO_ACK);
        }
        break;
    case MAC_TX_BUSY:
        m->stats.tx_busy++;
        if (indirect) keep_pending(m, s);
        else if (!next_round(m, s)) complete(m, s, MAC_STATUS_CHANNEL_ACCESS_FAILURE);
        break;
    default:
        m->stats.tx_error++;
        if (indirect) keep_pending(m, s);
        else complete(m, s, MAC_STATUS_RADIO_ERROR);
        break;
    }
    kick(m);
}

static bool duplicate(mac_t *m, const mac_frame_t *f) {
    if (!f->ack_request || f->src_mode == MAC_ADDR_NONE) return false;
    uint64_t key = f->src_mode == MAC_ADDR_EXT ? f->src_ext
                 : DUP_SHORT_TAG | ((uint64_t)f->src_pan << 16) | f->src_short;
    uint16_t i = core_index_find(&m->dup_index, key);
    if (i != CORE_INDEX_NONE) {
        bool dup = m->dup_seq[i] == f->seq && (uint32_t)(m->now_ms - m->dup_time[i]) < MAC_DUP_WINDOW_MS;
        m->dup_seq[i] = f->seq;
        m->dup_time[i] = m->now_ms;
        return dup;
    }
    /* Round-robin replacement keeps the cache bounded. */
    i = m->dup_next;
    m->dup_next = (uint8_t)((m->dup_next + 1u) % MAC_DUP_ENTRIES);
    if (m->dup_occupied[i / 8] & (1u << (i % 8))) (void)core_index_remove(&m->dup_index, m->dup_keys[i]);
    if (core_index_insert(&m->dup_index, key, i)) {
        m->dup_seq[i] = f->seq;
        m->dup_time[i] = m->now_ms;
    }
    return false;
}

static bool addressed_to_us(const mac_t *m, const mac_frame_t *f) {
    if (f->dst_mode == MAC_ADDR_NONE) {
        /* Only a PAN coordinator accepts frames without destination, and only
         * from its own PAN (802.15.4-2006 7.5.6.2, third-level filtering). */
        return f->src_mode != MAC_ADDR_NONE && f->src_pan == m->pan_id;
    }
    if (f->dst_pan != m->pan_id && f->dst_pan != MAC_BROADCAST_PAN) return false;
    if (f->dst_mode == MAC_ADDR_SHORT) return f->dst_short == m->short_addr || f->dst_short == MAC_BROADCAST_SHORT;
    return f->dst_ext == m->ext_addr;
}

static void send_beacon(mac_t *m) {
    int s = alloc_slot(m);
    if (s == NO_SLOT) return;
    mac_slot_t *slot = &m->slots[s];
    mac_zigbee_beacon_t b = m->beacon;
    b.pan_id = m->pan_id;
    b.short_addr = m->short_addr;
    b.seq = m->bsn++;
    b.association_permit = m->association_permit;
    b.device_depth = 0;
    slot->length = (uint8_t)mac_build_zigbee_beacon(&b, slot->mpdu, sizeof slot->mpdu);
    slot->kind = SLOT_INTERNAL;
    if (!slot->length || !push(m->urgent_q, MAC_URGENT_QUEUE, &m->urgent_head, &m->urgent_count, s, false)) {
        free_slot(m, s);
        m->stats.queue_full++;
        return;
    }
    slot->queued = true;
    m->stats.beacons++;
}

static void serve_poll(mac_t *m, const mac_frame_t *f) {
    m->stats.data_requests++;
    int s = NO_SLOT;
    bool more = false;
    if (f->src_mode == MAC_ADDR_SHORT) {
        void *payload = 0;
        core_indirect_token_t t = core_indirect_poll(&m->indirect, f->src_short, &payload);
        if (t != CORE_INDIRECT_NONE) {
            s = (int)((mac_slot_t *)payload - m->slots);
            m->slots[s].token = t;
            more = core_indirect_count(&m->indirect, f->src_short) > 1;
        }
    } else if (f->src_mode == MAC_ADDR_EXT) {
        uint32_t oldest = 0;
        for (unsigned i = 0; i < MAC_EXT_INDIRECT; i++) {
            int c = m->ext_indirect[i];
            if (c == NO_SLOT || m->slots[c].dst_ext != f->src_ext || m->slots[c].in_flight) continue;
            uint32_t age = m->now_ms - m->slots[c].enqueued_ms;
            if (s == NO_SLOT || age > oldest) { s = c; oldest = age; }
        }
        more = s != NO_SLOT && ext_pending(m, f->src_ext) > 1;
    }
    if (s != NO_SLOT) {
        mac_slot_t *slot = &m->slots[s];
        set_pending_bit(slot, more);
        if (push(m->urgent_q, MAC_URGENT_QUEUE, &m->urgent_head, &m->urgent_count, s, false)) {
            slot->in_flight = true;   /* protected from expiry while queued */
            slot->queued = true;
            m->stats.polls_served++;
        } else {
            m->stats.queue_full++;
            m->stats.poll_queue_full++;
            keep_pending(m, s);
            s = NO_SLOT;
        }
    } else {
        m->stats.polls_empty++;
    }
    if (m->upper.poll_indication)
        m->upper.poll_indication(m->upper.ctx, f->src_mode == MAC_ADDR_SHORT ? f->src_short : 0xFFFE,
                                 f->src_mode == MAC_ADDR_EXT ? f->src_ext : 0, s != NO_SLOT);
}

void mac_rx(mac_t *m, const uint8_t *mpdu, uint8_t length, uint8_t lqi, int8_t rssi) {
    mac_frame_t f;
    if (mac_frame_parse(mpdu, length, &f) != MAC_OK) { m->stats.rx_malformed++; return; }
    m->stats.rx_frames++;
    if (!m->started || f.type == MAC_TYPE_ACK || f.type == MAC_TYPE_BEACON || f.security) {
        m->stats.rx_filtered++;
        return;
    }
    if (!addressed_to_us(m, &f)) { m->stats.rx_filtered++; return; }
    if (duplicate(m, &f)) { m->stats.rx_duplicates++; return; }
    if (f.type == MAC_TYPE_DATA) {
        if (m->upper.data_indication) m->upper.data_indication(m->upper.ctx, &f, lqi, rssi);
        return;
    }
    switch (f.payload[0]) {
    case MAC_CMD_BEACON_REQUEST:
        if (f.dst_mode == MAC_ADDR_SHORT && f.dst_short == MAC_BROADCAST_SHORT && f.src_mode == MAC_ADDR_NONE)
            send_beacon(m);
        break;
    case MAC_CMD_ASSOC_REQUEST:
        if (f.payload_len >= 2 && f.src_mode == MAC_ADDR_EXT && f.src_pan == MAC_BROADCAST_PAN) {
            m->stats.assoc_requests++;
            if (m->upper.assoc_indication) m->upper.assoc_indication(m->upper.ctx, f.src_ext, f.payload[1], lqi);
        }
        break;
    case MAC_CMD_DATA_REQUEST:
        if (f.payload_len == 1) serve_poll(m, &f);
        break;
    case MAC_CMD_ORPHAN_NOTIFY:
        if (f.src_mode == MAC_ADDR_EXT && m->upper.orphan_indication)
            m->upper.orphan_indication(m->upper.ctx, f.src_ext);
        break;
    default:
        break;
    }
    kick(m);
}

void mac_tick(mac_t *m, uint32_t now_ms) {
    m->now_ms = now_ms;
    for (unsigned i = 0; i < MAC_POOL_SLOTS; i++) {
        mac_slot_t *slot = &m->slots[i];
        if ((slot->kind == SLOT_INDIRECT_SHORT || slot->kind == SLOT_INDIRECT_EXT) && !slot->in_flight &&
            (uint32_t)(now_ms - slot->enqueued_ms) >= MAC_PERSISTENCE_MS) {
            m->stats.indirect_expired++;
            complete(m, (int)i, MAC_STATUS_TRANSACTION_EXPIRED);
        }
    }
    kick(m);
}

uint8_t mac_pending_count(mac_t *m, uint16_t short_addr) {
    return core_indirect_count(&m->indirect, short_addr);
}

void mac_source_match_resync(mac_t *m) {
    if (!m->radio.source_match) return;
    for (unsigned i = 0; i < sizeof m->slots / sizeof m->slots[0]; i++) {
        const mac_slot_t *slot = &m->slots[i];
        if (slot->kind == SLOT_INDIRECT_SHORT) m->radio.source_match(m->radio.ctx, false, slot->dst_short, true);
        else if (slot->kind == SLOT_INDIRECT_EXT) m->radio.source_match(m->radio.ctx, true, slot->dst_ext, true);
    }
}
