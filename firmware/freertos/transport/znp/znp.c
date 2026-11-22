/* SPDX-License-Identifier: MIT
 * ZNP MT interface over the FreeRTOS coordinator. See znp.h.
 * Command layouts: zigbee-herdsman 11.2.1 src/adapter/z-stack/znp/definition.ts
 * (the host this firmware is validated against) and the TI Z-Stack MT API.
 */
#include "znp.h"
#include "znp_structs.h"
#include "zb_mmo.h"

/* MT frame types / subsystems */
#define T_SREQ 0x20u
#define T_AREQ 0x40u
#define T_SRSP 0x60u
#define S_RPC 0x00u
#define S_SYS 0x01u
#define S_AF 0x04u
#define S_ZDO 0x05u
#define S_UTIL 0x07u
#define S_APP_CNF 0x0Fu

/* Z-Stack status codes */
#define ST_SUCCESS 0x00u
#define ST_FAILURE 0x01u
#define ST_INVALID_PARAM 0x02u
#define ST_NV_ITEM_INITIALIZED 0x09u
#define ST_NV_OPER_FAILED 0x0Au
#define ST_NV_BAD_ITEM_LEN 0x0Cu
#define ST_NWK_NO_ROUTE 0xCDu
#define ST_APS_NOT_SUPPORTED 0xB6u

/* MT capabilities (SYS_PING) */
#define CAP_SYS 0x0001u
#define CAP_AF 0x0008u
#define CAP_ZDO 0x0010u
#define CAP_UTIL 0x0040u
#define CAP_APP_CNF 0x0800u

/* NV items (Z-Stack ids) */
#define NV_EXTADDR 0x0001u
#define NV_STARTUP_OPTION 0x0003u
#define NV_NIB 0x0021u
#define NV_EXTENDED_PAN_ID 0x002Du
#define NV_ACTIVE_KEY_INFO 0x003Au
#define NV_ALTERN_KEY_INFO 0x003Bu
#define NV_APS_LINK_KEY_TABLE 0x004Cu
#define NV_PRECFGKEY 0x0062u
#define NV_NWKKEY 0x0082u
#define NV_PANID 0x0083u
#define NV_CHANLIST 0x0084u
#define NV_TCLK_SEED 0x0101u
#define STARTOPT_CLEAR_CONFIG 0x01u
#define STARTOPT_CLEAR_STATE 0x02u

/* Extended tables (sysid 1) */
#define SYSID_ZSTACK 1u
#define EX_ADDRMGR 1u
#define EX_TCLK 4u
#define EX_APS_KEY_DATA 6u
#define EX_NWK_SEC_MATERIAL 7u

#define KEYDESC_LEN 17u      /* nwkKeyDesc: sequence + key (no padding in Z-Stack) */
#define NWKKEY_LEN 24u       /* nwkActiveKeyItems, aligned: desc + pad + frame counter */
#define SM_LEN (2u + ZNP_SM_CAPACITY * ZNP_SME_SIZE)
#define AF_DEFAULT_RADIUS 30u
#define AF_ACK_REQUEST 0x10u
#define AF_EN_SECURITY 0x40u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8u * i)); }
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8u * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void zero(void *d, size_t n) { volatile uint8_t *p = d; while (n--) *p++ = 0; }
static void copy(void *d, const void *s, size_t n) {
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--) *a++ = *b++;
}
static bool all(const uint8_t *p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; i++) if (p[i] != v) return false;
    return true;
}

static void send(znp_t *z, uint8_t cmd0, uint8_t cmd1, const uint8_t *p, size_t len) {
    if (len > 250u) return;
    z->p.send(z->p.ctx, cmd0, cmd1, p, (uint8_t)len);
}
static void srsp(znp_t *z, uint8_t sub, uint8_t cmd, const uint8_t *p, size_t len) { send(z, T_SRSP | sub, cmd, p, len); }
static void srsp_status(znp_t *z, uint8_t sub, uint8_t cmd, uint8_t status) { srsp(z, sub, cmd, &status, 1); }
static void areq(znp_t *z, uint8_t sub, uint8_t cmd, const uint8_t *p, size_t len) { send(z, T_AREQ | sub, cmd, p, len); }

static void rpc_error(znp_t *z, uint8_t error, uint8_t cmd0, uint8_t cmd1) {
    const uint8_t p[3] = {error, cmd0, cmd1};
    z->stats.frames_unsupported++;
    srsp(z, S_RPC, 0x00, p, 3);
}

/* ---- generic NV items (stored as they are) -------------------------------- */
static bool generic_id(uint16_t item) { return item < 256u; }
static uint16_t generic_len(znp_t *z, uint16_t item, uint8_t *buf) {
    uint16_t len = 0;
    if (!z->store || !generic_id(item) || !nv_log_read(z->store, (uint16_t)(ZNP_NV_GENERIC_BASE + item), buf, NV_LOG_MAX_RECORD, &len))
        return 0;
    return len;
}
static bool generic_put(znp_t *z, uint16_t item, const uint8_t *buf, uint16_t len) {
    bool ok = z->store && nv_log_write(z->store, (uint16_t)(ZNP_NV_GENERIC_BASE + item), buf, len);
    if (ok) z->stats.nv_writes++;
    else z->stats.nv_failures++;
    return ok;
}

/* ---- synthesized items --------------------------------------------------- */
static bool synthesized(uint16_t item) {
    return item == NV_EXTADDR || item == NV_NIB || item == NV_ACTIVE_KEY_INFO || item == NV_ALTERN_KEY_INFO ||
           item == NV_NWKKEY || item == NV_TCLK_SEED || item == NV_APS_LINK_KEY_TABLE;
}

static const nwk_device_t *dev_at(znp_t *z, uint16_t slot) {
    const nwk_device_t *d = &z->coord->devices.dev[slot];
    return d->ext ? d : 0;
}

/* Slot of the index-th device holding an explicit (non seed-derived) key. */
static uint16_t explicit_key_slot(znp_t *z, unsigned index) {
    unsigned k = 0;
    for (uint16_t s = 0; s < NWK_MAX_DEVICES; s++) {
        const nwk_device_t *d = dev_at(z, s);
        if (!d || !d->unique_key || d->tclk_shift != 0xFFu) continue;
        if (k++ == index) return s;
    }
    return NWK_DEVICE_NONE;
}

/* Build a synthesized item; returns its length (0 = item does not exist). */
static uint16_t synth_item(znp_t *z, uint16_t item, uint8_t *b) {
    zb_coord_t *c = z->coord;
    switch (item) {
    case NV_EXTADDR: {
        uint8_t tmp[NV_LOG_MAX_RECORD];
        if (generic_len(z, NV_EXTADDR, tmp) == 8) { copy(b, tmp, 8); return 8; }   /* pending override */
        put64(b, z->ieee);
        return 8;
    }
    case NV_NIB:
        if (!z->formed) return 0;
        zero(b, ZNP_NIB_SIZE);
        b[ZNP_NIB_SEQUENCENUM] = c->nwk_seq;
        b[ZNP_NIB_PASSIVEACKTIMEOUT] = 5;
        b[ZNP_NIB_MAXBROADCASTRETRIES] = 2;
        b[ZNP_NIB_MAXCHILDREN] = 0;
        b[ZNP_NIB_MAXDEPTH] = 15;
        b[ZNP_NIB_MAXROUTERS] = 0;
        b[ZNP_NIB_BROADCASTDELIVERYTIME] = 30;
        b[ZNP_NIB_REPORTCONSTANTCOST] = 0;
        b[ZNP_NIB_ROUTEDISCRETRIES] = 0;
        b[ZNP_NIB_SECUREALLFRAMES] = 1;
        b[ZNP_NIB_SECURITYLEVEL] = 5;
        b[ZNP_NIB_SYMLINK] = 1;
        b[ZNP_NIB_CAPABILITYFLAGS] = 0x8F;
        put16(b + ZNP_NIB_TRANSACTIONPERSISTENCETIME, 7);
        b[ZNP_NIB_NWKPROTOCOLVERSION] = 2;
        b[ZNP_NIB_ROUTEDISCOVERYTIME] = 13;
        b[ZNP_NIB_ROUTEEXPIRYTIME] = 30;
        put16(b + ZNP_NIB_NWKDEVADDRESS, 0x0000);
        b[ZNP_NIB_NWKLOGICALCHANNEL] = c->mac.channel;
        put16(b + ZNP_NIB_NWKCOORDADDRESS, 0x0000);
        put64(b + ZNP_NIB_NWKCOORDEXTADDRESS, 0);
        put16(b + ZNP_NIB_NWKPANID, c->mac.pan_id);
        b[ZNP_NIB_NWKSTATE] = 1;    /* NWK_ROUTER (Z-Stack value for a running coordinator) */
        put32(b + ZNP_NIB_CHANNELLIST, z->primary_channels ? z->primary_channels : (1u << c->mac.channel));
        b[ZNP_NIB_BEACONORDER] = 15;
        b[ZNP_NIB_SUPERFRAMEORDER] = 15;
        b[ZNP_NIB_SCANDURATION] = 0;
        b[ZNP_NIB_NODEDEPTH] = 0;
        copy(b + ZNP_NIB_EXTENDEDPANID, c->mac.beacon.ext_pan_id, 8);   /* little endian, as in Z-Stack */
        b[ZNP_NIB_NWKKEYLOADED] = 1;
        b[ZNP_NIB_NWKLINKSTATUSPERIOD] = 15;
        b[ZNP_NIB_NWKROUTERAGELIMIT] = 3;
        b[ZNP_NIB_NWKUSEMULTICAST] = 0;
        b[ZNP_NIB_NWKISCONCENTRATOR] = 1;
        b[ZNP_NIB_NWKCONCENTRATORDISCOVERYTIME] = 120;
        b[ZNP_NIB_NWKCONCENTRATORRADIUS] = 10;
        b[ZNP_NIB_NWKALLFRESH] = 1;
        put16(b + ZNP_NIB_NWKMANAGERADDR, 0x0000);
        put16(b + ZNP_NIB_NWKTOTALTRANSMISSIONS, 0);
        b[ZNP_NIB_NWKUPDATEID] = c->nwk_update_id;
        return ZNP_NIB_SIZE;
    case NV_ACTIVE_KEY_INFO:
    case NV_ALTERN_KEY_INFO:
        zero(b, KEYDESC_LEN);
        if (z->formed) { b[0] = c->key_seq; copy(b + 1, c->network_key, 16); }
        return KEYDESC_LEN;
    case NV_NWKKEY:
        zero(b, NWKKEY_LEN);
        if (z->formed) {
            b[0] = c->key_seq;
            copy(b + 1, c->network_key, 16);
            put32(b + 20, zb_coord_nwk_frame_counter(c));
        }
        return NWKKEY_LEN;
    case NV_TCLK_SEED:
        if (!c->tclk_seed_valid) return 0;
        copy(b, c->tclk_seed, 16);
        return 16;
    case NV_APS_LINK_KEY_TABLE: {
        zero(b, SM_LEN);
        uint16_t used = 0;
        for (unsigned i = 0; i < ZNP_SM_CAPACITY; i++) {
            uint8_t *e = b + 2 + i * ZNP_SME_SIZE;
            uint16_t slot = explicit_key_slot(z, i);
            if (slot == NWK_DEVICE_NONE) {
                put16(e + ZNP_SME_AMI, 0xFFFE);   /* herdsman default entry: fe ff 00 00 00 */
                continue;
            }
            put16(e + ZNP_SME_AMI, slot);
            put16(e + ZNP_SME_KEYNVID, (uint16_t)i);
            e[ZNP_SME_AUTHENTICATIONOPTION] = 0x01;   /* authenticated (CBKE/verified key) */
            used++;
        }
        put16(b, used);
        return SM_LEN;
    }
    default:
        return 0;
    }
}

