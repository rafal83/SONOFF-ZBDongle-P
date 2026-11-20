/* SPDX-License-Identifier: MIT */
#include "gp_proxy.h"

#include "aes128.h"
#include "ccm_star.h"

/* Zigbee Alliance TC link key "ZigBeeAlliance09": GPD keys in GP Pairing
 * are encrypted with it (A.1.5.3.3), as zigbee-herdsman does. */
static const uint8_t TC_KEY[16] = {0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C,
                                   0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39};

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8u * i)); }
static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

int gpdf_parse(const uint8_t *p, size_t len, gpdf_t *g) {
    if (!p || len < 1) return -1;
    size_t pos = 0;
    uint8_t fc = p[pos++];
    *g = (gpdf_t){0};
    g->frame_type = fc & 3u;
    if (((fc >> 2) & 0x0Fu) != 3u) return -1;              /* protocol version 3 = Green Power */
    g->auto_commissioning = (fc >> 6) & 1u;
    uint8_t app_id = 0;
    if (fc & 0x80u) {
        if (pos >= len) return -1;
        uint8_t ext = p[pos++];
        app_id = ext & 7u;
        g->sec_level = (ext >> 3) & 3u;
        g->key_type = (ext >> 5) & 1u;
        g->rx_after_tx = (ext >> 6) & 1u;
        g->direction = (ext >> 7) & 1u;
    }
    if (app_id != 0 || g->direction) return -1;              /* SrcID only, GPD -> proxy only */
    if (g->frame_type == 0 || (g->frame_type == 1 && (fc & 0x80u))) {
        if (pos + 4 > len) return -1;
        g->src_id = get32(p + pos);
        pos += 4;
    }
    if (g->frame_type == 1) {
        /* Maintenance frame: only the Channel Request (A.4.2.1.4) of a GPD
         * looking for its channel, without SrcID and without security. */
        if ((fc & 0x80u) || pos + 2 != len || p[pos] != 0xE3) return -1;
        g->rx_after_tx = true;                               /* it listens after a Channel Request */
        g->header_len = pos;
        g->cmd = p[pos++];
        g->payload = p + pos;
        g->payload_len = 1;
        return 0;
    }
    if (g->frame_type != 0 || g->src_id == 0 || g->src_id == 0xFFFFFFFFu) return -1;
    if (g->sec_level == 1) return -1;                        /* deprecated 1-byte MIC: refused */
    if (g->sec_level >= 2) {
        if (pos + 4 > len) return -1;
        g->counter = get32(p + pos);
        pos += 4;
    }
    g->header_len = pos;
    size_t mic_len = g->sec_level >= 2 ? 4u : 0u;
    if (pos + 1 + mic_len > len) return -1;
    g->cmd = p[pos++];
    g->payload = p + pos;
    g->payload_len = len - pos - mic_len;
    g->mic = mic_len ? p + len - mic_len : 0;
    return 0;
}

static void gp_nonce(uint32_t src_id, uint32_t counter, uint8_t nonce[13]) {
    put32(nonce, src_id);
    put32(nonce + 4, src_id);
    put32(nonce + 8, counter);
    nonce[12] = 0x05;
}

/* MIC check (and decryption for level 3) with the GPD key. `plain` receives
 * the command id followed by the payload. */
static bool gp_authenticate(const gp_entry_t *e, const uint8_t *frame, const gpdf_t *g, uint8_t *plain) {
    uint8_t nonce[13];
    gp_nonce(g->src_id, g->counter, nonce);
    aes128_ctx_t aes;
    aes128_init(&aes, e->key);
    bool ok;
    size_t body = 1 + g->payload_len;
    if (g->sec_level == 3) {
        ok = ccm_star_open(&aes, nonce, frame, g->header_len, frame + g->header_len, body, plain, g->mic, 4, true);
    } else {
        ok = ccm_star_open(&aes, nonce, frame, g->header_len + body, 0, 0, 0, g->mic, 4, false);
        if (ok) for (size_t i = 0; i < body; i++) plain[i] = frame[g->header_len + i];
    }
    aes128_clear(&aes);
    return ok;
}

