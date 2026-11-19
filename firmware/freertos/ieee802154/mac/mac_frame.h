/* SPDX-License-Identifier: MIT
 * IEEE 802.15.4 MAC frame codec (2003/2006 frames as used by Zigbee PRO;
 * 2015 frames without Information Elements). Strictly bounds-checked: the
 * parser never reads past the given length and rejects reserved encodings.
 * MPDU buffers here EXCLUDE the 2-byte FCS (the CC26x2 radio checks and
 * appends it); mac_fcs() is provided for host simulation and tests.
 */
#ifndef MAC_FRAME_H
#define MAC_FRAME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MAC_TYPE_BEACON = 0, MAC_TYPE_DATA = 1, MAC_TYPE_ACK = 2, MAC_TYPE_COMMAND = 3 };
enum { MAC_ADDR_NONE = 0, MAC_ADDR_SHORT = 2, MAC_ADDR_EXT = 3 };
enum {
    MAC_OK = 0,
    MAC_ERR_TRUNCATED = -1,
    MAC_ERR_RESERVED = -2,
    MAC_ERR_UNSUPPORTED = -3,
    MAC_ERR_PARAM = -4,
};
enum {
    MAC_CMD_ASSOC_REQUEST = 0x01,
    MAC_CMD_ASSOC_RESPONSE = 0x02,
    MAC_CMD_DISASSOC_NOTIFY = 0x03,
    MAC_CMD_DATA_REQUEST = 0x04,
    MAC_CMD_PAN_ID_CONFLICT = 0x05,
    MAC_CMD_ORPHAN_NOTIFY = 0x06,
    MAC_CMD_BEACON_REQUEST = 0x07,
    MAC_CMD_COORD_REALIGN = 0x08,
};
#define MAC_BROADCAST_PAN 0xFFFFu
#define MAC_BROADCAST_SHORT 0xFFFFu
#define MAC_MAX_MPDU 125u

typedef struct {
    uint8_t type, version;
    bool security, pending, ack_request, pan_id_compression, seq_suppressed;
    uint8_t seq;
    uint8_t dst_mode, src_mode;
    bool has_dst_pan, has_src_pan;
    uint16_t dst_pan, src_pan;
    uint16_t dst_short, src_short;
    uint64_t dst_ext, src_ext;     /* host integer; little endian on air */
    /* Auxiliary security header (MAC security; unused by Zigbee PRO). */
    uint8_t sec_level, key_id_mode, key_index;
    bool frame_counter_suppressed;
    uint32_t frame_counter;
    uint64_t key_source;
    size_t header_len;             /* MHR length including aux security header */
    const uint8_t *payload;
    size_t payload_len;
} mac_frame_t;

int mac_frame_parse(const uint8_t *mpdu, size_t len, mac_frame_t *out);
/* Encode the MHR described by `f` (addressing/PAN presence derived from
 * modes + pan_id_compression; aux security header not supported) followed by
 * the payload. Returns the MPDU length or 0 on error. */
size_t mac_frame_build(const mac_frame_t *f, const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t cap);
uint16_t mac_fcs(const uint8_t *data, size_t len);

/* Zigbee PRO beacon (802.15.4 beacon + 15-byte Zigbee beacon payload). */
typedef struct {
    uint16_t pan_id, short_addr;
    uint8_t seq;
    bool association_permit, router_capacity, end_device_capacity;
    uint8_t device_depth, update_id;
    uint8_t stack_profile, protocol_version;
    uint8_t ext_pan_id[8];         /* little endian, as on air */
} mac_zigbee_beacon_t;
size_t mac_build_zigbee_beacon(const mac_zigbee_beacon_t *b, uint8_t *out, size_t cap);
int mac_parse_zigbee_beacon(const mac_frame_t *frame, mac_zigbee_beacon_t *out);

/* Association response command (coordinator -> device, indirect). */
size_t mac_build_assoc_response(uint8_t seq, uint16_t pan_id, uint64_t dst_ext, uint64_t src_ext,
                                uint16_t short_addr, uint8_t status, uint8_t *out, size_t cap);
#endif