static uint16_t item_read(znp_t *z, uint16_t item, uint8_t *b) {
    return synthesized(item) ? synth_item(z, item, b) : generic_len(z, item, b);
}

/* Apply a complete synthesized item written by the host. */
static uint8_t synth_write(znp_t *z, uint16_t item, const uint8_t *b, uint16_t len) {
    zb_coord_t *c = z->coord;
    switch (item) {
    case NV_EXTADDR:
        /* Coordinator IEEE override (restore of a backup): used from the next boot. */
        if (len != 8 || all(b, 8, 0x00) || all(b, 8, 0xFF)) return ST_NV_BAD_ITEM_LEN;
        return generic_put(z, NV_EXTADDR, b, 8) ? ST_SUCCESS : ST_NV_OPER_FAILED;
    case NV_NIB: {
        if (len != ZNP_NIB_SIZE || !z->formed) return ST_NV_OPER_FAILED;
        uint8_t channel = b[ZNP_NIB_NWKLOGICALCHANNEL];
        uint16_t pan = get16(b + ZNP_NIB_NWKPANID);
        if (channel < 11 || channel > 26 || pan == 0xFFFF) return ST_INVALID_PARAM;
        z->primary_channels = get32(b + ZNP_NIB_CHANNELLIST);
        zb_coord_set_network(c, pan, channel, b + ZNP_NIB_EXTENDEDPANID, b[ZNP_NIB_NWKUPDATEID]);
        return ST_SUCCESS;
    }
    case NV_ACTIVE_KEY_INFO:
        if ((len != KEYDESC_LEN && len != ZNP_KEYDESC_SIZE) || !z->formed) return ST_NV_OPER_FAILED;
        zb_coord_set_network_key(c, b + ZNP_KEYDESC_KEY, b[ZNP_KEYDESC_KEYSEQNUM]);
        return ST_SUCCESS;
    case NV_ALTERN_KEY_INFO:
        /* No alternate key support: only the active key itself is accepted. */
        if ((len != KEYDESC_LEN && len != ZNP_KEYDESC_SIZE) || !z->formed) return ST_NV_OPER_FAILED;
        for (unsigned i = 0; i < 16; i++) if (b[1 + i] != c->network_key[i]) return ST_NV_OPER_FAILED;
        return ST_SUCCESS;
    case NV_TCLK_SEED:
        if (len != 16) return ST_NV_BAD_ITEM_LEN;
        zb_coord_set_tclk_seed(c, b);
        return ST_SUCCESS;
    case NV_APS_LINK_KEY_TABLE:
        if (len != SM_LEN) return ST_NV_BAD_ITEM_LEN;
        copy(z->sm, b, SM_LEN);
        z->sm_valid = true;
        return ST_SUCCESS;
    default:
        return ST_NV_OPER_FAILED;   /* NWKKEY and the like are read-only */
    }
}

static uint16_t item_length(znp_t *z, uint16_t item) {
    uint8_t b[NV_LOG_MAX_RECORD];
    return item_read(z, item, b);
}

/* Write len bytes at offset of an item: read-modify-write of the whole item. */
static uint8_t item_write(znp_t *z, uint16_t item, uint16_t offset, const uint8_t *data, uint16_t len) {
    uint8_t b[NV_LOG_MAX_RECORD];
    uint16_t total = item_read(z, item, b);
    if (item == NV_EXTADDR && offset == 0 && len == 8) total = 8;
    if (!total) return ST_NV_OPER_FAILED;               /* must be created first */
    if ((uint32_t)offset + len > total) return ST_NV_BAD_ITEM_LEN;
    copy(b + offset, data, len);
    if (synthesized(item)) return synth_write(z, item, b, total);
    return generic_put(z, item, b, total) ? ST_SUCCESS : ST_NV_OPER_FAILED;
}

/* ---- extended tables ----------------------------------------------------- */
static uint16_t ex_entry_size(uint16_t table) {
    switch (table) {
    case EX_ADDRMGR: return ZNP_AME_SIZE;
    case EX_TCLK: return ZNP_TCLK_SIZE;
    case EX_APS_KEY_DATA: return ZNP_APSKEY_SIZE;
    case EX_NWK_SEC_MATERIAL: return ZNP_SECMAT_SIZE;
    default: return 0;
    }
}
static uint16_t ex_capacity(uint16_t table) {
    switch (table) {
    case EX_ADDRMGR:
    case EX_TCLK: return NWK_MAX_DEVICES;
    case EX_APS_KEY_DATA: return ZNP_APSKEY_CAPACITY;
    case EX_NWK_SEC_MATERIAL: return ZNP_SECMAT_CAPACITY;
    default: return 0;
    }
}
static uint16_t ex_length(uint8_t sysid, uint16_t table, uint16_t sub) {
    if (sysid != SYSID_ZSTACK || sub >= ex_capacity(table)) return 0;
    return ex_entry_size(table);
}


/* Outgoing counter given by the host (restore): never below what it says,
 * plus ZNP_RESTORE_COUNTER_MARGIN when the store had no counters floor. */
static uint32_t host_counter(znp_t *z, uint32_t v) {
    if (!v || z->coord->counter_floor_from_store) return v;
    z->stats.restore_margins++;
    return v > UINT32_MAX - ZNP_RESTORE_COUNTER_MARGIN ? UINT32_MAX - 1u : v + ZNP_RESTORE_COUNTER_MARGIN;
}

static void ex_read(znp_t *z, uint16_t table, uint16_t sub, uint8_t *e) {
    zb_coord_t *c = z->coord;
    uint16_t size = ex_entry_size(table);
    zero(e, size);
    switch (table) {
    case EX_ADDRMGR: {
        const nwk_device_t *d = dev_at(z, sub);
        if (!d) {
            for (unsigned i = 0; i < size; i++) e[i] = 0xFF;   /* Z-Stack empty entry */
            e[ZNP_AME_USER] = 0;
            break;
        }
        e[ZNP_AME_USER] = (uint8_t)((d->relationship == NWK_REL_CHILD ? 0x01u : 0u) | 0x02u /* security */);
        e[1] = 0xFF;   /* padding byte of the aligned layout */
        put16(e + ZNP_AME_NWKADDR, d->short_addr);
        put64(e + ZNP_AME_EXTADDR, d->ext);
        break;
    }
    case EX_TCLK: {
        const nwk_device_t *d = dev_at(z, sub);
        if (!d || !d->unique_key || d->tclk_shift == 0xFFu || !c->tclk_seed_valid) break;
        put32(e + ZNP_TCLK_TXFRMCNTR, c->tc_aps_frame_counter);
        put32(e + ZNP_TCLK_RXFRMCNTR, d->aps_in_counter);
        put64(e + ZNP_TCLK_EXTADDR, d->ext);
        e[ZNP_TCLK_KEYATTRIBUTES] = 0x02;   /* verified key */
        e[ZNP_TCLK_KEYTYPE] = 0x00;
        e[ZNP_TCLK_SEEDSHIFT_ICINDEX] = d->tclk_shift;
        break;
    }
    case EX_APS_KEY_DATA: {
        uint16_t slot = explicit_key_slot(z, sub);
        if (slot == NWK_DEVICE_NONE) break;
        const nwk_device_t *d = dev_at(z, slot);
        copy(e + ZNP_APSKEY_KEY, d->link_key, 16);
        put32(e + ZNP_APSKEY_TXFRMCNTR, c->tc_aps_frame_counter);
        put32(e + ZNP_APSKEY_RXFRMCNTR, d->aps_in_counter);
        break;
    }
    case EX_NWK_SEC_MATERIAL:
        if (!z->formed) break;
        put32(e + ZNP_SECMAT_FRAMECOUNTER, zb_coord_nwk_frame_counter(c));
        if (sub == 0) copy(e + ZNP_SECMAT_EXTENDEDPANID, c->mac.beacon.ext_pan_id, 8);
        else for (unsigned i = 0; i < 8; i++) e[ZNP_SECMAT_EXTENDEDPANID + i] = 0xFF;   /* generic entry */
        break;
    default:
        break;
    }
}