static gp_entry_t *find(gp_proxy_t *gp, uint32_t src_id) {
    for (unsigned i = 0; i < GP_TABLE; i++) if (gp->table[i].used && gp->table[i].src_id == src_id) return &gp->table[i];
    return 0;
}

static void save(gp_proxy_t *gp, unsigned i) {
    if (!gp->store) return;
    const gp_entry_t *e = &gp->table[i];
    if (!e->used) { (void)nv_log_delete(gp->store, (uint16_t)(GP_STORE_ID0 + i)); return; }
    uint8_t b[4 + 4 + 16 + 3];
    put32(b, e->src_id);
    put32(b + 4, e->counter);
    for (unsigned k = 0; k < 16; k++) b[8 + k] = e->key[k];
    b[24] = e->sec_level;
    b[25] = e->key_type;
    b[26] = (uint8_t)((e->key_valid ? 1u : 0u) | (e->counter_valid ? 2u : 0u));
    (void)nv_log_write(gp->store, (uint16_t)(GP_STORE_ID0 + i), b, sizeof b);
    wipe(b, sizeof b);
}

/* ---- TempMaster (A.3.9.1): gpTxQueue and GPDF to the GPD -------------- */

/* GP Response (A.3.3.4.6) from the host: options, TempMaster short address,
 * TempMaster Tx channel, GPD id, GPD command id, payload (length-prefixed,
 * 0xFF = none). Queued only while commissioning and only if we are the
 * TempMaster (Z-Stack zclGp_GpResponseCommandCB does the same). */
static void gp_response(gp_proxy_t *gp, const uint8_t *p, size_t n, uint32_t now_ms) {
    if (n < 1 + 2 + 1 + 4 + 1 + 1 || (p[0] & 7u) != 0) { gp->stats.responses_ignored++; return; }
    uint16_t temp_master = (uint16_t)(p[1] | p[2] << 8);
    uint8_t channel = (uint8_t)((p[3] & 0x0Fu) + 11u);
    uint32_t src_id = get32(p + 4);
    uint8_t cmd = p[8], len = p[9];
    if (len == 0xFF) len = 0;
    bool maintenance = cmd == 0xF3;   /* Channel Configuration: answers a Channel Request (no SrcID) */
    if (!gp->commissioning || temp_master != gp->own_short || 10u + len > n || len > GP_TXQ_PAYLOAD ||
        (maintenance && src_id != 0) || (!maintenance && (src_id == 0 || src_id == 0xFFFFFFFFu))) {
        gp->stats.responses_ignored++;
        return;
    }
    gp->txq = (gp_txq_t){0};
    gp->txq.used = true;
    gp->txq.maintenance = maintenance;
    gp->txq.src_id = src_id;
    gp->txq.channel = channel;
    gp->txq.cmd = cmd;
    gp->txq.len = len;
    for (unsigned i = 0; i < len; i++) gp->txq.payload[i] = p[10 + i];
    gp->txq.expires_ms = now_ms + GP_TXQ_LIFETIME_MS;
    gp->want_channel = channel;
    gp->want_until = gp->txq.expires_ms;
    gp->stats.responses++;
}

/* The GPD opened its receive window: build the queued GPDF for it.
 * MAC: data, no ACK, no security, dst PAN 0xFFFF, dst 0xFFFF, no source
 * (application id 0). NWK: Commissioning Reply & co as a data frame with
 * the extended frame control (direction 1 = from the proxy) and the SrcID;
 * Channel Configuration as a maintenance frame. Unsecured (as herdsman
 * sends them: "encryption disabled for RX capable GPDs"). */
