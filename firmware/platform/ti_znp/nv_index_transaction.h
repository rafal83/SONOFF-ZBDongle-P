/* SPDX-License-Identifier: MIT; included after the NVOCMP private type declarations. */
typedef struct {
    NVOCMP_itemHdr_t previous;
    uint8_t data[64];
    uint32_t generation, barriers;
    port_nv_cache_checkpoint cache_checkpoint;
    uint8_t patterns;
    bool restore, had_previous;
} port_nv_transaction;
static void port_nv_transaction_begin(port_nv_transaction *, NVINTF_itemID_t);
static void port_nv_transaction_end(port_nv_transaction *, NVINTF_itemID_t, uint32_t, const void *, uint8_t);