static uint8_t ex_write(znp_t *z, uint16_t table, uint16_t sub, const uint8_t *e) {
    zb_coord_t *c = z->coord;
    switch (table) {
    case EX_ADDRMGR: {
        uint8_t user = e[ZNP_AME_USER];
        uint64_t ext = get64(e + ZNP_AME_EXTADDR);
        uint16_t nwk = get16(e + ZNP_AME_NWKADDR);
        /* herdsman restores devices without key nor association with user 0:
         * still a network member (Z-Stack would forget it), kept here. */
        (void)user;
        bool set = ext != 0 && ext != UINT64_MAX && nwk != 0x0000 && nwk < 0xFFF8u;
        if (!set) {
            const nwk_device_t *d = dev_at(z, sub);
            if (d) (void)zb_coord_remove_device(c, d->ext);
            return ST_SUCCESS;
        }
        return zb_coord_import_device(c, sub, ext, nwk, (user & 0x01u) != 0, 0, 0xFF) ? ST_SUCCESS : ST_NV_OPER_FAILED;
    }
    case EX_TCLK: {
        uint64_t ext = get64(e + ZNP_TCLK_EXTADDR);
        if (!ext) return ST_SUCCESS;   /* empty entry */
        uint16_t slot = nwk_devices_find_ext(&c->devices, ext);
        uint8_t shift = e[ZNP_TCLK_SEEDSHIFT_ICINDEX];
        if (slot == NWK_DEVICE_NONE || !c->tclk_seed_valid || shift > 15u) return ST_NV_OPER_FAILED;
        nwk_device_t *d = &c->devices.dev[slot];
        zb_tclk_derive(c->tclk_seed, shift, ext, d->link_key);
        d->unique_key = true;
        d->tclk_shift = shift;
        d->aps_in_counter = get32(e + ZNP_TCLK_RXFRMCNTR);
        d->aps_in_seen = d->aps_in_counter != 0;
        zb_coord_raise_aps_frame_counter(c, host_counter(z, get32(e + ZNP_TCLK_TXFRMCNTR)));
        zb_coord_device_changed(c, slot);
        return ST_SUCCESS;
    }
    case EX_APS_KEY_DATA: {
        if (all(e + ZNP_APSKEY_KEY, 16, 0x00)) return ST_SUCCESS;
        if (!z->sm_valid) return ST_NV_OPER_FAILED;
        for (unsigned i = 0; i < ZNP_SM_CAPACITY; i++) {
            const uint8_t *sme = z->sm + 2 + i * ZNP_SME_SIZE;
            uint16_t ami = get16(sme + ZNP_SME_AMI);
            if (get16(sme + ZNP_SME_KEYNVID) != sub || ami >= NWK_MAX_DEVICES) continue;
            if (ami == 0x0000 && sme[ZNP_SME_AUTHENTICATIONOPTION] == 0) continue;   /* default entry */
            nwk_device_t *d = &c->devices.dev[ami];
            if (!d->ext) return ST_NV_OPER_FAILED;
            copy(d->link_key, e + ZNP_APSKEY_KEY, 16);
            d->unique_key = true;
            d->tclk_shift = 0xFF;
            d->aps_in_counter = get32(e + ZNP_APSKEY_RXFRMCNTR);
            d->aps_in_seen = d->aps_in_counter != 0;
            zb_coord_raise_aps_frame_counter(c, host_counter(z, get32(e + ZNP_APSKEY_TXFRMCNTR)));
            zb_coord_device_changed(c, ami);
            return ST_SUCCESS;
        }
        return ST_NV_OPER_FAILED;
    }
    case EX_NWK_SEC_MATERIAL: {
        const uint8_t *epid = e + ZNP_SECMAT_EXTENDEDPANID;
        if (all(epid, 8, 0x00)) return ST_SUCCESS;   /* empty entry */
        bool match = z->formed;
        for (unsigned i = 0; i < 8 && match; i++) match = epid[i] == c->mac.beacon.ext_pan_id[i];
        /* Only the current network's counter (or the generic entry) matters. */
        if (!z->formed || (!match && !all(epid, 8, 0xFF))) return ST_SUCCESS;
        return zb_coord_raise_nwk_frame_counter(c, host_counter(z, get32(e + ZNP_SECMAT_FRAMECOUNTER)))
                   ? ST_SUCCESS : ST_NV_OPER_FAILED;
    }
    default:
        return ST_NV_OPER_FAILED;
    }
}

/* ---- SYS ----------------------------------------------------------------- */
static void sys_version(znp_t *z) {
    uint8_t p[9] = {2, z->p.product, z->p.major, z->p.minor, z->p.maint};
    put32(p + 5, z->p.revision);
    srsp(z, S_SYS, 0x02, p, sizeof p);
}

static void handle_sys(znp_t *z, uint8_t cmd, const uint8_t *in, uint8_t n) {
    uint8_t out[256];
    switch (cmd) {
    case 0x00:   /* resetReq (AREQ): no SRSP; SYS_RESET_IND after reboot */
        if (z->p.reset) z->p.reset(z->p.ctx);
        return;
    case 0x01:
        put16(out, CAP_SYS | CAP_AF | CAP_ZDO | CAP_UTIL | CAP_APP_CNF);
        srsp(z, S_SYS, cmd, out, 2);
        return;
    case 0x02:
        sys_version(z);
        return;
    case 0x04:
        put64(out, z->ieee);
        srsp(z, S_SYS, cmd, out, 8);
        return;
    case 0x07: {   /* osalNvItemInit(id, len, initlen, init...) */
        if (n < 5 || n < 5u + in[4]) break;
        uint16_t id = get16(in), len = get16(in + 2);
        uint8_t initlen = in[4];
        uint8_t status;
        if (item_length(z, id)) status = ST_SUCCESS;   /* exists already */
        else if (id == NV_TCLK_SEED && len == 16 && initlen == 16) {
            zb_coord_set_tclk_seed(z->coord, in + 5);
            status = ST_NV_ITEM_INITIALIZED;
        } else if (synthesized(id) || !generic_id(id) || !len || len > NV_LOG_MAX_RECORD || initlen > len) status = ST_NV_OPER_FAILED;
        else {
            zero(out, len);
            copy(out, in + 5, initlen);
            status = generic_put(z, id, out, len) ? ST_NV_ITEM_INITIALIZED : ST_NV_OPER_FAILED;
        }
        srsp_status(z, S_SYS, cmd, status);
        return;
    }
    case 0x08:     /* osalNvRead(id, offset u8) */
    case 0x1C: {   /* osalNvReadExt(id, offset u16) */
        if (n != (cmd == 0x08 ? 3 : 4)) break;
        uint16_t id = get16(in);
        uint16_t offset = cmd == 0x08 ? in[2] : get16(in + 2);
        uint8_t item[NV_LOG_MAX_RECORD];
        uint16_t total = item_read(z, id, item);
        if (!total || offset >= total) { out[0] = ST_NV_OPER_FAILED; out[1] = 0; srsp(z, S_SYS, cmd, out, 2); return; }
        uint16_t chunk = (uint16_t)(total - offset);
        if (chunk > 246u) chunk = 246u;
        out[0] = ST_SUCCESS;
        out[1] = (uint8_t)chunk;
        copy(out + 2, item + offset, chunk);
        srsp(z, S_SYS, cmd, out, 2u + chunk);
        return;
    }
    case 0x09: {   /* osalNvWrite(id, offset u8, len u8, value) */
        if (n < 4 || n != 4u + in[3]) break;
        srsp_status(z, S_SYS, cmd, item_write(z, get16(in), in[2], in + 4, in[3]));
        return;
    }
    case 0x1D: {   /* osalNvWriteExt(id, offset u16, len u16, value) */
        if (n < 6 || n != 6u + get16(in + 4)) break;
        srsp_status(z, S_SYS, cmd, item_write(z, get16(in), get16(in + 2), in + 6, get16(in + 4)));
        return;
    }
    case 0x0F: {   /* stackTune(operation, value) */
        if (n != 2) break;
        if (in[0] == 0 && z->p.set_tx_power) out[0] = (uint8_t)z->p.set_tx_power(z->p.ctx, (int8_t)in[1]);
        else { rpc_error(z, 3, T_SREQ | S_SYS, cmd); return; }   /* other tunings: unsupported */
        srsp(z, S_SYS, cmd, out, 1);
        return;
    }
    case 0x12: {   /* osalNvDelete(id, len) */
        if (n != 4) break;
        uint16_t id = get16(in);
        uint8_t status = ST_SUCCESS;
        if (!item_length(z, id)) status = ST_NV_ITEM_INITIALIZED;   /* Z-Stack: item did not exist */
        else if (id == NV_NIB) {
            /* Deleting the NIB = leaving the network (herdsman commissioning). */
            zb_coord_forget(z->coord);
            z->formed = false;
            z->dev_state = ZNP_DEV_HOLD;
        } else if (synthesized(id)) status = ST_NV_OPER_FAILED;
        else if (!z->store || !nv_log_delete(z->store, (uint16_t)(ZNP_NV_GENERIC_BASE + id))) status = ST_NV_OPER_FAILED;
        srsp_status(z, S_SYS, cmd, status);
        return;
    }
    case 0x13:   /* osalNvLength(id) */
        if (n != 2) break;
        put16(out, item_length(z, get16(in)));
        srsp(z, S_SYS, cmd, out, 2);
        return;
    case 0x30: {   /* nvCreate(sysid, item, sub, len u32) */
        if (n != 9) break;
        uint16_t size = ex_length(in[0], get16(in + 1), get16(in + 3));
        srsp_status(z, S_SYS, cmd, size && get32(in + 5) == size ? ST_SUCCESS : ST_NV_OPER_FAILED);
        return;
    }
    case 0x31:   /* nvDelete: tables are fixed-size, entries cleared by writing them */
        if (n != 5) break;
        srsp_status(z, S_SYS, cmd, ST_NV_OPER_FAILED);
        return;
    case 0x32:   /* nvLength(sysid, item, sub) -> uint32 (herdsman reads the low byte) */
        if (n != 5) break;
        put32(out, ex_length(in[0], get16(in + 1), get16(in + 3)));
        srsp(z, S_SYS, cmd, out, 4);
        return;
    case 0x33: {   /* nvRead(sysid, item, sub, offset u16, len u8) */
        if (n != 8) break;
        uint16_t table = get16(in + 1), sub = get16(in + 3), offset = get16(in + 5);
        uint16_t size = ex_length(in[0], table, sub);
        uint8_t len = in[7];
        if (!size || (uint32_t)offset + len > size) { out[0] = ST_NV_OPER_FAILED; out[1] = 0; srsp(z, S_SYS, cmd, out, 2); return; }
        uint8_t e[32];
        ex_read(z, table, sub, e);
        out[0] = ST_SUCCESS;
        out[1] = len;
        copy(out + 2, e + offset, len);
        srsp(z, S_SYS, cmd, out, 2u + len);
        return;
    }
    case 0x34: {   /* nvWrite(sysid, item, sub, offset u16, len u8, value) */
        if (n < 8 || n != 8u + in[7]) break;
        uint16_t table = get16(in + 1), sub = get16(in + 3), offset = get16(in + 5);
        uint16_t size = ex_length(in[0], table, sub);
        uint8_t status;
        if (!size) status = ST_NV_OPER_FAILED;
        else if (offset != 0 || in[7] != size) status = ST_NV_BAD_ITEM_LEN;   /* whole entries only */
        else status = ex_write(z, table, sub, in + 8);
        srsp_status(z, S_SYS, cmd, status);
        return;
    }
    default:
        rpc_error(z, 2, T_SREQ | S_SYS, cmd);
        return;
    }
    rpc_error(z, 4, T_SREQ | S_SYS, cmd);
}