static void reply_if_queued(gp_proxy_t *gp, const gpdf_t *g, uint32_t now_ms) {
    gp_txq_t *q = &gp->txq;
    if (!q->used || !g->rx_after_tx) return;
    if ((int32_t)(now_ms - q->expires_ms) >= 0) return;            /* gp_proxy_tick drops it */
    if (q->maintenance ? g->cmd != 0xE3 : g->src_id != q->src_id) return;
    if (gp->cur_channel && gp->cur_channel != q->channel) return;   /* not where the GPD listens */
    uint8_t *m = gp->tx_mpdu;
    size_t n = 0;
    m[n++] = 0x01; m[n++] = 0x08;                                   /* data, dst short, no source */
    m[n++] = gp->mac_seq_tx++;
    m[n++] = 0xFF; m[n++] = 0xFF;                                   /* dst PAN */
    m[n++] = 0xFF; m[n++] = 0xFF;                                   /* dst address */
    if (q->maintenance) {
        m[n++] = 0x0D;                                              /* maintenance, protocol 3 */
    } else {
        m[n++] = 0x8C;                                              /* data, protocol 3, extended FC */
        m[n++] = 0x80;                                              /* app id 0, no security, direction 1 */
        put32(m + n, q->src_id); n += 4;
    }
    m[n++] = q->cmd;
    for (unsigned i = 0; i < q->len; i++) m[n++] = q->payload[i];
    gp->tx_len = (uint8_t)n;
    gp->tx_ready = true;
    q->used = false;
    gp->stats.replies++;
    if (gp->want_channel) gp->want_until = now_ms + GP_RETURN_MS;  /* time to send it, then back */
}

uint8_t gp_proxy_tick(gp_proxy_t *gp, uint32_t now_ms) {
    if (gp->txq.used && (int32_t)(now_ms - gp->txq.expires_ms) >= 0) {
        gp->txq.used = false;
        gp->stats.txq_expired++;
    }
    if (gp->want_channel && (int32_t)(now_ms - gp->want_until) >= 0) gp->want_channel = 0;
    return gp->want_channel;
}

void gp_proxy_init(gp_proxy_t *gp, nv_log_t *store) {
    *gp = (gp_proxy_t){0};
    gp->store = store;
    if (!store) return;
    for (unsigned i = 0; i < GP_TABLE; i++) {
        uint8_t b[NV_LOG_MAX_RECORD];
        uint16_t len = 0;
        if (!nv_log_read(store, (uint16_t)(GP_STORE_ID0 + i), b, sizeof b, &len) || len != 27) continue;
        gp_entry_t *e = &gp->table[i];
        e->used = true;
        e->src_id = get32(b);
        e->counter = get32(b + 4);
        for (unsigned k = 0; k < 16; k++) e->key[k] = b[8 + k];
        e->sec_level = b[24];
        e->key_type = b[25];
        e->key_valid = b[26] & 1u;
        e->counter_valid = (b[26] & 2u) != 0;
        wipe(b, sizeof b);
    }
}

static size_t zcl_header(gp_proxy_t *gp, uint8_t *zcl, uint8_t cmd) {
    zcl[0] = 0x11;   /* cluster specific, client -> server (proxy -> sink), no default response */
    zcl[1] = gp->zcl_seq++;
    zcl[2] = cmd;
    return 3;
}

