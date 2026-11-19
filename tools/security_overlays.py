"""Source-only security persistence changes; never alter closed TI libraries."""

UPDATE_BODY = r'''
    if (!ZDSecMgrEntries || index >= gZDSECMGR_ENTRY_MAX) return;
    uint16_t size = sizeof(nvDeviceListHdr_t) + gZDSECMGR_ENTRY_MAX * sizeof(ZDSecMgrEntry_t);
    uint8_t *buffer = OsalPort_malloc(size);
    if (!buffer) return;
    /* Read the existing complete record once. Never synthesize missing bytes. */
    if (osal_nv_read(ZCD_NV_APS_LINK_KEY_TABLE, 0, size, buffer) == SUCCESS)
    {
        nvDeviceListHdr_t header;
        ZDSecMgrEntry_t previous;
        uint16_t offset = sizeof(header) + index * sizeof(previous);
        OsalPort_memcpy(&header, buffer, sizeof(header));
        OsalPort_memcpy(&previous, buffer + offset, sizeof(previous));
        bool was_used = previous.ami != INVALID_NODE_ADDR;
        bool is_used = ZDSecMgrEntries[index].ami != INVALID_NODE_ADDR;
        if (header.numRecs <= gZDSECMGR_ENTRY_MAX &&
            !(was_used && !is_used && !header.numRecs) &&
            !(!was_used && is_used && header.numRecs == gZDSECMGR_ENTRY_MAX))
        {
            if (!was_used && is_used) header.numRecs++;
            if (was_used && !is_used) header.numRecs--;
            OsalPort_memcpy(buffer, &header, sizeof(header));
            OsalPort_memcpy(buffer + offset, &ZDSecMgrEntries[index], sizeof(previous));
            /* Header and entry share ONE NVOCMP transaction, including deletion.
             * No second header write and no count increment on ordinary updates. */
            osal_nv_write(ZCD_NV_APS_LINK_KEY_TABLE, size, buffer);
        }
    }
    OsalPort_free(buffer);
'''

RESTORE_BODY = r'''
    nvDeviceListHdr_t header;
    if (!ZDSecMgrEntries) return;
    for (uint16_t index = 0; index < gZDSECMGR_ENTRY_MAX; index++)
    {
        ZDSecMgrEntries[index].ami = INVALID_NODE_ADDR;
        ZDSecMgrEntries[index].keyNvId = SEC_NO_KEY_NV_ID;
    }
    if (osal_nv_read(ZCD_NV_APS_LINK_KEY_TABLE, 0, sizeof(header), &header) == SUCCESS &&
        header.numRecs > 0 && header.numRecs <= gZDSECMGR_ENTRY_MAX)
    {
        APSME_ApsLinkKeyNVEntry_t *key = OsalPort_malloc(sizeof(*key));
        for (uint16_t index = 0; index < gZDSECMGR_ENTRY_MAX; index++)
        {
            ZDSecMgrEntry_t entry;
            if (osal_nv_read(ZCD_NV_APS_LINK_KEY_TABLE,
                sizeof(header) + index * sizeof(entry), sizeof(entry), &entry) != SUCCESS)
            {
                ZDSecMgrEntries[index].ami = INVALID_NODE_ADDR;
                ZDSecMgrEntries[index].keyNvId = SEC_NO_KEY_NV_ID;
                continue;
            }
            if (entry.ami != INVALID_NODE_ADDR)
            {
                bool usable = key && entry.keyNvId < gZDSECMGR_ENTRY_MAX;
                if (usable)
                    usable = osal_nv_read_ex(ZCD_NV_EX_APS_KEY_DATA_TABLE, entry.keyNvId,
                                            0, sizeof(*key), key) == SUCCESS;
                const uint32_t reservation = MAX_APS_FRAMECOUNTER_CHANGES + 1u;
                /* Leave an entire subsequent checkpoint interval available;
                 * do not resume at UINT32_MAX or immediately before wrap. */
                if (usable) usable = key->txFrmCntr <= UINT32_MAX - 2u * reservation;
                if (usable)
                {
                    key->txFrmCntr += reservation;
                    /* Publish counters in RAM ONLY after durable reservation. */
                    usable = osal_nv_write_ex(ZCD_NV_EX_APS_KEY_DATA_TABLE, entry.keyNvId,
                                              sizeof(*key), key) == SUCCESS;
                }
                if (usable)
                {
                    ApsLinkKeyRAMEntry[entry.keyNvId].txFrmCntr = key->txFrmCntr;
                    ApsLinkKeyRAMEntry[entry.keyNvId].rxFrmCntr = key->rxFrmCntr;
                }
                else
                {
                    entry.ami = INVALID_NODE_ADDR;
                    entry.keyNvId = SEC_NO_KEY_NV_ID;
                }
            }
            ZDSecMgrEntries[index] = entry;
        }
        if (key)
        {
            /* Do not retain a local copy of secret material. */
            volatile uint8_t *clear = (volatile uint8_t *)key;
            for (unsigned i = 0; i < sizeof(*key); i++) clear[i] = 0;
            OsalPort_free(key);
        }
    }
    osal_nv_read(ZCD_NV_TRUSTCENTER_ADDR, 0, Z_EXTADDR_LEN, zgApsTrustCenterAddr);
'''

def apply(text, function_span, replace_body):
    for name, body in [('ZDSecMgrUpdateNV', UPDATE_BODY), ('ZDSecMgrRestoreFromNV', RESTORE_BODY)]:
        start, end = function_span(text, name)
        original = text[start:end]
        old_body = original[original.index('{') + 1:-1]
        text = replace_body(text, name, '#if defined(PORT_EXPERIMENTAL_NEXT)\n' + body +
                            '\n#else\n' + old_body + '\n#endif')
    return text