/* ---- network start / formation ------------------------------------------- */
static void state_change(znp_t *z, uint8_t state) {
    z->dev_state = state;
    areq(z, S_ZDO, 0xC0, &state, 1);
}

static void network_up(znp_t *z) {
    state_change(z, ZNP_DEV_COORD_STARTING);
    if (z->p.network_up) z->p.network_up(z->p.ctx, z->coord->mac.channel);
    state_change(z, ZNP_DEV_ZB_COORD);
}

static uint8_t lowest_channel(uint32_t mask) {
    for (uint8_t ch = 11; ch <= 26; ch++) if (mask & (1u << ch)) return ch;
    return 0;
}

/* Form a new network from the commissioning NV items. */
static bool form(znp_t *z) {
    uint8_t b[NV_LOG_MAX_RECORD], rnd[16];
    zb_coord_t *c = z->coord;
    uint32_t mask = z->primary_channels;
    if (!mask && generic_len(z, NV_CHANLIST, b) == 4) mask = get32(b);
    uint8_t channel = lowest_channel(mask);
    if (!channel) channel = 11;   /* Z-Stack default channel list starts at 11 */
    if (!z->p.random || !z->p.random(z->p.ctx, rnd)) return false;
    uint16_t pan = generic_len(z, NV_PANID, b) == 2 ? get16(b) : 0xFFFFu;
    if (pan == 0xFFFFu || pan == 0x0000u) pan = (uint16_t)(1u + (get16(rnd) % 0xFFF0u));
    uint8_t epid[8];
    if (generic_len(z, NV_EXTENDED_PAN_ID, b) == 8 && !all(b, 8, 0xFF) && !all(b, 8, 0x00)) copy(epid, b, 8);
    else put64(epid, z->ieee);
    uint8_t key[16];
    if (generic_len(z, NV_PRECFGKEY, b) == 16 && !all(b, 16, 0x00)) copy(key, b, 16);
    else if (!z->p.random(z->p.ctx, key)) return false;
    zb_coord_form(c, pan, channel, epid, key, 0);
    zero(key, sizeof key);
    zero(rnd, sizeof rnd);
    z->primary_channels = mask ? mask : (1u << channel);
    z->formed = true;
    return true;
}

static void gp_channel_service(znp_t *z);

/* End of a Touchlink session: radio back on the network channel. */
static bool interpan_close(znp_t *z) {
    bool ok = true;
    if (z->interpan_channel && z->p.gp_channel) ok = z->p.gp_channel(z->p.ctx, z->coord->mac.channel);
    z->interpan_channel = 0;
    if (!ok) {   /* recovered by radio_recover_service */
        z->interpan_stats.restore_failed++;
        z->radio_recover = 1;
        z->radio_recover_at_ms = z->coord->now_ms + ZNP_RECOVER_FIRST_MS;
    }
    return ok;
}

/* Bounded recovery of a radio left off the network channel (see
 * ZNP_RECOVER_RETRIES): channel moves with a doubling delay, one full
 * restart, then a reboot. */
static void radio_recover_service(znp_t *z) {
    if (!z->radio_recover || (int32_t)(z->coord->now_ms - z->radio_recover_at_ms) < 0) return;
    uint8_t net = z->coord->mac.channel;
    if (z->radio_recover > ZNP_RECOVER_RETRIES + 1u) {
        z->interpan_stats.restore_reboots++;
        z->radio_recover = 0;
        if (z->p.reset) z->p.reset(z->p.ctx);
        return;
    }
    if (z->radio_recover == ZNP_RECOVER_RETRIES + 1u) {
        z->interpan_stats.restore_restarts++;
        if (z->p.network_up) z->p.network_up(z->p.ctx, net);   /* RF core powered down and up */
    } else {
        z->interpan_stats.restore_retries++;
    }
    if (z->p.gp_channel && z->p.gp_channel(z->p.ctx, net)) {
        z->interpan_stats.restore_recovered++;
        z->radio_recover = 0;
        return;
    }
    uint8_t shift = z->radio_recover < 6u ? z->radio_recover : 5u;
    z->radio_recover_at_ms = z->coord->now_ms + (ZNP_RECOVER_FIRST_MS << shift);
    z->radio_recover++;
}

void znp_poll(znp_t *z) {
    if (z->interpan_channel && z->coord->now_ms - z->interpan_last_ms > ZNP_INTERPAN_IDLE_MS) {
        (void)interpan_close(z);
        z->interpan_stats.timeouts++;
    }
    radio_recover_service(z);
    gp_channel_service(z);
    if (z->form_pending) {
        z->form_pending = false;
        uint8_t note[3] = {0x00 /* success */, 0x04 /* formation */, 0x00};
        if (!z->formed && !form(z)) note[0] = 0x08;   /* BDB_COMMISSIONING_FORMATION_FAILURE */
        if (note[0] == 0x00 && z->dev_state != ZNP_DEV_ZB_COORD) network_up(z);
        areq(z, S_APP_CNF, 0x80, note, sizeof note);
    }
    if (z->start_pending) {
        z->start_pending = false;
        if (!z->formed && !form(z)) return;
        if (z->dev_state != ZNP_DEV_ZB_COORD) network_up(z);
    }
}

/* ---- APP_CNF -------------------------------------------------------------- */
static void handle_app_cnf(znp_t *z, uint8_t cmd, const uint8_t *in, uint8_t n) {
    switch (cmd) {
    case 0x08:   /* bdbSetChannel(isPrimary, mask u32) */
        if (n != 5) break;
        if (in[0]) z->primary_channels = get32(in + 1);
        else z->secondary_channels = get32(in + 1);
        srsp_status(z, S_APP_CNF, cmd, ST_SUCCESS);
        return;
    case 0x05:   /* bdbStartCommissioning(mode) */
        if (n != 1) break;
        if (in[0] & 0x04u) {   /* formation */
            srsp_status(z, S_APP_CNF, cmd, ST_SUCCESS);
            z->form_pending = true;
            return;
        }
        if (in[0] & 0x02u) {   /* network steering: open joining for 180 s */
            if (!z->formed || z->dev_state != ZNP_DEV_ZB_COORD) { srsp_status(z, S_APP_CNF, cmd, ST_FAILURE); return; }
            zb_coord_permit_join(z->coord, 180);
            srsp_status(z, S_APP_CNF, cmd, ST_SUCCESS);
            uint8_t d = 180;
            areq(z, S_ZDO, 0xCB, &d, 1);
            return;
        }
        srsp_status(z, S_APP_CNF, cmd, ST_FAILURE);   /* touchlink / finding-binding: not implemented */
        return;
    case 0x04: {   /* bdbAddInstallCode(format, ieee, code) */
        if (n < 9) break;
        uint64_t ext = get64(in + 1);
        uint8_t key[16];
        bool ok;
        if (in[0] == 0x02 && n == 9 + 16) { copy(key, in + 9, 16); ok = true; }
        else if (in[0] == 0x01) ok = zb_install_code_to_key(in + 9, (size_t)(n - 9), key);
        else ok = false;
        ok = ok && zb_coord_set_link_key(z->coord, ext, key);
        zero(key, sizeof key);
        srsp_status(z, S_APP_CNF, cmd, ok ? ST_SUCCESS : ST_FAILURE);
        return;
    }
    default:
        rpc_error(z, 2, T_SREQ | S_APP_CNF, cmd);
        return;
    }
    rpc_error(z, 4, T_SREQ | S_APP_CNF, cmd);
}

