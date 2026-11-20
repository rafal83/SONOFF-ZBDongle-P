/* SPDX-License-Identifier: MIT
 * Green Power proxy of the coordinator (gp_proxy): GPD frames built here as
 * a GPD would send them, host GP Pairing / Commissioning Mode as
 * zigbee-herdsman sends them (GPD key encrypted with the TC link key). The
 * ZCL notifications produced are written to build/freertos/host/gp-frames.txt
 * and parsed again by the REAL herdsman ZCL parser (tools/check_gp_frames.cjs).
 */
#include "frt_test.h"
#include "aes128.h"
#include "ccm_star.h"
#include "gp_proxy.h"
#include "nv_log.h"

static FILE *out;
static void dump(const char *label, const uint8_t *z, size_t n) {
    if (!out) return;
    fprintf(out, "%s ", label);
    for (size_t i = 0; i < n; i++) fprintf(out, "%02x", z[i]);
    fprintf(out, "\n");
}

/* ---- RAM flash for the store ---- */
#define SECTOR 1024u
static uint8_t nor[4u * SECTOR];
static bool f_read(void *c, uint32_t a, void *b, size_t n) { (void)c; memcpy(b, nor + a, n); return true; }
static bool f_prog(void *c, uint32_t a, const void *d, size_t n) {
    (void)c;
    for (size_t i = 0; i < n; i++) nor[a + i] &= ((const uint8_t *)d)[i];
    return true;
}
static bool f_erase(void *c, uint32_t a) { (void)c; memset(nor + a, 0xFF, SECTOR); return true; }

static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8u * i)); }
static void nonce_of(uint32_t a, uint32_t b, uint8_t n[13]) { put32(n, a); put32(n + 4, a); put32(n + 8, b); n[12] = 5; }

/* GPDF: NWK FC (+ ext FC), SrcID, [counter], cmd, payload, [MIC]. */
static size_t gpdf(uint8_t *f, uint32_t src, uint8_t level, uint32_t counter, uint8_t cmd, const uint8_t *pl,
                   size_t pl_len, const uint8_t *key, bool rx_after_tx) {
    size_t n = 0;
    bool ext = level || rx_after_tx;
    f[n++] = (uint8_t)(0x0C | (ext ? 0x80 : 0));        /* data, protocol 3 */
    if (ext) f[n++] = (uint8_t)((level << 3) | (level ? 1u << 5 : 0) | (rx_after_tx ? 1u << 6 : 0));
    put32(f + n, src); n += 4;
    if (level >= 2) { put32(f + n, counter); n += 4; }
    size_t hl = n;
    f[n++] = cmd;
    if (pl_len) memcpy(f + n, pl, pl_len);
    n += pl_len;
    if (level >= 2) {
        uint8_t nonce[13], mic[4];
        nonce_of(src, counter, nonce);
        aes128_ctx_t a;
        aes128_init(&a, key);
        if (level == 3) CHECK(ccm_star_seal(&a, nonce, f, hl, f + hl, 1 + pl_len, f + hl, mic, 4, true));
        else CHECK(ccm_star_seal(&a, nonce, f, n, 0, 0, 0, mic, 4, false));
        aes128_clear(&a);
        memcpy(f + n, mic, 4); n += 4;
    }
    return n;
}

/* Host GP Pairing (cluster-specific, server -> client), as herdsman builds it. */
static size_t pairing(uint8_t *z, uint32_t src, bool add, uint8_t level, const uint8_t *key, bool counter_present,
                      uint32_t counter) {
    size_t n = 0;
    z[n++] = 0x19; z[n++] = 0x42; z[n++] = 0x01;
    uint32_t opt = (add ? 1u << 3 : 1u << 4) | (2u << 5) | ((uint32_t)level << 9) | (key ? 4u << 11 : 0) |
                   (counter_present ? 0x4000u : 0) | (key ? 0x8000u : 0);
    z[n++] = (uint8_t)opt; z[n++] = (uint8_t)(opt >> 8); z[n++] = (uint8_t)(opt >> 16);
    put32(z + n, src); n += 4;
    if (add) { z[n++] = 0x84; z[n++] = 0x0B; }          /* sink group 0x0B84 (mode 2) */
    if (add) z[n++] = 0x02;                              /* device id */
    if (counter_present) { put32(z + n, counter); n += 4; }
    if (key) {
        /* herdsman encryptSecurityKey: AES-CCM(TC key, nonce SrcID x3 | 5), first 16 bytes */
        static const uint8_t tc[16] = {0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C,
                                       0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39};
        uint8_t nonce[13], tag[16];
        nonce_of(src, src, nonce);
        aes128_ctx_t a;
        aes128_init(&a, tc);
        CHECK(ccm_star_seal(&a, nonce, 0, 0, key, 16, z + n, tag, 16, true));
        aes128_clear(&a);
        n += 16;
    }
    return n;
}

