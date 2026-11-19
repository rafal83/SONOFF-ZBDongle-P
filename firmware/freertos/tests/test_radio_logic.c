/* SPDX-License-Identifier: MIT */
#include "frt_test.h"
#include "rf_logic.h"

static const rf_pa_entry_t table[] = {{-20, false, 1}, {0, false, 2}, {5, false, 3}, {14, true, 4}, {20, true, 5}};

int main(void) {
    CHECK(rf_channel_to_mhz(10) == 0 && rf_channel_to_mhz(27) == 0);
    CHECK(rf_channel_to_mhz(11) == 2405 && rf_channel_to_mhz(15) == 2425 && rf_channel_to_mhz(26) == 2480);

    uint8_t el[RF_RX_ELEMENT_MAX] = {0};
    rf_rx_frame_t f;
    /* 5-byte MPDU + 6 appended bytes. */
    el[0] = 11;
    el[1] = 0x02; el[2] = 0x00; el[3] = 0x77; el[4] = 0xAA; el[5] = 0xBB;
    el[6] = (uint8_t)(int8_t)-60;      /* RSSI */
    el[7] = 0x25;                      /* corr 37, crc ok */
    el[8] = 0x78; el[9] = 0x56; el[10] = 0x34; el[11] = 0x12;
    CHECK(rf_rx_parse_element(el, sizeof el, &f));
    CHECK(f.length == 5 && f.mpdu[2] == 0x77 && f.rssi == -60 && f.correlation == 37);
    CHECK(f.crc_ok && !f.ignored && f.timestamp == 0x12345678u);
    CHECK(f.lqi == 127);
    el[7] = 0xC0;
    CHECK(rf_rx_parse_element(el, sizeof el, &f) && !f.crc_ok && f.ignored);
    CHECK(!rf_rx_parse_element(el, 11, &f));        /* length byte exceeds capacity */
    el[0] = 8;
    CHECK(!rf_rx_parse_element(el, sizeof el, &f)); /* below minimal MPDU */
    el[0] = (uint8_t)(RF_MAX_MPDU_NO_FCS + RF_RX_APPENDED + 1);
    CHECK(!rf_rx_parse_element(el, sizeof el, &f));
    el[0] = (uint8_t)(RF_MAX_MPDU_NO_FCS + RF_RX_APPENDED);
    CHECK(rf_rx_parse_element(el, sizeof el, &f) && f.length == RF_MAX_MPDU_NO_FCS);
    CHECK(!rf_rx_parse_element(el, 0, &f));
    for (unsigned i = 0; i < 100000; i++) {          /* never reads out of bounds (UBSan) */
        unsigned cap = frt_rand() % (RF_RX_ELEMENT_MAX + 1);
        for (unsigned k = 0; k < cap; k++) el[k] = (uint8_t)frt_rand();
        (void)rf_rx_parse_element(el, cap, &f);
    }
    CHECK(rf_rssi_to_lqi(-100) == 0 && rf_rssi_to_lqi(-30) == 255 && rf_rssi_to_lqi(0) == 255);

    /* Values from TI's NEW_OVERRIDE_SEGMENT(), region code included. */
    CHECK(rf_override_segment(0x20001234u) == 0x00048D1Fu);
    CHECK(rf_override_segment(0x21000040u) == 0x4000102Fu);
    CHECK(rf_override_segment(0x00012340u) == 0x0048D04Fu);

    uint32_t base[4] = {0x000F8883u, 0x00F388D3u, RF_END_OVERRIDE, RF_END_OVERRIDE};
    CHECK(rf_override_attach(base, 4, 0x20001000u));
    CHECK(base[2] == rf_override_segment(0x20001000u));
    CHECK(rf_override_attach(base, 4, 0x20001000u) && base[3] == RF_END_OVERRIDE); /* idempotent */
    CHECK(rf_override_detach(base, 4, 0x20001000u) && base[2] == RF_END_OVERRIDE);
    CHECK(rf_override_detach(base, 4, 0x20001000u));
    uint32_t full[2] = {1, 2};
    CHECK(!rf_override_attach(full, 2, 0x20001000u));
    uint32_t pa[3] = {0x022Bu | (0x7217u << 10), 0x05320703u, RF_END_OVERRIDE};
    CHECK(rf_override_set_power(pa, 3, 0x022Bu, 0x2853u) && pa[0] == (0x022Bu | (0x2853u << 10)));
    CHECK(!rf_override_set_power(pa, 3, 0x002Bu, 1));
    CHECK(rf_pa_lookup(table, 5, 5)->raw == 3);
    CHECK(rf_pa_lookup(table, 5, 13)->raw == 3);
    CHECK(rf_pa_lookup(table, 5, 100)->raw == 5);
    CHECK(rf_pa_lookup(table, 5, -50)->raw == 1);
    CHECK(rf_pa_lookup(table, 0, 0) == 0);
    /* Source matching tables. */
    rf_src_match_t sm;
    rf_src_match_init(&sm);
    bool sm_full;
    uint8_t e = rf_src_match_update(&sm, false, 0x1234, 0x1A62, true, true, &sm_full);
    CHECK(e == 0 && !sm_full && sm.short_list.entry[0] == (0x1234u | (0x1A62u << 16)));
    CHECK(sm.short_list.enable[0] == 1u && sm.short_list.pending[0] == 1u);
    CHECK(rf_src_match_update(&sm, false, 0x1234, 0x1A62, true, true, &sm_full) == 0);   /* idempotent */
    CHECK(rf_src_match_update(&sm, false, 0x9999, 0x1A62, false, true, &sm_full) == RF_SRC_NONE);
    for (unsigned i = 1; i < RF_SRC_SHORT_ENTRIES; i++)
        CHECK(rf_src_match_update(&sm, false, 0x2000 + i, 0x1A62, true, false, &sm_full) == i);
    CHECK(sm.short_list.enable[0] == 1u);              /* RX running: words left to the RF core */
    CHECK(rf_src_match_update(&sm, false, 0x3000, 0x1A62, true, false, &sm_full) == RF_SRC_NONE && sm_full);
    CHECK(sm.overflows == 1);
    CHECK(rf_src_match_update(&sm, false, 0x1234, 0x1A62, false, true, &sm_full) == 0);
    CHECK(sm.short_list.enable[0] == 0 && sm.short_list.pending[0] == 0);
    CHECK(rf_src_match_update(&sm, false, 0x3000, 0x1A62, true, true, &sm_full) == 0);  /* reuses entry 0 */
    e = rf_src_match_update(&sm, true, 0xA4C1380000000001ull, 0, true, true, &sm_full);
    CHECK(e == 0 && sm.ext_list.entry[0] == 0x00000001u && sm.ext_list.entry[1] == 0xA4C13800u);
    CHECK(sm.ext_list.enable[0] == 1u);
    CHECK(rf_src_match_update(&sm, true, 0xA4C1380000000001ull, 0, false, true, &sm_full) == 0);
    CHECK(sm.ext_list.enable[0] == 0 && sm.ext_used == 0);
    PASS("radio_logic");
    return 0;
}