/* ---- UTIL ----------------------------------------------------------------- */
static uint8_t node_relation(const nwk_device_t *d) {
    if (d->relationship != NWK_REL_CHILD) return 0x06;   /* OTHER */
    bool ffd = (d->capability & 0x02u) != 0, rx_on = (d->capability & NWK_CAP_RX_ON_IDLE) != 0;
    return ffd ? (rx_on ? 0x04 : 0x03) : (rx_on ? 0x02 : 0x01);
}

static void handle_util(znp_t *z, uint8_t cmd, const uint8_t *in, uint8_t n) {
    uint8_t out[250];
    switch (cmd) {
    case 0x00: {   /* getDeviceInfo */
        if (n) break;
        size_t k = 0;
        out[k++] = ST_SUCCESS;
        put64(out + k, z->ieee); k += 8;
        put16(out + k, 0x0000); k += 2;
        out[k++] = 0x07;   /* coordinator | router | end device capable build */
        out[k++] = z->dev_state;
        size_t count_at = k++;
        uint8_t count = 0;
        for (uint16_t s = 0; s < NWK_MAX_DEVICES && count < 64; s++) {
            const nwk_device_t *d = dev_at(z, s);
            if (!d || d->relationship != NWK_REL_CHILD) continue;
            put16(out + k, d->short_addr); k += 2;
            count++;
        }
        out[count_at] = count;
        srsp(z, S_UTIL, cmd, out, k);
        return;
    }
    case 0x4A: {   /* assocGetWithAddress(extaddr, nwkaddr) */
        if (n != 10) break;
        uint64_t ext = get64(in);
        uint16_t slot = ext ? nwk_devices_find_ext(&z->coord->devices, ext) : nwk_devices_find_short(&z->coord->devices, get16(in + 8));
        const nwk_device_t *d = slot == NWK_DEVICE_NONE ? 0 : dev_at(z, slot);
        put16(out, d ? d->short_addr : 0xFFFE);
        put16(out + 2, d ? slot : 0xFFFF);
        out[4] = d ? node_relation(d) : 0xFF;
        srsp(z, S_UTIL, cmd, out, 5);
        return;
    }
    /* assocRemove / assocAdd (Koenkk's Z-Stack, revision >= 20200805 /
     * 20201026, which our revision announces): herdsman removes a child whose
     * indirect frames expire, retries through a route discovery, and adds it
     * back if that fails too. An RPC error here made the whole command fail. */
    case 0x63:   /* assocRemove(ieee) */
        if (n != 8) break;
        srsp_status(z, S_UTIL, cmd, zb_coord_child_remove(z->coord, get64(in)) ? ST_SUCCESS : ST_FAILURE);
        return;
    case 0x64:   /* assocAdd(ieee, nwkaddr, noderelation) */
        if (n != 11) break;
        srsp_status(z, S_UTIL, cmd, zb_coord_child_add(z->coord, get64(in), get16(in + 8), in[10]) ? ST_SUCCESS
                                                                                                  : ST_FAILURE);
        return;
    default:
        rpc_error(z, 2, T_SREQ | S_UTIL, cmd);   /* incl. ledControl: no LED driver */
        return;
    }
    rpc_error(z, 4, T_SREQ | S_UTIL, cmd);
}

/* ---- ZDO ------------------------------------------------------------------ */
/* AREQ command id carrying a ZDO response cluster (herdsman definition). */
static int zdo_rsp_cmd(uint16_t cluster) {
    switch (cluster) {
    case 0x8000: case 0x8001: case 0x8002: case 0x8003: case 0x8004: case 0x8005: case 0x8006:
    case 0x8021: case 0x8022: case 0x8031: case 0x8032: case 0x8033: case 0x8034: case 0x8036: case 0x8038:
        return 0x80 + (cluster & 0xFF);
    case 0x8015: return 0x8A;
    default: return -1;
    }
}

/* Forward a ZDO response (starting with its TSN) as the MT AREQ. */
static void zdo_response_areq(znp_t *z, uint16_t src, uint16_t cluster, const uint8_t *rsp, size_t len) {
    int cmd = zdo_rsp_cmd(cluster);
    if (cmd < 0 || len < 2) return;
    uint8_t out[250];
    size_t k = 0;
    rsp++; len--;   /* drop the TSN */
    if (cluster == 0x8000 || cluster == 0x8001) {
        /* No source address; Z-Stack sends startIndex before numAssocDev. */
        if (len > sizeof out) return;
        copy(out, rsp, len);
        k = len;
        if (k == 11) { out[k++] = 0; out[k++] = 0; }
        else if (k >= 13) { uint8_t t = out[11]; out[11] = out[12]; out[12] = t; }
    } else {
        if (len + 2 > sizeof out) return;
        put16(out, src);
        copy(out + 2, rsp, len);
        k = len + 2;
    }
    z->stats.zdo_rx++;
    areq(z, S_ZDO, (uint8_t)cmd, out, k);
}

static uint8_t zdo_send(znp_t *z, uint16_t dst, uint16_t cluster, const uint8_t *payload, size_t len) {
    uint8_t frame[96];
    if (len + 1 > sizeof frame) return ST_INVALID_PARAM;
    frame[0] = z->zdo_seq++;
    copy(frame + 1, payload, len);
    if (dst == 0x0000) {
        uint8_t rsp[128];
        size_t n = zb_coord_zdo_local(z->coord, cluster, frame, len + 1, false, rsp, sizeof rsp);
        if (n) zdo_response_areq(z, 0x0000, (uint16_t)(cluster | 0x8000u), rsp, n);
        return ST_SUCCESS;
    }
    if (z->dev_state != ZNP_DEV_ZB_COORD) return ST_FAILURE;
    uint8_t status = zb_coord_send_aps(z->coord, dst, 0, 0, cluster, 0x0000, frame, (uint8_t)(len + 1), false);
    if (status == ZB_SEND_OK) { z->stats.zdo_tx++; return ST_SUCCESS; }
    return status == ZB_SEND_NO_ROUTE ? ST_NWK_NO_ROUTE : ST_FAILURE;
}

static void permit_join_local(znp_t *z, uint8_t seconds) {
    zb_coord_permit_join(z->coord, seconds == 0xFF ? 254u : seconds);
    areq(z, S_ZDO, 0xCB, &seconds, 1);
}