int main(void) {
    out = fopen("build/freertos/host/gp-frames.txt", "w");
    static const nv_flash_ops_t ops = {f_read, f_prog, f_erase, 0};
    static nv_log_t store;
    memset(nor, 0xFF, sizeof nor);
    CHECK(nv_log_mount(&store, &ops, 0, 2 * SECTOR, SECTOR));
    static gp_proxy_t gp;
    gp_proxy_init(&gp, &store);
    uint8_t f[128], z[160], h[64];
    uint32_t now = 1000;

    /* Malformed / foreign frames. */
    uint8_t bad[3] = {0x08, 0x00, 0x00};                 /* protocol version 2 */
    CHECK(gp_proxy_rx(&gp, bad, sizeof bad, 1, now, z, sizeof z) == 0 && gp.stats.malformed == 1);
    CHECK(gp_proxy_rx(&gp, f, gpdf(f, 0x01020304, 0, 0, 0x22, 0, 0, 0, false) - 2, 1, now, z, sizeof z) == 0);

    /* 1. Commissioning (Hue Tap like, unsecured): only while commissioning. */
    const uint32_t TAP = 0x0150A0B1;
    const uint8_t comm[] = {0x02, 0x00};   /* device id (on/off switch), options (no extension) */
    size_t fl = gpdf(f, TAP, 0, 0, 0xE0, comm, sizeof comm, 0, false);
    CHECK(gp_proxy_rx(&gp, f, fl, 7, now, z, sizeof z) == 0 && gp.stats.not_paired == 1 && gp.stats.malformed == 2);
    const uint8_t mode[] = {0x19, 0x10, 0x02, 0x0B, 0xB4, 0x00};   /* enter, window 180 s */
    gp_proxy_host_frame(&gp, mode, sizeof mode, now);
    size_t zl = gp_proxy_rx(&gp, f, fl, 8, now, z, sizeof z);
    CHECK(zl && z[2] == 0x04 && z[5] == (uint8_t)TAP && z[13] == 0xE0 && z[14] == sizeof comm);
    dump("commissioning", z, zl);
    gp_proxy_host_frame(&gp, mode, sizeof mode, now);
    /* Window expiry. */
    CHECK(gp_proxy_rx(&gp, f, fl, 9, now + 181000u, z, sizeof z) == 0);

    /* 2. Operational, unsecured GPD paired without security. */
    const uint8_t none[1] = {0};
    fl = gpdf(f, TAP, 0, 0, 0x22, none, 0, 0, false);
    CHECK(gp_proxy_rx(&gp, f, fl, 10, now, z, sizeof z) == 0);          /* not paired yet */
    size_t hl = pairing(h, TAP, true, 0, 0, false, 0);
    gp_proxy_host_frame(&gp, h, hl, now);
    CHECK(gp.stats.pairings == 1);
    zl = gp_proxy_rx(&gp, f, fl, 11, now, z, sizeof z);
    CHECK(zl && z[2] == 0x00 && z[13] == 0x22 && z[14] == 0);
    dump("notification", z, zl);
    CHECK(gp_proxy_rx(&gp, f, fl, 11, now + 100, z, sizeof z) == 0 && gp.stats.duplicates == 1);   /* repeat */
    CHECK(gp_proxy_rx(&gp, f, fl, 12, now + 200, z, sizeof z) != 0);

    /* 3. Secured GPD (full encryption, key from the pairing). */
    const uint32_t SW = 0x0175AB12;
    uint8_t key[16];
    for (unsigned i = 0; i < 16; i++) key[i] = (uint8_t)(0x30 + 7 * i);
    hl = pairing(h, SW, true, 2, key, true, 100);
    gp_proxy_host_frame(&gp, h, hl, now);
    const uint8_t press[2] = {0x10, 0x01};
    fl = gpdf(f, SW, 3, 101, 0x69, press, sizeof press, key, false);
    zl = gp_proxy_rx(&gp, f, fl, 1, now, z, sizeof z);
    CHECK(zl && z[2] == 0x00 && z[13] == 0x69 && z[14] == 2 && z[15] == 0x10 && z[16] == 0x01);
    CHECK(((z[3] >> 6) & 3) == 2);                        /* authenticated, already in clear */
    dump("notification_secured", z, zl);
    CHECK(gp_proxy_rx(&gp, f, fl, 2, now, z, sizeof z) == 0 && gp.stats.replays == 1);
    fl = gpdf(f, SW, 3, 102, 0x69, press, sizeof press, key, false);
    f[fl - 1] ^= 1;                                        /* forged MIC */
    CHECK(gp_proxy_rx(&gp, f, fl, 3, now, z, sizeof z) == 0 && gp.stats.auth_failed == 1);
    fl = gpdf(f, SW, 0, 0, 0x69, press, sizeof press, 0, false);   /* downgrade to no security */
    CHECK(gp_proxy_rx(&gp, f, fl, 4, now, z, sizeof z) == 0 && gp.stats.auth_failed == 2);
    uint8_t wrong[16];
    memcpy(wrong, key, 16); wrong[0] ^= 0x55;
    fl = gpdf(f, SW, 3, 103, 0x69, press, sizeof press, wrong, false);   /* another key */
    CHECK(gp_proxy_rx(&gp, f, fl, 5, now, z, sizeof z) == 0 && gp.stats.auth_failed == 3);
    fl = gpdf(f, SW, 2, 104, 0x22, none, 0, key, false);              /* level 2: authenticated only */
    CHECK(gp_proxy_rx(&gp, f, fl, 6, now, z, sizeof z) != 0);

    /* 4. Reboot: paired GPDs, keys and counters back from the store. */
    gp_proxy_init(&gp, &store);
    fl = gpdf(f, SW, 3, 104, 0x69, press, sizeof press, key, false);
    CHECK(gp_proxy_rx(&gp, f, fl, 7, now, z, sizeof z) == 0 && gp.stats.replays == 1);   /* counter kept */
    fl = gpdf(f, SW, 3, 105, 0x69, press, sizeof press, key, false);
    CHECK(gp_proxy_rx(&gp, f, fl, 8, now, z, sizeof z) != 0);
    fl = gpdf(f, TAP, 0, 0, 0x22, none, 0, 0, false);
    CHECK(gp_proxy_rx(&gp, f, fl, 30, now, z, sizeof z) != 0);

    /* 5. Removal (GP Pairing with removeGpd): no longer forwarded. */
    hl = pairing(h, SW, false, 0, 0, false, 0);
    gp_proxy_host_frame(&gp, h, hl, now);
    fl = gpdf(f, SW, 3, 106, 0x69, press, sizeof press, key, false);
    CHECK(gp_proxy_rx(&gp, f, fl, 9, now, z, sizeof z) == 0 && gp.stats.removals == 1);
    gp_proxy_init(&gp, &store);
    CHECK(gp_proxy_rx(&gp, f, fl, 10, now, z, sizeof z) == 0);          /* removal persisted */

    /* 6. TempMaster for a bidirectional GPD (A.3.9.1). */
    {
        gp_proxy_init(&gp, &store);
        gp.own_short = 0x0000;
        gp.cur_channel = gp.net_channel = 15;
        const uint32_t BIDIR = 0x01AABBCC;
        /* GP Response as herdsman sends it for an RxOnCapable GPD: options 0,
         * TempMaster 0x0000 (we forwarded the notification), TempMasterTx =
         * channel - 11, Commissioning Reply (0xF0) with options 0. */
        const uint8_t rsp_f0[] = {0x19, 0x21, 0x06, 0x00, 0x00, 0x00, 0x04, 0xCC, 0xBB, 0xAA, 0x01, 0xF0, 0x01, 0x00};
        dump("response_f0", rsp_f0, sizeof rsp_f0);
        gp_proxy_host_frame(&gp, rsp_f0, sizeof rsp_f0, now);                /* not commissioning: ignored */
        CHECK(!gp.txq.used && gp.stats.responses_ignored == 1);
        gp_proxy_host_frame(&gp, mode, sizeof mode, now);                    /* enter commissioning */
        uint8_t other[sizeof rsp_f0];
        memcpy(other, rsp_f0, sizeof other);
        other[4] = 0x34; other[5] = 0x12;                                    /* TempMaster = another proxy */
        gp_proxy_host_frame(&gp, other, sizeof other, now);
        CHECK(!gp.txq.used && gp.stats.responses_ignored == 2);
        gp_proxy_host_frame(&gp, rsp_f0, sizeof rsp_f0, now);
        CHECK(gp.txq.used && gp.txq.channel == 15 && gp.stats.responses == 1);
        CHECK(gp_proxy_tick(&gp, now) == 15);                                /* = network channel: no move */
        /* A frame without RxAfterTx: the GPD is not listening, nothing sent. */
        const uint8_t comm_rx[] = {0x02, 0x02};                              /* options: RxOnCapability */
        fl = gpdf(f, BIDIR, 0, 0, 0xE0, comm_rx, sizeof comm_rx, 0, false);
        CHECK(gp_proxy_rx(&gp, f, fl, 40, now, z, sizeof z) != 0 && !gp.tx_ready && gp.txq.used);
        /* Its next commissioning frame with RxAfterTx: reply built. */
        fl = gpdf(f, BIDIR, 0, 0, 0xE0, comm_rx, sizeof comm_rx, 0, true);
        zl = gp_proxy_rx(&gp, f, fl, 41, now + 10, z, sizeof z);
        CHECK(zl && z[2] == 0x04 && ((z[3] >> 3) & 1u));                    /* notification still forwarded */
        static const uint8_t want_f0[] = {0x01, 0x08, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x8C, 0x80,
                                          0xCC, 0xBB, 0xAA, 0x01, 0xF0, 0x00};
        CHECK(gp.tx_ready && gp.tx_len == sizeof want_f0 && !memcmp(gp.tx_mpdu, want_f0, sizeof want_f0));
        dump("reply_f0", gp.tx_mpdu, gp.tx_len);
        CHECK(!gp.txq.used && gp.stats.replies == 1);
        gp.tx_ready = false;
        fl = gpdf(f, BIDIR, 0, 0, 0xE0, comm_rx, sizeof comm_rx, 0, true);
        CHECK(gp_proxy_rx(&gp, f, fl, 42, now + 20, z, sizeof z) != 0 && !gp.tx_ready);   /* sent once */
        CHECK(gp_proxy_tick(&gp, now + 10 + GP_RETURN_MS) == 0);

        /* Expiry of a queued reply (gpTxQueueEntryLifetime). */
        gp_proxy_host_frame(&gp, rsp_f0, sizeof rsp_f0, now);
        CHECK(gp.txq.used && gp_proxy_tick(&gp, now + GP_TXQ_LIFETIME_MS - 1) == 15);
        CHECK(gp_proxy_tick(&gp, now + GP_TXQ_LIFETIME_MS) == 0 && !gp.txq.used && gp.stats.txq_expired == 1);
        fl = gpdf(f, BIDIR, 0, 0, 0xE0, comm_rx, sizeof comm_rx, 0, true);
        CHECK(gp_proxy_rx(&gp, f, fl, 43, now + GP_TXQ_LIFETIME_MS, z, sizeof z) != 0 && !gp.tx_ready);

        /* Channel Request (maintenance frame, no SrcID): next attempt on
         * channel 16 (5), then 15 (4). Forwarded with SrcID 0 and RxAfterTx. */
        const uint8_t chreq[] = {0x0D, 0xE3, 0x45};
        dump("channel_request_gpdf", chreq, sizeof chreq);
        zl = gp_proxy_rx(&gp, chreq, sizeof chreq, 44, now, z, sizeof z);
        CHECK(zl && z[2] == 0x04 && z[5] == 0 && z[6] == 0 && z[7] == 0 && z[8] == 0 && z[13] == 0xE3 &&
              z[14] == 1 && z[15] == 0x45 && ((z[3] >> 3) & 1u));
        dump("commissioning_channel_request", z, zl);
        /* herdsman: Channel Configuration (0xF3) to send on the requested
         * channel (TempMasterTx = nextChannel), operational channel 15 (4), basic. */
        const uint8_t rsp_f3[] = {0x19, 0x22, 0x06, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x01, 0x14};
        dump("response_f3", rsp_f3, sizeof rsp_f3);
        gp_proxy_host_frame(&gp, rsp_f3, sizeof rsp_f3, now);
        CHECK(gp.txq.used && gp.txq.maintenance && gp_proxy_tick(&gp, now) == 16);   /* radio must move */
        /* Still on 15 (the platform did not move yet): not where it listens. */
        CHECK(gp_proxy_rx(&gp, chreq, sizeof chreq, 45, now, z, sizeof z) != 0 && !gp.tx_ready);
        gp.cur_channel = 16;                                                  /* radio moved */
        fl = gpdf(f, TAP, 0, 0, 0x22, none, 0, 0, false);                     /* other frames: ignored there */
        CHECK(gp_proxy_rx(&gp, f, fl, 46, now + 50, z, sizeof z) == 0 && gp.stats.off_channel_dropped == 1);
        zl = gp_proxy_rx(&gp, chreq, sizeof chreq, 47, now + 100, z, sizeof z);
        CHECK(zl == 0);                                                       /* answered: not forwarded again */
        static const uint8_t want_f3[] = {0x01, 0x08, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x0D, 0xF3, 0x14};
        CHECK(gp.tx_ready && gp.tx_len == sizeof want_f3 && !memcmp(gp.tx_mpdu, want_f3, sizeof want_f3));
        dump("reply_f3", gp.tx_mpdu, gp.tx_len);
        gp.tx_ready = false;
        CHECK(gp_proxy_tick(&gp, now + 100 + GP_RETURN_MS - 1) == 16);        /* time to send it */
        CHECK(gp_proxy_tick(&gp, now + 100 + GP_RETURN_MS) == 0);             /* back to the network */
        gp.cur_channel = 15;

        /* Refused responses: SrcID 0 for a Commissioning Reply, a SrcID for a
         * Channel Configuration, truncated payload. */
        uint8_t bad1[sizeof rsp_f0];
        memcpy(bad1, rsp_f0, sizeof bad1);
        memset(bad1 + 7, 0, 4);
        uint8_t bad2[sizeof rsp_f3];
        memcpy(bad2, rsp_f3, sizeof bad2);
        bad2[7] = 1;
        uint32_t ign = gp.stats.responses_ignored;
        gp_proxy_host_frame(&gp, bad1, sizeof bad1, now);
        gp_proxy_host_frame(&gp, bad2, sizeof bad2, now);
        gp_proxy_host_frame(&gp, rsp_f0, sizeof rsp_f0 - 1, now);
        CHECK(!gp.txq.used && gp.stats.responses_ignored == ign + 3);
        /* Maintenance frames outside commissioning, or other than a Channel Request. */
        const uint8_t leave[] = {0x19, 0x23, 0x02, 0x00};                   /* exit commissioning */
        gp_proxy_host_frame(&gp, leave, sizeof leave, now);
        CHECK(gp_proxy_rx(&gp, chreq, sizeof chreq, 48, now, z, sizeof z) == 0);
        const uint8_t maint_other[] = {0x0D, 0xE0, 0x00};
        gpdf_t g;
        CHECK(gpdf_parse(maint_other, sizeof maint_other, &g) != 0);
    }

    if (out) fclose(out);
    PASS("gp");
    return 0;
}
