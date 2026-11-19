/* SPDX-License-Identifier: MIT */
#include "frt_test.h"
#include "mac_frame.h"

int main(void) {
    /* 802.15.4 FCS = CRC-16/KERMIT; standard check value. */
    CHECK(mac_fcs((const uint8_t *)"123456789", 9) == 0x2189);

    /* Imm-ACK. */
    const uint8_t ack[3] = {0x02, 0x00, 0x5A};
    mac_frame_t f;
    CHECK(mac_frame_parse(ack, 3, &f) == MAC_OK && f.type == MAC_TYPE_ACK && f.seq == 0x5A);
    const uint8_t ack_pending[3] = {0x12, 0x00, 0x5A};
    CHECK(mac_frame_parse(ack_pending, 3, &f) == MAC_OK && f.pending);

    /* Data frame, short->short, PAN ID compression, ACK request (typical Zigbee). */
    mac_frame_t d = {0};
    d.type = MAC_TYPE_DATA;
    d.ack_request = true;
    d.pan_id_compression = true;
    d.seq = 0x33;
    d.dst_mode = d.src_mode = MAC_ADDR_SHORT;
    d.dst_pan = 0x1A62;
    d.dst_short = 0x0000;
    d.src_short = 0x4E21;
    const uint8_t payload[4] = {1, 2, 3, 4};
    uint8_t buf[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&d, payload, 4, buf, sizeof buf);
    CHECK(n == 9 + 4);
    CHECK(buf[0] == 0x61 && buf[1] == 0x88);   /* well-known Zigbee FCF 0x8861 */
    CHECK(mac_frame_parse(buf, n, &f) == MAC_OK);
    CHECK(f.type == MAC_TYPE_DATA && f.ack_request && f.has_dst_pan && !f.has_src_pan);
    CHECK(f.dst_pan == 0x1A62 && f.src_pan == 0x1A62 && f.src_short == 0x4E21 && f.payload_len == 4);
    CHECK(f.header_len == 9 && f.payload[3] == 4);
    for (size_t cut = 0; cut < 9; cut++) CHECK(mac_frame_parse(buf, cut, &f) < 0);

    /* Extended addressing, version 1, no compression. */
    d.version = 1;
    d.pan_id_compression = false;
    d.dst_mode = d.src_mode = MAC_ADDR_EXT;
    d.dst_ext = 0x00124B0012345678ull;
    d.src_ext = 0x8877665544332211ull;
    d.src_pan = 0xFFFF;
    n = mac_frame_build(&d, payload, 0, buf, sizeof buf);
    CHECK(n == 3 + 2 + 8 + 2 + 8);
    CHECK(mac_frame_parse(buf, n, &f) == MAC_OK && f.version == 1 && f.dst_ext == d.dst_ext);
    CHECK(f.src_ext == d.src_ext && f.src_pan == 0xFFFF && f.has_src_pan);

    /* Compression without both addresses is reserved in 2003/2006. */
    d.version = 0;
    d.pan_id_compression = true;
    d.src_mode = MAC_ADDR_NONE;
    CHECK(mac_frame_build(&d, 0, 0, buf, sizeof buf) == 0);
    buf[0] = 0x41; buf[1] = 0x08; /* data, compression, dst short, no src */
    CHECK(mac_frame_parse(buf, 7, &f) == MAC_ERR_RESERVED);

    /* Reserved address mode and frame version. */
    uint8_t bad[3] = {0x01, 0x04, 0};
    CHECK(mac_frame_parse(bad, 3, &f) == MAC_ERR_RESERVED);
    bad[1] = 0x30;
    CHECK(mac_frame_parse(bad, 3, &f) == MAC_ERR_RESERVED);
    bad[0] = 0x05; bad[1] = 0x00; /* multipurpose frame type */
    CHECK(mac_frame_parse(bad, 3, &f) == MAC_ERR_UNSUPPORTED);

    /* 2015 frame: seq suppression, dst short only with compression -> no PAN. */
    const uint8_t v2[4] = {0x41, 0x29, 0x34, 0x12};
    CHECK(mac_frame_parse(v2, 4, &f) == MAC_OK && f.version == 2 && f.seq_suppressed);
    CHECK(!f.has_dst_pan && f.dst_short == 0x1234);
    const uint8_t v2ie[3] = {0x01, 0x22, 0x00};
    CHECK(mac_frame_parse(v2ie, 3, &f) == MAC_ERR_UNSUPPORTED);

    /* Beacon request command (broadcast). */
    const uint8_t breq[8] = {0x03, 0x08, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, MAC_CMD_BEACON_REQUEST};
    CHECK(mac_frame_parse(breq, 8, &f) == MAC_OK && f.type == MAC_TYPE_COMMAND);
    CHECK(f.dst_short == MAC_BROADCAST_SHORT && f.payload_len == 1 && f.payload[0] == MAC_CMD_BEACON_REQUEST);

    /* Zigbee beacon round trip. */
    mac_zigbee_beacon_t b = {.pan_id = 0x1A62, .short_addr = 0, .seq = 9, .association_permit = true,
                             .router_capacity = true, .end_device_capacity = true, .device_depth = 0,
                             .update_id = 3, .stack_profile = 2, .protocol_version = 2,
                             .ext_pan_id = {0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD}};
    n = mac_build_zigbee_beacon(&b, buf, sizeof buf);
    CHECK(n == 7 + 19);
    CHECK(buf[7] == 0xFF && buf[8] == 0xCF);       /* BO/SO 15, PAN coord, permit */
    CHECK(buf[11] == 0x00 && buf[12] == 0x22 && buf[13] == 0x84);
    mac_zigbee_beacon_t b2;
    CHECK(mac_frame_parse(buf, n, &f) == MAC_OK && mac_parse_zigbee_beacon(&f, &b2) == MAC_OK);
    CHECK(b2.pan_id == 0x1A62 && b2.association_permit && b2.router_capacity && b2.end_device_capacity);
    CHECK(b2.stack_profile == 2 && b2.protocol_version == 2 && b2.update_id == 3 && b2.ext_pan_id[7] == 0xDD);
    CHECK(mac_parse_zigbee_beacon(&f, &b2) == MAC_OK);
    f.payload_len = 10;
    CHECK(mac_parse_zigbee_beacon(&f, &b2) == MAC_ERR_TRUNCATED);

    /* Association response: ext->ext, compressed, indirect via poll. */
    n = mac_build_assoc_response(0x44, 0x1A62, 0x1111111111111111ull, 0x2222222222222222ull, 0x5A5A, 0, buf, sizeof buf);
    CHECK(n == 3 + 2 + 8 + 8 + 4);
    CHECK(mac_frame_parse(buf, n, &f) == MAC_OK && f.payload[0] == MAC_CMD_ASSOC_RESPONSE);
    CHECK(f.payload[1] == 0x5A && f.payload[2] == 0x5A && f.payload[3] == 0 && f.ack_request);

    /* MAC security header parse (version 1, key id mode 1). */
    const uint8_t sec[] = {0x69, 0x98, 0x01, 0x34, 0x12, 0x00, 0x00, 0x01, 0x00,
                           0x0D, 0x78, 0x56, 0x34, 0x12, 0x07, 0xEE};
    CHECK(mac_frame_parse(sec, sizeof sec, &f) == MAC_OK && f.security && f.sec_level == 5);
    CHECK(f.key_id_mode == 1 && f.frame_counter == 0x12345678u && f.key_index == 7 && f.payload_len == 1);
    CHECK(mac_frame_parse(sec, sizeof sec - 2, &f) == MAC_ERR_TRUNCATED);

    /* Fuzz: random MPDUs never crash and payload stays inside the buffer. */
    for (unsigned i = 0; i < 200000; i++) {
        size_t len = frt_rand() % (MAC_MAX_MPDU + 1);
        for (size_t k = 0; k < len; k++) buf[k] = (uint8_t)frt_rand();
        if (mac_frame_parse(buf, len, &f) == MAC_OK) {
            CHECK(f.header_len <= len && f.payload == buf + f.header_len && f.header_len + f.payload_len == len);
            mac_zigbee_beacon_t z;
            (void)mac_parse_zigbee_beacon(&f, &z);
        }
    }
    PASS("mac_frame");
    return 0;
}