static void handle_zdo(znp_t *z, uint8_t cmd, const uint8_t *in, uint8_t n) {
    uint8_t out[64];
    switch (cmd) {
    case 0x40:   /* startupFromApp(startDelay) */
        if (n != 2) break;
        /* 0: restored network state, 1: new network state */
        srsp_status(z, S_ZDO, cmd, z->formed ? 0x00 : 0x01);
        z->start_pending = true;
        return;
    case 0x50: {   /* extNwkInfo */
        if (n) break;
        zb_coord_t *c = z->coord;
        size_t k = 0;
        put16(out + k, 0x0000); k += 2;
        out[k++] = z->dev_state;
        put16(out + k, z->formed ? c->mac.pan_id : 0xFFFF); k += 2;
        put16(out + k, 0xFFFE); k += 2;   /* no parent */
        if (z->formed) copy(out + k, c->mac.beacon.ext_pan_id, 8); else zero(out + k, 8);
        k += 8;
        zero(out + k, 8); k += 8;
        out[k++] = z->formed ? c->mac.channel : 0;
        srsp(z, S_ZDO, cmd, out, k);
        return;
    }
    case 0x4A: {   /* extFindGroup(endpoint, group) */
        if (n != 3) break;
        uint16_t g = get16(in + 1);
        uint8_t status = ST_FAILURE;
        for (unsigned i = 0; i < ZNP_MAX_GROUPS; i++)
            if (z->groups[i].used && z->groups[i].endpoint == in[0] && z->groups[i].group == g) status = ST_SUCCESS;
        out[0] = status;
        put16(out + 1, g);
        out[3] = 0;
        srsp(z, S_ZDO, cmd, out, 4);
        return;
    }
    case 0x4B: {   /* extAddGroup(endpoint, group, namelen, name) */
        if (n < 4) break;
        uint16_t g = get16(in + 1);
        uint8_t status = ST_FAILURE;
        for (unsigned i = 0; i < ZNP_MAX_GROUPS; i++)
            if (z->groups[i].used && z->groups[i].endpoint == in[0] && z->groups[i].group == g) status = ST_SUCCESS;
        for (unsigned i = 0; i < ZNP_MAX_GROUPS && status != ST_SUCCESS; i++) {
            if (z->groups[i].used) continue;
            z->groups[i].used = true;
            z->groups[i].endpoint = in[0];
            z->groups[i].group = g;
            status = ST_SUCCESS;
        }
        srsp_status(z, S_ZDO, cmd, status);
        return;
    }
    case 0x47: {   /* extRemoveGroup(endpoint, group) */
        if (n != 3) break;
        uint8_t status = ST_FAILURE;
        for (unsigned i = 0; i < ZNP_MAX_GROUPS; i++) {
            if (z->groups[i].used && z->groups[i].endpoint == in[0] && z->groups[i].group == get16(in + 1)) {
                z->groups[i].used = false;
                status = ST_SUCCESS;
            }
        }
        srsp_status(z, S_ZDO, cmd, status);
        return;
    }
    case 0x45:   /* extRouteDisc(dst, options, radius): broadcast Route Request */
        if (n != 4) break;
        srsp_status(z, S_ZDO, cmd, z->dev_state == ZNP_DEV_ZB_COORD && zb_coord_discover_route(z->coord, get16(in))
                                       ? ST_SUCCESS : ST_FAILURE);
        return;
    /* ZDO requests (herdsman payload formats, see zStackAdapter.sendZdoInternal) */
    case 0x00: {   /* nwkAddrReq(ieee, reqType, startIndex): broadcast */
        if (n != 10) break;
        if (get64(in) == z->ieee) { srsp_status(z, S_ZDO, cmd, zdo_send(z, 0x0000, 0x0000, in, n)); return; }
        srsp_status(z, S_ZDO, cmd, zdo_send(z, 0xFFFD, 0x0000, in, n));
        return;
    }
    case 0x01:   /* ieeeAddrReq(nwk of interest, reqType, startIndex) */
        if (n != 4) break;
        srsp_status(z, S_ZDO, cmd, zdo_send(z, get16(in), 0x0001, in, n));
        return;
    case 0x36: {   /* mgmtPermitJoinReq(addrMode, dst, duration, tcSignificance) */
        if (n != 5) break;
        uint16_t dst = get16(in + 1);
        uint8_t status = ST_SUCCESS;
        bool broadcast = in[0] == 0x0F || dst >= 0xFFF8u;
        if (dst == 0x0000 || broadcast) {
            if (z->dev_state != ZNP_DEV_ZB_COORD) { srsp_status(z, S_ZDO, cmd, ST_FAILURE); return; }
            permit_join_local(z, in[3]);
        }
        if (broadcast) {
            uint8_t frame[3] = {z->zdo_seq++, in[3], in[4]};
            (void)zb_coord_send_aps(z->coord, dst, 0, 0, 0x0036, 0x0000, frame, 3, false);
        } else if (dst == 0x0000) {
            const uint8_t rsp[2] = {z->zdo_seq++, ST_SUCCESS};
            zdo_response_areq(z, 0x0000, 0x8036, rsp, 2);
        } else {
            status = zdo_send(z, dst, 0x0036, in + 3, 2);
        }
        srsp_status(z, S_ZDO, cmd, status);
        return;
    }
    case 0x21:   /* bindReq(dst, srcIeee, srcEp, cluster, mode, dstAddr..., zero padded to 21) */
    case 0x22: {
        if (n != 23) break;
        const uint8_t *p = in + 2;
        size_t len = p[11] == 0x03 ? 21 : p[11] == 0x01 ? 14 : 0;
        if (!len) { srsp_status(z, S_ZDO, cmd, ST_INVALID_PARAM); return; }
        srsp_status(z, S_ZDO, cmd, zdo_send(z, get16(in), cmd == 0x21 ? 0x0021 : 0x0022, p, len));
        return;
    }
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x0C:
    case 0x31: case 0x32: case 0x33: case 0x34: {
        if (n < 2) break;
        static const uint16_t clusters[0x35] = {
            [0x02] = 0x0002, [0x03] = 0x0003, [0x04] = 0x0004, [0x05] = 0x0005, [0x06] = 0x0006,
            [0x0C] = 0x0015, [0x31] = 0x0031, [0x32] = 0x0032, [0x33] = 0x0033, [0x34] = 0x0034,
        };
        srsp_status(z, S_ZDO, cmd, zdo_send(z, get16(in), clusters[cmd], in + 2, (size_t)(n - 2)));
        return;
    }
    case 0x4E: {   /* extUpdateNwkKey(dstAddr, keySeqNum, key[16]) */
        if (n != 19) break;
        srsp_status(z, S_ZDO, cmd, zb_coord_update_network_key(z->coord, get16(in), in[2], in + 3) ? ST_SUCCESS
                                                                                                      : ST_FAILURE);
        return;
    }
    case 0x4F: {   /* extSwitchNwkKey(dstAddr, keySeqNum) */
        if (n != 3) break;
        srsp_status(z, S_ZDO, cmd, zb_coord_switch_network_key(z->coord, get16(in), in[2]) ? ST_SUCCESS : ST_FAILURE);
        return;
    }
    case 0x37: {   /* mgmtNwkUpdateReq(dstaddr, dstaddrmode, channelmask u32, scanduration, scancount, manager) */
        if (n < 8) break;
        uint32_t mask = get32(in + 3);
        uint8_t duration = in[7], channel = 0;
        for (uint8_t ch = 11; ch <= 26; ch++) if (mask == (1ul << ch)) channel = ch;
        /* Only the channel change (0xFE, one channel) is implemented: an
         * energy scan or a mask/manager update is refused, not faked. */
        uint8_t status = duration == 0xFE && channel && zb_coord_change_channel(z->coord, channel) ? ST_SUCCESS
                                                                                                  : ST_FAILURE;
        srsp_status(z, S_ZDO, cmd, status);
        return;
    }
    default:
        rpc_error(z, 2, T_SREQ | S_ZDO, cmd);
        return;
    }
    rpc_error(z, 4, T_SREQ | S_ZDO, cmd);
}

/* ---- AF ------------------------------------------------------------------- */
static const zb_endpoint_t *endpoint(znp_t *z, uint8_t ep) {
    for (unsigned i = 0; i < z->coord->endpoint_count; i++)
        if (z->coord->endpoints[i].endpoint == ep) return &z->coord->endpoints[i];
    return 0;
}

static void data_confirm(znp_t *z, uint8_t status, uint8_t ep, uint8_t transid) {
    const uint8_t p[3] = {status, ep, transid};
    areq(z, S_AF, 0x80, p, 3);
}

static void incoming(znp_t *z, uint16_t group, uint16_t cluster, uint16_t src, uint8_t src_ep, uint8_t dst_ep,
                     bool broadcast, uint8_t lqi, const uint8_t *data, size_t len) {
    uint8_t out[250];
    if (len > ZNP_INCOMING_MAX) {
        /* Longer than one frame: kept, announced by AF_INCOMING_MSG_EXT with the
         * total length and no data; herdsman reads it with AF_DATA_RETRIEVE. */
        uint32_t now = z->coord->now_ms;
        if (len > sizeof z->huge.data || (z->huge.used && now - z->huge.timestamp < ZNP_HUGE_HOLD_MS)) {
            z->stats.af_rx_huge_dropped++;
            return;
        }
        z->huge.used = true;
        z->huge.len = (uint16_t)len;
        z->huge.timestamp = now;
        copy(z->huge.data, data, len);
        size_t k = 0;
        put16(out + k, group); k += 2;
        put16(out + k, cluster); k += 2;
        out[k++] = 0x02;                              /* 16-bit source address */
        put64(out + k, src); k += 8;
        out[k++] = src_ep;
        put16(out + k, z->coord->mac.pan_id); k += 2;
        out[k++] = dst_ep;
        out[k++] = broadcast ? 1 : 0;
        out[k++] = lqi;
        out[k++] = 0;                                 /* securityuse */
        put32(out + k, now); k += 4;                  /* timestamp: the AF_DATA_RETRIEVE key */
        out[k++] = 0;                                 /* transseq */
        put16(out + k, (uint16_t)len); k += 2;        /* no data: retrieved by blocks */
        z->stats.af_rx_huge++;
        areq(z, S_AF, 0x82, out, k);
        return;
    }
    size_t k = 0;
    put16(out + k, group); k += 2;
    put16(out + k, cluster); k += 2;
    put16(out + k, src); k += 2;
    out[k++] = src_ep;
    out[k++] = dst_ep;
    out[k++] = broadcast ? 1 : 0;
    out[k++] = lqi;
    out[k++] = 0;   /* securityuse (deprecated field) */
    put32(out + k, z->coord->now_ms); k += 4;
    out[k++] = 0;   /* transseq */
    out[k++] = (uint8_t)len;
    copy(out + k, data, len);
    k += len;
    z->stats.af_rx++;
    areq(z, S_AF, 0x81, out, k);
}

static uint8_t af_send(znp_t *z, uint16_t dst, bool group, uint8_t dst_ep, uint8_t src_ep, uint16_t cluster,
                       uint8_t transid, uint8_t options, const uint8_t *data, size_t len) {
    const zb_endpoint_t *ep = endpoint(z, src_ep);
    if (!ep) return 0x02;   /* afStatus_INVALID_PARAMETER: unregistered source endpoint */
    if (z->dev_state != ZNP_DEV_ZB_COORD) return ST_FAILURE;
    if (len > ZB_FRAG_MAX) return 0x02;   /* longer than one ZNP frame can carry */
    if (!group && dst == 0x0000) {
        /* Loopback to a local endpoint, as Z-Stack does. */
        data_confirm(z, ST_SUCCESS, src_ep, transid);
        if (endpoint(z, dst_ep)) incoming(z, 0, cluster, 0x0000, src_ep, dst_ep, false, 0xFF, data, len);
        return ST_SUCCESS;
    }
    /* A unicast needs a slot to carry its confirmation back: without one the
     * request is refused (ZMemError) BEFORE anything is sent, never confirmed
     * blindly. */
    int slot = -1;
    if (!group && !nwk_is_broadcast(dst))
        for (unsigned i = 0; i < ZNP_MAX_CONFIRMS && slot < 0; i++) if (!z->confirms[i].used) slot = (int)i;
    if (!group && !nwk_is_broadcast(dst) && slot < 0) { z->stats.af_tx_failed++; return 0x10u; }
    if (cluster == GP_CLUSTER && (src_ep == GP_ENDPOINT || dst_ep == GP_ENDPOINT))
        gp_proxy_host_frame(&z->gp, data, len, z->coord->now_ms);   /* we are a GP proxy too (channel: znp_poll) */
    uint8_t status;
    if (options & AF_EN_SECURITY) {
        /* APS encryption with the destination's link key, never silently dropped. */
        if (group || nwk_is_broadcast(dst)) return 0x02;
        status = zb_coord_send_aps_secured(z->coord, dst, dst_ep, src_ep, cluster, ep->profile, data, (uint8_t)len,
                                           (options & AF_ACK_REQUEST) != 0);
    } else {
        status = zb_coord_send_aps_ex(z->coord, dst, group, dst_ep, src_ep, cluster, ep->profile, data,
                                      (uint8_t)len, (options & AF_ACK_REQUEST) != 0);
    }
    if (status != ZB_SEND_OK) {
        z->stats.af_tx_failed++;
        /* Z-Stack codes herdsman knows how to recover from. */
        /* A full local transmit queue is reported as BUFFER_FULL (0x11), which
         * herdsman retries after a delay ("many commands at once"), whereas
         * MAC_TRANSACTION_OVERFLOW (0xF1) made it give the command up. */
        uint8_t code = status == ZB_SEND_NO_ROUTE ? ST_NWK_NO_ROUTE :
                       status == ZB_SEND_MAC_REFUSED ? 0x11 /* ZStatus BUFFER_FULL */ : ST_FAILURE;
        data_confirm(z, code, src_ep, transid);
        return ST_SUCCESS;
    }
    z->stats.af_tx++;
    if (group || nwk_is_broadcast(dst)) {
        data_confirm(z, ST_SUCCESS, src_ep, transid);   /* broadcasts confirm on hand-over */
        return ST_SUCCESS;
    }
    z->confirms[slot].used = true;
    z->confirms[slot].handle = z->coord->last_send_handle;
    z->confirms[slot].endpoint = src_ep;
    z->confirms[slot].transid = transid;
    return ST_SUCCESS;
}

