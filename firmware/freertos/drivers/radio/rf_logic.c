/* SPDX-License-Identifier: MIT */
#include "rf_logic.h"

uint16_t rf_channel_to_mhz(uint8_t channel) {
    if (channel < RF_CHANNEL_MIN || channel > RF_CHANNEL_MAX) return 0;
    return (uint16_t)(2405u + 5u * (channel - RF_CHANNEL_MIN));
}

uint8_t rf_rssi_to_lqi(int8_t rssi) {
    /* Linear mapping of the usable CC26x2 RSSI range (-90..-30 dBm) onto
     * 0..255. This is an approximation for diagnostics, not the Z-Stack
     * LQI formula. */
    if (rssi <= -90) return 0;
    if (rssi >= -30) return 255;
    return (uint8_t)(((int)rssi + 90) * 255 / 60);
}

bool rf_rx_parse_element(const uint8_t *element, size_t capacity, rf_rx_frame_t *out) {
    if (capacity < 1) return false;
    uint8_t total = element[0];
    /* Must hold at least the 3-byte minimal MPDU (ACK without FCS) plus the
     * appended status bytes, and fit in the entry buffer. */
    if (total < 3u + RF_RX_APPENDED || (size_t)total + 1u > capacity) return false;
    uint8_t length = (uint8_t)(total - RF_RX_APPENDED);
    if (length > RF_MAX_MPDU_NO_FCS) return false;
    const uint8_t *p = element + 1;
    for (uint8_t i = 0; i < length; i++) out->mpdu[i] = p[i];
    out->length = length;
    out->rssi = (int8_t)p[length];
    uint8_t status = p[length + 1u];
    out->correlation = status & 0x3Fu;
    out->ignored = (status & 0x40u) != 0;
    out->crc_ok = (status & 0x80u) == 0;
    out->timestamp = (uint32_t)p[length + 2u] | ((uint32_t)p[length + 3u] << 8) |
                     ((uint32_t)p[length + 4u] << 16) | ((uint32_t)p[length + 5u] << 24);
    out->lqi = rf_rssi_to_lqi(out->rssi);
    return true;
}

uint32_t rf_override_segment(uint32_t address) {
    /* Same encoding as NEW_OVERRIDE_SEGMENT() in TI rf_mailbox.h, INCLUDING
     * the memory-region code in bits 7..4 (0x20 SRAM = 1): without it the RF
     * core rejects CMD_RADIO_SETUP_PA with ERROR_PAR as soon as the 20 dBm
     * segment is chained (found on the dongle, 18 November). */
    uint32_t top = address >> 24, region;
    switch (top) {
    case 0x20: region = 0x1; break;
    case 0x21: region = 0x2; break;
    case 0xA0: region = 0x3; break;
    case 0x00: region = 0x4; break;
    case 0x10: region = 0x5; break;
    case 0x11: region = 0x6; break;
    case 0x40: region = 0x7; break;
    case 0x50: region = 0x8; break;
    default: region = 0x9; break;   /* illegal range, as TI */
    }
    return ((address & 0x03FFFFFCu) << 6) | 0x000Fu | (region << 4);
}

bool rf_override_attach(uint32_t *base, size_t base_len, uint32_t segment_address) {
    uint32_t link = rf_override_segment(segment_address);
    for (size_t i = 0; i < base_len; i++) {
        if (base[i] == link) return true;
        if (base[i] == RF_END_OVERRIDE) { base[i] = link; return true; }
    }
    return false;
}

bool rf_override_detach(uint32_t *base, size_t base_len, uint32_t segment_address) {
    uint32_t link = rf_override_segment(segment_address);
    for (size_t i = 0; i < base_len; i++) {
        if (base[i] == RF_END_OVERRIDE) return true;
        if (base[i] == link) { base[i] = RF_END_OVERRIDE; return true; }
    }
    return false;
}

bool rf_override_set_power(uint32_t *list, size_t len, uint32_t pattern, uint32_t raw) {
    /* TX power overrides keep the command in bits 9..0 and the gain in 31..10. */
    for (size_t i = 0; i < len && list[i] != RF_END_OVERRIDE; i++) {
        if ((list[i] & 0x3FFu) == pattern) {
            list[i] = pattern | (raw << 10);
            return true;
        }
    }
    return false;
}

const rf_pa_entry_t *rf_pa_lookup(const rf_pa_entry_t *table, size_t count, int8_t dbm) {
    const rf_pa_entry_t *best = 0;
    for (size_t i = 0; i < count; i++) {
        if (table[i].dbm <= dbm && (!best || table[i].dbm > best->dbm)) best = &table[i];
    }
    if (!best && count) {
        best = &table[0];
        for (size_t i = 1; i < count; i++) if (table[i].dbm < best->dbm) best = &table[i];
    }
    return best;
}

void rf_src_match_init(rf_src_match_t *t) { *t = (rf_src_match_t){0}; }

uint8_t rf_src_match_update(rf_src_match_t *t, bool ext, uint64_t address, uint16_t pan_id,
                            bool pending, bool direct, bool *full) {
    unsigned n = ext ? RF_SRC_EXT_ENTRIES : RF_SRC_SHORT_ENTRIES;
    uint32_t *used = ext ? &t->ext_used : &t->short_used;
    uint32_t *enable = ext ? t->ext_list.enable : t->short_list.enable;
    uint32_t *pend = ext ? t->ext_list.pending : t->short_list.pending;
    if (full) *full = false;
    unsigned found = RF_SRC_NONE, free_entry = RF_SRC_NONE;
    for (unsigned i = 0; i < n; i++) {
        if (!(*used & (1u << i))) { if (free_entry == RF_SRC_NONE) free_entry = i; continue; }
        bool same = ext ? (t->ext_list.entry[2 * i] == (uint32_t)address &&
                           t->ext_list.entry[2 * i + 1] == (uint32_t)(address >> 32))
                        : t->short_list.entry[i] == ((uint32_t)(uint16_t)address | ((uint32_t)pan_id << 16));
        if (same) { found = i; break; }
    }
    if (!pending) {
        if (found == RF_SRC_NONE) return RF_SRC_NONE;
        *used &= ~(1u << found);
        if (direct) { enable[0] &= ~(1u << found); pend[0] &= ~(1u << found); }
        return (uint8_t)found;
    }
    if (found == RF_SRC_NONE) {
        if (free_entry == RF_SRC_NONE) {
            t->overflows++;
            if (full) *full = true;
            return RF_SRC_NONE;
        }
        found = free_entry;
        /* Address written while the entry is disabled. */
        if (ext) {
            t->ext_list.entry[2 * found] = (uint32_t)address;
            t->ext_list.entry[2 * found + 1] = (uint32_t)(address >> 32);
        } else {
            t->short_list.entry[found] = (uint32_t)(uint16_t)address | ((uint32_t)pan_id << 16);
        }
        *used |= 1u << found;
    }
    if (direct) { enable[0] |= 1u << found; pend[0] |= 1u << found; }
    return (uint8_t)found;
}