size_t gp_proxy_rx(gp_proxy_t *gp, const uint8_t *nwk, size_t len, uint8_t mac_seq, uint32_t now_ms,
                   uint8_t *zcl, size_t cap) {
    gpdf_t g;
    gp->stats.gpdf_rx++;
    gp->tx_ready = false;
    if (gpdf_parse(nwk, len, &g) != 0) { gp->stats.malformed++; return 0; }
    /* On the TempMaster channel (A.3.9.1 step 9a): Channel Requests only. */
    if (gp->want_channel && gp->want_channel != gp->net_channel && gp->cur_channel == gp->want_channel &&
        g.cmd != 0xE3) {
        gp->stats.off_channel_dropped++;
        return 0;
    }
    reply_if_queued(gp, &g, now_ms);
    /* A Channel Request we just answered is not forwarded again: the sink
     * would answer it once more and keep the radio off the network channel. */
    if (gp->tx_ready && g.cmd == 0xE3) return 0;
    if (gp->commissioning && gp->commissioning_until && (int32_t)(now_ms - gp->commissioning_until) >= 0)
        gp->commissioning = false;
    gp_entry_t *e = find(gp, g.src_id);
    bool commissioning_cmd = g.cmd >= 0xE0 && g.cmd <= 0xE3;
    uint8_t plain[1 + 127];
    if (1 + g.payload_len > sizeof plain || 16 + g.payload_len + 8 > cap) { gp->stats.malformed++; return 0; }
    uint32_t counter = g.sec_level >= 2 ? g.counter : mac_seq;

    if (g.frame_type == 1 && !gp->commissioning) { gp->stats.malformed++; return 0; }
    if (commissioning_cmd && (gp->commissioning || g.cmd == 0xE1)) {
        /* GP Commissioning Notification (A.3.3.4.3). */
        bool failed = false;
        if (g.sec_level >= 2) {
            failed = !(e && e->key_valid && gp_authenticate(e, nwk, &g, plain));
        } else {
            plain[0] = g.cmd;
            for (size_t i = 0; i < g.payload_len; i++) plain[1 + i] = g.payload[i];
        }
        if (failed) {   /* forwarded as received, the sink decides (it may know the key) */
            plain[0] = g.cmd;
            for (size_t i = 0; i < g.payload_len; i++) plain[1 + i] = g.payload[i];
        }
        size_t n = zcl_header(gp, zcl, 0x04);
        uint16_t level = failed ? g.sec_level : (g.sec_level >= 2 ? 2u : 0u);
        uint16_t options = (uint16_t)((g.rx_after_tx ? 1u << 3 : 0u) | (level << 4) |
                                      ((uint16_t)(g.key_type ? 4u : 0u) << 6) | (failed ? 1u << 9 : 0u));
        put16(zcl + n, options); n += 2;
        put32(zcl + n, g.src_id); n += 4;
        put32(zcl + n, counter); n += 4;
        zcl[n++] = plain[0];
        zcl[n++] = (uint8_t)g.payload_len;
        for (size_t i = 0; i < g.payload_len; i++) zcl[n++] = plain[1 + i];
        if (failed) { for (unsigned i = 0; i < 4; i++) zcl[n++] = g.mic[i]; }
        wipe(plain, sizeof plain);
        gp->stats.commissioning_fwd++;
        return n;
    }
    if (!e) { gp->stats.not_paired++; return 0; }
    /* Operational frame of a paired GPD. */
    if (g.sec_level < e->sec_level) { gp->stats.auth_failed++; return 0; }   /* no downgrade */
    if (g.sec_level >= 2) {
        if (!e->key_valid || !gp_authenticate(e, nwk, &g, plain)) { gp->stats.auth_failed++; return 0; }
        if (e->counter_valid && g.counter <= e->counter) { gp->stats.replays++; wipe(plain, sizeof plain); return 0; }
        e->counter = g.counter;
        e->counter_valid = true;
        save(gp, (unsigned)(e - gp->table));
    } else {
        if (e->counter_valid && e->mac_seq == mac_seq && (uint32_t)(now_ms - e->seen_ms) < GP_DUP_MS) {
            gp->stats.duplicates++;
            return 0;
        }
        e->mac_seq = mac_seq;
        e->counter_valid = true;
        plain[0] = g.cmd;
        for (size_t i = 0; i < g.payload_len; i++) plain[1 + i] = g.payload[i];
    }
    e->seen_ms = now_ms;
    /* GP Notification (A.3.3.4.1); authenticated payload sent in clear
     * (security level 2 in the options: the sink must not decrypt it). */
    size_t n = zcl_header(gp, zcl, 0x00);
    uint16_t level = g.sec_level >= 2 ? 2u : 0u;
    uint16_t options = (uint16_t)((level << 6) | ((uint16_t)(g.sec_level >= 2 ? e->key_type : 0u) << 8) |
                                  (g.rx_after_tx ? 1u << 11 : 0u));
    put16(zcl + n, options); n += 2;
    put32(zcl + n, g.src_id); n += 4;
    put32(zcl + n, counter); n += 4;
    zcl[n++] = plain[0];
    zcl[n++] = (uint8_t)g.payload_len;
    for (size_t i = 0; i < g.payload_len; i++) zcl[n++] = plain[1 + i];
    wipe(plain, sizeof plain);
    gp->stats.forwarded++;
    return n;
}