static void handle_af(znp_t *z, uint8_t cmd, const uint8_t *in, uint8_t n) {
    switch (cmd) {
    case 0x00: {   /* register */
        if (n < 9) break;
        zb_endpoint_t ep = {0};
        ep.endpoint = in[0];
        ep.profile = get16(in + 1);
        ep.device_id = get16(in + 3);
        ep.version = in[5];
        size_t k = 7;
        ep.in_count = in[k++];
        if (ep.in_count > ZB_MAX_EP_CLUSTERS || n < k + 2u * ep.in_count + 1u) break;
        for (unsigned i = 0; i < ep.in_count; i++, k += 2) ep.in[i] = get16(in + k);
        ep.out_count = in[k++];
        if (ep.out_count > ZB_MAX_EP_CLUSTERS || n != k + 2u * ep.out_count) break;
        for (unsigned i = 0; i < ep.out_count; i++, k += 2) ep.out[i] = get16(in + k);
        /* Z-Stack refuses to register an endpoint twice (afStatus_INVALID_PARAMETER). */
        uint8_t status = endpoint(z, ep.endpoint) ? 0x02 :
                         zb_coord_register_endpoint(z->coord, &ep) ? ST_SUCCESS : 0x10;
        srsp_status(z, S_AF, cmd, status);
        return;
    }
    case 0x01: {   /* dataRequest(dst, dstEp, srcEp, cluster, transid, options, radius, len, data) */
        if (n < 10 || n != 10u + in[9]) break;
        srsp_status(z, S_AF, cmd, af_send(z, get16(in), false, in[2], in[3], get16(in + 4), in[6], in[7], in + 10, in[9]));
        return;
    }
    case 0x02: {   /* dataRequestExt(mode, addr[8], dstEp, panid, srcEp, cluster, transid, options, radius, len u16, data) */
        if (n < 20 || n != 20u + get16(in + 18)) break;
        uint8_t mode = in[0];
        uint16_t dst = get16(in + 1);
        uint8_t status;
        uint16_t pan = get16(in + 10);
        if (pan != 0 && pan != z->coord->mac.pan_id) {
            /* Inter-PAN (Touchlink): only during an interPanCtl session, to a
             * 64-bit address or MAC broadcast, profile of the source endpoint. */
            const zb_endpoint_t *ep = endpoint(z, in[12]);
            uint16_t dlen = get16(in + 18);
            uint64_t dst64 = mode == 0x03 ? get64(in + 1) : 0;
            if (!z->interpan_channel || !ep || dlen > 80u || (mode != 0x03 && !((mode == 0x02 || mode == 0x0F) && dst == 0xFFFF)))
                status = 0x02;
            else {
                status = zb_coord_send_interpan(z->coord, dst64, pan, get16(in + 13), ep->profile, in + 20, (uint8_t)dlen) ==
                         ZB_SEND_OK ? ST_SUCCESS : ST_FAILURE;
                z->interpan_last_ms = z->coord->now_ms;
                if (status == ST_SUCCESS) data_confirm(z, ST_SUCCESS, in[12], in[15]);   /* on hand-over */
            }
        }
        else if (mode == 0x02 || mode == 0x0F) status = af_send(z, dst, false, in[9], in[12], get16(in + 13), in[15], in[16], in + 20, get16(in + 18));
        else if (mode == 0x01) status = af_send(z, dst, true, in[9], in[12], get16(in + 13), in[15], in[16], in + 20, get16(in + 18));
        else if (mode == 0x03) {
            uint16_t slot = nwk_devices_find_ext(&z->coord->devices, get64(in + 1));
            status = slot == NWK_DEVICE_NONE ? ST_NWK_NO_ROUTE :
                     af_send(z, z->coord->devices.dev[slot].short_addr, false, in[9], in[12], get16(in + 13), in[15], in[16], in + 20, get16(in + 18));
        } else status = 0x02;
        srsp_status(z, S_AF, cmd, status);
        return;
    }
    case 0x12: {   /* dataRetrieve(timestamp u32, index u16, length u8): block of the long message */
        if (n != 7) break;
        uint32_t ts = get32(in);
        uint16_t index = get16(in + 4);
        uint8_t length = in[6], out[2 + 240];
        out[1] = 0;
        if (!z->huge.used || ts != z->huge.timestamp || length > 240u) {
            out[0] = 0x02;                            /* afStatus_INVALID_PARAMETER */
            srsp(z, S_AF, cmd, out, 2);
            return;
        }
        if (!length) {                                /* the host is done: freed */
            z->huge.used = false;
            out[0] = ST_SUCCESS;
            srsp(z, S_AF, cmd, out, 2);
            return;
        }
        if ((uint32_t)index + length > z->huge.len) { out[0] = 0x02; srsp(z, S_AF, cmd, out, 2); return; }
        out[0] = ST_SUCCESS;
        out[1] = length;
        copy(out + 2, z->huge.data + index, length);
        srsp(z, S_AF, cmd, out, 2u + length);
        return;
    }
    case 0x10: {   /* interPanCtl(cmd, data): 0 back to the network, 1 channel, 2 endpoint, 3 check */
        if (n < 1) break;
        uint8_t status = ST_FAILURE;
        if (in[0] == 0) {
            status = interpan_close(z) ? ST_SUCCESS : ST_FAILURE;   /* the host learns the radio is deaf */
        } else if (in[0] == 1 && n == 2 && in[1] >= 11 && in[1] <= 26 && z->p.gp_channel && !z->radio_recover) {
            if (z->p.gp_channel(z->p.ctx, in[1])) {
                z->interpan_channel = in[1];
                z->interpan_last_ms = z->coord->now_ms;
                z->gp_off_channel = 0;   /* the radio left any TempMaster channel; GP asks again after */
                status = ST_SUCCESS;
            }
        } else if (in[0] == 2 && n == 2 && endpoint(z, in[1])) {
            z->interpan_ep = in[1];
            status = ST_SUCCESS;
        } else if (in[0] == 3 && n == 4) {   /* InterPanChk(panId, endpoint) */
            uint16_t pan = get16(in + 1);
            status = z->interpan_channel && in[3] == z->interpan_ep && pan != z->coord->mac.pan_id ? ST_SUCCESS : ST_FAILURE;
        }
        srsp_status(z, S_AF, cmd, status);
        return;
    }
    default:
        rpc_error(z, 2, T_SREQ | S_AF, cmd);
        return;
    }
    rpc_error(z, 4, T_SREQ | S_AF, cmd);
}

/* ---- coordinator upcalls -------------------------------------------------- */
static void up_joined(void *ctx, uint16_t short_addr, uint64_t ext, uint8_t capability) {
    znp_t *z = ctx;
    uint8_t p[13];
    put16(p, short_addr);
    put64(p + 2, ext);
    put16(p + 10, 0x0000);   /* parent: only direct children join (one-hop network) */
    areq(z, S_ZDO, 0xCA, p, 12);   /* tcDeviceInd -> herdsman "deviceJoined" */
    put16(p, short_addr);
    put16(p + 2, short_addr);
    put64(p + 4, ext);
    p[12] = capability;
    areq(z, S_ZDO, 0xC1, p, 13);   /* endDeviceAnnceInd */
}

static void up_aps(void *ctx, const nwk_header_t *nwk, const aps_header_t *aps, const uint8_t *payload, size_t len,
                   uint8_t lqi) {
    znp_t *z = ctx;
    if (aps->profile == 0x0000 && aps->dst_endpoint == 0 && aps->delivery != APS_DELIVERY_GROUP) {
        if (aps->cluster & 0x8000u) zdo_response_areq(z, nwk->src, aps->cluster, payload, len);
        return;
    }
    if (aps->delivery == APS_DELIVERY_GROUP) {
        for (unsigned i = 0; i < ZNP_MAX_GROUPS; i++) {
            if (!z->groups[i].used || z->groups[i].group != aps->group) continue;
            incoming(z, aps->group, aps->cluster, nwk->src, aps->src_endpoint, z->groups[i].endpoint, true, lqi, payload, len);
            return;
        }
        /* Not a member: delivered to endpoint 1 anyway, as the Z-Stack ZNP
         * coordinator used with Zigbee2MQTT does (af.c, IS_COORDINATOR patch):
         * remotes that drive their devices through a group (Profalux shutter
         * remotes, IKEA...) are only seen by Z2M this way. */
        if (endpoint(z, 1)) {
            z->stats.af_rx_group_any++;
            incoming(z, aps->group, aps->cluster, nwk->src, aps->src_endpoint, 1, true, lqi, payload, len);
        }
        return;
    }
    if (aps->dst_endpoint != 0xFF && !endpoint(z, aps->dst_endpoint)) { z->stats.af_rx_dropped++; return; }
    incoming(z, 0, aps->cluster, nwk->src, aps->src_endpoint, aps->dst_endpoint, nwk_is_broadcast(nwk->dst), lqi,
             payload, len);
}

