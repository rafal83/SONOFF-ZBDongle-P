/* SPDX-License-Identifier: MIT
 * Test-only simulated radio for the Unicorn ARM run of the MAC core.
 * Linked only into build/freertos/arm-test/ ELF, never into the firmware.
 */
#include "mac_core.h"

uint8_t arm_sim_last[MAC_MAX_MPDU];
uint32_t arm_sim_len, arm_sim_count, arm_sim_match_count;
uint64_t arm_sim_match_addr;
uint32_t arm_sim_match_pending;
uint32_t arm_sim_assoc_count;

static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx; (void)ack;
    for (unsigned i = 0; i < len; i++) arm_sim_last[i] = mpdu[i];
    arm_sim_len = len;
    arm_sim_count++;
    return 0;
}
static void sim_match(void *ctx, bool ext, uint64_t address, bool pending) {
    (void)ctx; (void)ext;
    arm_sim_match_addr = address;
    arm_sim_match_pending = pending;
    arm_sim_match_count++;
}
static void sim_assoc(void *ctx, uint64_t device, uint8_t cap, uint8_t lqi) {
    (void)cap; (void)lqi;
    arm_sim_assoc_count++;
    (void)mac_association_response((mac_t *)ctx, device, 0x4E21, 0);
}

void arm_sim_mac_setup(mac_t *m) {
    static const uint8_t epid[8] = {0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD};
    const mac_radio_ops_t radio = {sim_tx, sim_match, 0};
    const mac_upper_t upper = {0, 0, sim_assoc, 0, 0, 0, m};
    mac_init(m, &radio, &upper, 0x00124B0001020304ull);
    mac_start(m, 0x1A62, 15, epid);
    mac_set_association_permit(m, true);
}