void gp_proxy_host_frame(gp_proxy_t *gp, const uint8_t *zcl, size_t len, uint32_t now_ms) {
    if (len < 3 || (zcl[0] & 3u) != 1u) return;              /* cluster-specific only */
    size_t pos = (zcl[0] & 0x04u) ? 5u : 3u;                  /* manufacturer code */
    if (len < pos) return;
    uint8_t cmd = zcl[pos - 1];
    const uint8_t *p = zcl + pos;
    size_t n = len - pos;
    if (cmd == 0x02) {                                        /* GP Commissioning Mode */
        if (n < 1) return;
        bool enter = p[0] & 1u;
        uint16_t window = (p[0] & 2u) && n >= 3 ? (uint16_t)(p[1] | p[2] << 8) : 180u;
        gp->commissioning = enter;
        gp->commissioning_until = enter ? now_ms + (uint32_t)window * 1000u : 0;
        if (enter && !gp->commissioning_until) gp->commissioning_until = 1;
        return;
    }
    if (cmd == 0x06) { gp_response(gp, p, n, now_ms); return; }   /* GP Response */
    if (cmd != 0x01 || n < 7) return;                         /* GP Pairing */
    uint32_t options = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    if ((options & 7u) != 0) return;                          /* SrcID application only */
    size_t k = 3;
    uint32_t src_id = get32(p + k);
    k += 4;
    bool add = options & (1u << 3), remove = options & (1u << 4);
    uint32_t sink = (options >> 4) & 7u;
    if (sink == 0 || sink == 6) k += 8 + 2;                   /* sink IEEE + NWK */
    if (sink == 2 || sink == 4) k += 2;                       /* sink group */
    if (add) k += 1;                                          /* device id */
    bool has_counter = options & 0x4000u, has_key = options & 0x8000u;
    uint32_t counter = 0;
    if (has_counter) { if (k + 4 > n) return; counter = get32(p + k); k += 4; }
    uint8_t key[16];
    if (has_key) {
        if (k + 16 > n) return;
        /* Decrypt with the TC link key (CTR of AES-CCM, nonce SrcID x3 | 5). */
        uint8_t nonce[13];
        gp_nonce(src_id, src_id, nonce);
        aes128_ctx_t aes;
        aes128_init(&aes, TC_KEY);
        bool ok = ccm_star_open(&aes, nonce, 0, 0, p + k, 16, key, 0, 0, true);
        aes128_clear(&aes);
        if (!ok) return;
    }
    gp_entry_t *e = find(gp, src_id);
    if (remove || !add) {
        if (e) {
            e->used = false;
            wipe(e->key, sizeof e->key);
            save(gp, (unsigned)(e - gp->table));
            gp->stats.removals++;
        }
        wipe(key, sizeof key);
        return;
    }
    if (!e) {
        for (unsigned i = 0; i < GP_TABLE && !e; i++) if (!gp->table[i].used) e = &gp->table[i];
        if (!e) { wipe(key, sizeof key); return; }            /* table full: not proxied by us */
        *e = (gp_entry_t){0};
    }
    e->used = true;
    e->src_id = src_id;
    e->sec_level = (uint8_t)((options >> 9) & 3u);
    e->key_type = (uint8_t)((options >> 11) & 7u);
    if (has_key) { for (unsigned i = 0; i < 16; i++) e->key[i] = key[i]; e->key_valid = true; }
    if (has_counter) { e->counter = counter; e->counter_valid = true; }
    wipe(key, sizeof key);
    save(gp, (unsigned)(e - gp->table));
    gp->stats.pairings++;
}