static void up_left(void *ctx, uint16_t short_addr, uint64_t ext, bool rejoin) {
    znp_t *z = ctx;
    uint8_t p[13];
    put16(p, short_addr);
    put64(p + 2, ext);
    p[10] = 0;              /* request: the device left on its own */
    p[11] = 0;              /* remove children */
    p[12] = rejoin ? 1 : 0;
    areq(z, S_ZDO, 0xC9, p, sizeof p);   /* leaveInd -> herdsman "deviceLeave" (unless rejoin) */
}

static void incoming(znp_t *z, uint16_t group, uint16_t cluster, uint16_t src, uint8_t src_ep, uint8_t dst_ep,
                     bool broadcast, uint8_t lqi, const uint8_t *data, size_t len);
static const zb_endpoint_t *endpoint(znp_t *z, uint8_t ep);

/* GPD frame heard by the coordinator: as a GP proxy, deliver the GP
 * (Commissioning) Notification to the sink (herdsman) on the GP endpoint.
 * Marked as received by broadcast, as proxies groupcast them. */
/* Inter-PAN frame received during a Touchlink session: AF_INCOMING_MSG_EXT
 * as Z-Stack (64-bit source, source endpoint 0xFE, source PAN). */
static void up_interpan(void *ctx, bool src_ext, uint64_t src, uint16_t src_pan, bool broadcast, uint16_t cluster,
                        uint16_t profile, const uint8_t *zcl, size_t len, uint8_t lqi) {
    znp_t *z = ctx;
    if (!z->interpan_channel || !z->interpan_ep || len > 200u) return;
    /* Inter-PAN frames are not secured and their source IEEE is not
     * authenticated: only Touchlink commissioning is passed up, never what
     * could pass for a known device's report or command. */
    if (profile != ZNP_TOUCHLINK_PROFILE || cluster != ZNP_TOUCHLINK_CLUSTER) { z->interpan_stats.rx_dropped++; return; }
    z->interpan_last_ms = z->coord->now_ms;
    uint8_t out[230];
    size_t k = 0;
    put16(out + k, 0x0000); k += 2;                      /* group */
    put16(out + k, cluster); k += 2;
    out[k++] = src_ext ? 0x03 : 0x02;                    /* source address mode */
    put64(out + k, src); k += 8;
    out[k++] = 0xFE;                                     /* source endpoint (inter-PAN) */
    put16(out + k, src_pan); k += 2;
    out[k++] = z->interpan_ep;
    out[k++] = broadcast ? 1 : 0;
    out[k++] = lqi;
    out[k++] = 0;                                        /* security use */
    put32(out + k, z->coord->now_ms); k += 4;
    out[k++] = 0;                                        /* transaction sequence */
    put16(out + k, (uint16_t)len); k += 2;
    copy(out + k, zcl, len);
    k += len;
    z->stats.af_rx++;
    areq(z, S_AF, 0x82, out, k);
}

static void up_gpdf(void *ctx, const uint8_t *nwk, size_t len, uint8_t mac_seq, uint8_t lqi) {
    znp_t *z = ctx;
    if (!endpoint(z, GP_ENDPOINT)) return;
    uint8_t zcl[160];
    z->gp.net_channel = z->coord->mac.channel;
    z->gp.cur_channel = z->gp_off_channel ? z->gp_off_channel : z->coord->mac.channel;
    size_t n = gp_proxy_rx(&z->gp, nwk, len, mac_seq, z->coord->now_ms, zcl, sizeof zcl);
    if (z->gp.tx_ready) {
        /* First: the GPD listens gpTxOffset after its frame, whatever the host does. */
        z->gp.tx_ready = false;
        if (z->p.gp_tx && z->p.gp_tx(z->p.ctx, z->gp.tx_mpdu, z->gp.tx_len)) z->gp_stats.tx++;
        else z->gp_stats.tx_failed++;
    }
    if (n) incoming(z, 0, GP_CLUSTER, 0x0000, GP_ENDPOINT, GP_ENDPOINT, true, lqi, zcl, n);
}

/* TempMaster on another channel: the radio follows what the GP proxy asks
 * (gp_proxy_tick), back to the network channel when done or expired. */
static void gp_channel_service(znp_t *z) {
    if (!z->p.gp_channel || z->interpan_channel || z->radio_recover) return;   /* Touchlink owns the radio */
    z->gp.net_channel = z->coord->mac.channel;
    uint8_t want = gp_proxy_tick(&z->gp, z->coord->now_ms);
    if (want == z->coord->mac.channel) want = 0;
    if (want == z->gp_off_channel) return;
    uint8_t target = want ? want : z->coord->mac.channel;
    if (z->p.gp_channel(z->p.ctx, target)) {
        z->gp_off_channel = want;
        z->gp_stats.channel_moves++;
    } else {
        z->gp_stats.channel_failed++;
        if (want) z->gp.want_channel = 0;   /* stay on the network channel */
    }
}

static void up_channel(void *ctx, uint8_t channel) {
    znp_t *z = ctx;
    if (z->p.network_up) z->p.network_up(z->p.ctx, channel);   /* the radio follows the network */
}

static bool up_random(void *ctx, uint8_t key[16]) {
    znp_t *z = ctx;
    return z->p.random && z->p.random(z->p.ctx, key);
}

static void up_confirm(void *ctx, uint8_t handle, uint8_t mac_status) {
    znp_t *z = ctx;
    for (unsigned i = 0; i < ZNP_MAX_CONFIRMS; i++) {
        if (!z->confirms[i].used || z->confirms[i].handle != handle) continue;
        z->confirms[i].used = false;
        /* Z-Stack reports MAC codes as they are (0xE9 no ACK, 0xF0 expired...). */
        data_confirm(z, mac_status, z->confirms[i].endpoint, z->confirms[i].transid);
        return;
    }
}

/* ---- boot / dispatch ---------------------------------------------------- */
void znp_boot(znp_t *z, const znp_platform_t *platform, zb_coord_t *coord, const mac_radio_ops_t *radio,
              nv_log_t *store, uint64_t factory_ieee, uint32_t random_seed, uint8_t reset_reason) {
    *z = (znp_t){0};
    z->p = *platform;
    z->coord = coord;
    z->store = store;
    z->ieee = factory_ieee;
    uint8_t b[NV_LOG_MAX_RECORD];
    if (generic_len(z, NV_EXTADDR, b) == 8) z->ieee = get64(b);   /* restored coordinator identity */
    /* Startup options (Z-Stack semantics), cleared once applied. */
    bool clear_state = false;
    if (generic_len(z, NV_STARTUP_OPTION, b) == 1 && (b[0] & (STARTOPT_CLEAR_CONFIG | STARTOPT_CLEAR_STATE))) {
        uint8_t opt = b[0];
        clear_state = (opt & STARTOPT_CLEAR_STATE) != 0;
        if (opt & STARTOPT_CLEAR_CONFIG) {
            for (uint16_t item = 0; item < 256u; item++) {
                if (item == NV_STARTUP_OPTION || item == 0x60 /* ZNP_HAS_CONFIGURED */ || item == NV_EXTADDR) continue;
                (void)nv_log_delete(store, (uint16_t)(ZNP_NV_GENERIC_BASE + item));
            }
        }
        b[0] = (uint8_t)(opt & ~(STARTOPT_CLEAR_CONFIG | STARTOPT_CLEAR_STATE));
        (void)generic_put(z, NV_STARTUP_OPTION, b, 1);
    }
    const zb_coord_upper_t upper = {
        .device_joined = up_joined, .aps_data = up_aps, .reserve_nwk_counter = 0, .random_key = up_random,
        .key_established = 0, .send_confirm = up_confirm, .device_left = up_left, .ctx = z,
        .channel_changed = up_channel, .gpdf = up_gpdf, .interpan = up_interpan,
    };
    zb_coord_init(coord, radio, &upper, z->ieee, random_seed, 0);
    zb_coord_attach_store(coord, store);
    gp_proxy_init(&z->gp, store);
    z->gp.own_short = 0x0000;   /* the coordinator */
    coord->endpoint_count = 0;   /* as Z-Stack ZNP: the host registers every endpoint */
    if (clear_state) {
        (void)zb_coord_restore(coord);   /* load what exists so that it is deleted */
        zb_coord_forget(coord);
        zb_coord_init(coord, radio, &upper, z->ieee, random_seed, 0);
        zb_coord_attach_store(coord, store);
        coord->endpoint_count = 0;
    }
    z->formed = zb_coord_restore(coord);
    z->dev_state = ZNP_DEV_HOLD;
    z->zdo_seq = (uint8_t)(random_seed >> 24);
    uint8_t ind[6] = {reset_reason, 2, z->p.product, z->p.major, z->p.minor, 0};
    areq(z, S_SYS, 0x80, ind, sizeof ind);
}

void znp_handle(znp_t *z, uint8_t cmd0, uint8_t cmd1, const uint8_t *in, uint8_t n) {
    z->stats.frames_in++;
    uint8_t type = cmd0 & 0xE0u, sub = cmd0 & 0x1Fu;
    if (type != T_SREQ && !(type == T_AREQ && sub == S_SYS && cmd1 == 0x00)) return;   /* only SREQs (+ reset) */
    switch (sub) {
    case S_SYS: handle_sys(z, cmd1, in, n); break;
    case S_AF: handle_af(z, cmd1, in, n); break;
    case S_ZDO: handle_zdo(z, cmd1, in, n); break;
    case S_UTIL: handle_util(z, cmd1, in, n); break;
    case S_APP_CNF: handle_app_cnf(z, cmd1, in, n); break;
    default: rpc_error(z, 1, cmd0, cmd1); break;
    }
    znp_poll(z);
}
