"""Generate auditable modifications to TI source files without altering the SDK."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def function_span(text, name):
    pattern = r"^(?:static\s+)?(?:void\s*\*|uint8_t\s*\*|OsalPort_EventHdr\s*\*|void|bool|uint8_t|uint16|uint32_t|NPIMSG_msg_t\s*\*)\s*" + name + r"\([^;{}]*\)\s*\{"
    matches = list(re.finditer(pattern, text, re.M))
    if len(matches) != 1: raise ValueError("Ambiguous TI function: " + name)
    start, pos = matches[0].start(), matches[0].end()
    depth = 1
    # These TI functions contain no braces in quoted strings/comments; fail on
    # unexpected layouts instead of applying an approximate source patch.
    while depth and pos < len(text):
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    if depth: raise ValueError("Unterminated TI function: " + name)
    return start, pos

def replace_body(text, name, body):
    start, end = function_span(text, name)
    opening = text.index("{", start)
    return text[:opening + 1] + "\n" + body + "\n}" + text[end:]

def once(text, old, new):
    if text.count(old) != 1: raise ValueError("TI source context mismatch: " + old[:80])
    return text.replace(old, new, 1)

def generate(sdk, destination):
    destination.mkdir(parents=True, exist_ok=True)
    replacements, report = {}, {}
    def save(relative, text):
        original = sdk / relative
        target = destination / original.name
        target.write_text(text, encoding="utf-8", newline="\n")
        replacements[original] = target
        report[relative] = {"input_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                            "overlay_sha256": hashlib.sha256(target.read_bytes()).hexdigest()}

    relative = "source/ti/zstack/npi/npi_frame_mt.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, '#include "npi_rxbuf.h"', '#include "npi_rxbuf.h"\n#include "port_npi.h"')
    text = replace_body(text, "NPIFrame_initialize", "    PortNPI_initialize(incomingFrameCB);")
    text = replace_body(text, "NPIFrame_collectFrameData", "    PortNPI_collect();")
    # The old globals held a partially allocated heap message while waiting for
    # an FCS indefinitely. The new parser owns a fixed, bounded window instead.
    text = once(text, "static npiIncomingFrameCBack_t incomingFrameCBFunc = NULL;", "")
    a = text.index("static uint8_t state = 0;")
    b = text.index("static uint8_t tempDataLen;", a) + len("static uint8_t tempDataLen;")
    text = text[:a] + text[b:]
    save(relative, text)

    relative = "source/ti/zstack/npi/npi_task.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, '#include "npi_client.h"', '#include "npi_client.h"\n#include "port_npi.h"\n#include "port_watchdog.h"')
    text = replace_body(text, "NPITask_sendToHost", "    PortNPI_sendToHost(pMsg);")
    start, end = function_span(text, "NPITask_processStackMsg")
    original = text[start:end]
    prefix = original[:original.index("    pNPIMsg = NPIFrame_frameMsg(pMsg);")]
    prefix = prefix.replace("    NPIMSG_msg_t *pNPIMsg;", "")
    text = text[:start] + prefix + "    PortNPI_sendToHost(pMsg);\n}" + text[end:]
    text = replace_body(text, "NPITask_ProcessTXQ", "    PortNPI_pumpTx();")
    text = once(text, "Semaphore_pend(npiSemHandle, BIOS_WAIT_FOREVER);",
                "Semaphore_pend(npiSemHandle, PortWatchdog_waitTicks(PortNPI_waitTicks()));\n        PortWatchdog_progress(PORT_WATCHDOG_NPI);\n        PortNPI_collect();\n        PortNPI_pumpTx();")
    start, end = function_span(text, "NPITask_incomingFrameCB")
    body = text[start:end]
    body = body[:-1] + '''    else
    {
        if (recPtr) OsalPort_free(recPtr);
        if (npiMsgPtr) OsalPort_free(npiMsgPtr);
        OsalPort_msgDeallocate(pFrame);
    }
}'''
    text = text[:start] + body + text[end:]
    save(relative, text)
    relative = "source/ti/zstack/startup/zstackstartup.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, '#include "osal_nv.h"', '#include "osal_nv.h"\n#include "port_watchdog.h"\n#include "port_heap.h"')
    text = once(text, "    while(1)\n    {\n      /* Block here until TIRTOS task gets an event */",
                "    if (!PortWatchdog_start()) port_heap_fault();\n\n    while(1)\n    {\n      /* Block here until TIRTOS task gets an event */")
    text = once(text, "Semaphore_pend(stackSemHandle, BIOS_WAIT_FOREVER);",
                "Semaphore_pend(stackSemHandle, PortWatchdog_waitTicks(BIOS_WAIT_FOREVER));\n      PortWatchdog_progress(PORT_WATCHDOG_STACK);")
    save(relative, text)
    relative = "source/ti/zstack/stack/nwk/nwk_globals.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, "#define NWK_INDIRECT_MSG_MAX_PER    3",
                "#define NWK_INDIRECT_MSG_MAX_PER    PORT_INDIRECT_MAX_PER_CHILD")
    text = once(text, "#define NWK_INDIRECT_MSG_MAX_ALL    \\\n                            (NWK_MAX_DATABUFS_TOTAL - NWK_INDIRECT_MSG_MAX_PER)",
                "#define NWK_INDIRECT_MSG_MAX_ALL    PORT_INDIRECT_MAX_ALL\n"
                '_Static_assert(NWK_INDIRECT_MSG_MAX_PER > 0 && NWK_INDIRECT_MSG_MAX_PER <= NWK_INDIRECT_MSG_MAX_ALL, "Invalid child quota");\n'
                '_Static_assert(NWK_INDIRECT_MSG_MAX_ALL <= NWK_MAX_DATABUFS_TOTAL - 48, "Indirect traffic must leave 48 buffer slots");')
    save(relative, text)
    relative = "source/ti/zstack/zmac/f8w/zmac.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, "  dataReq.msdu.p = OsalPort_malloc(dataReq.msdu.len);",
                "  dataReq.msdu.p = OsalPort_malloc(dataReq.msdu.len);\n"
                "  /* A nonempty MSDU must never reach MAC with a NULL buffer. */\n"
                "  if (dataReq.msdu.len && !dataReq.msdu.p) return MAC_NO_RESOURCES;")
    save(relative, text)
    # Test the actual modified function, not a hand-copied implementation.
    start, end = function_span(text, "ZMacDataReqSec")
    (destination / "zmac_datareq_under_test.inc").write_text(text[start:end] + "\n", encoding="utf-8", newline="\n")

    relative = "source/ti/zstack/zmac/f8w/zmac_cb.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, '#include "zmac.h"', '#include "zmac.h"\n#include "radio_guard.h"')
    guard = '''  /* Validate before any FC/radius read or application callback. */
  if (!pData || pData->hdr.event >= sizeof(zmacCBSizeTable) / sizeof(zmacCBSizeTable[0])) return;
  if (pData->hdr.event == MAC_MCPS_DATA_IND &&
      !port_radio_guard_accept(pData->dataInd.msdu.p, pData->dataInd.msdu.len, _NIB.SecurityLevel))
  {
    /* DATA_IND ownership is transferred by MAC; release exactly once. */
    MAP_mac_msg_deallocate((uint8_t **)&pData);
    return;
  }
'''
    start, end = function_span(text, "MAC_CbackEvent")
    opening = text.index("{", start)
    text = text[:opening + 1] + "\n" + guard + text[opening + 1:]
    save(relative, text)
    (destination / "zmac_rx_guard_under_test.inc").write_text(guard, encoding="utf-8", newline="\n")

    relative = "source/ti/zstack/npi/npi_tl.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, "len = transportWrite(npiTxBufLen);",
                "len = transportWrite(npiTxBufLen);\n    if (!len) npiTxActive = FALSE; /* Allow a queued retry after driver rejection. */")
    save(relative, text)
    relative = "source/ti/zstack/npi/npi_tl_uart.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, '#include "npi_tl_uart.h"', '#include "npi_tl_uart.h"\n#include "port_npi.h"\n#include "port_autobaud.h"')
    text = once(text, "static UART2_Handle uartHandle;", "static UART2_Handle uartHandle;\nstatic uint32_t port_uart_rate;\nstatic volatile bool port_uart_reconfiguring;")
    text = once(text, "    params.baudRate = NPI_UART_BR;", "    if (!port_uart_rate) port_uart_rate = port_npi_baudrate;\n    params.baudRate = port_uart_rate;")
    text = once(text, "    params.eventMask |= UART2_EVENT_TX_FINISHED;",
                "    params.eventMask |= UART2_EVENT_TX_FINISHED | UART2_EVENT_FRAMING | UART2_EVENT_PARITY | UART2_EVENT_BREAK | UART2_EVENT_OVERRUN;")
    start, end = function_span(text, "NPITLUART_initializeTransport")
    body = once(text[start:end], "    NPITLUART_readTransport();", "    if (uartHandle) NPITLUART_readTransport();\n    PortAutobaud_start();")
    text = text[:start] + body + text[end:]
    start, end = function_span(text, "NPITLUART_readCallBack")
    opening = text.index("{", start)
    text = text[:opening + 1] + '''
    /* UART2_readCancel synchronously invokes this callback. Do not rearm or
     * forward the cancelled transfer while replacing its handle. */
    if (port_uart_reconfiguring) return;
    if (status != UART2_STATUS_SUCCESS) { PortAutobaud_error(); size = 0; }
''' + text[opening + 1:]
    start, end = function_span(text, "NPITLUART_eventCallBack")
    opening = text.index("{", start)
    text = text[:opening + 1] + '''
    if (port_uart_reconfiguring) return;
    if (event & (UART2_EVENT_FRAMING | UART2_EVENT_PARITY | UART2_EVENT_BREAK | UART2_EVENT_OVERRUN)) PortAutobaud_error();
''' + text[opening + 1:]
    text = once(text, "    if (event == UART2_EVENT_TX_FINISHED)", "    if (event & UART2_EVENT_TX_FINISHED)")
    start, end = function_span(text, "NPITLUART_writeTransport")
    opening = text.index("{", start)
    text = text[:opening + 1] + "\n    if (!uartHandle || port_uart_reconfiguring) return 0;\n" + text[opening + 1:]
    text += '''
uint32_t NPITLUART_getBaud(void) { return uartHandle ? port_uart_rate : 0u; }
bool NPITLUART_setBaud(uint32_t rate) {
    if (rate != 115200u && rate != 460800u && rate != 1000000u) return false;
    if (rate == port_uart_rate) return uartHandle != NULL;
    uint32_t previous = port_uart_rate;
    port_uart_reconfiguring = true;
    if (uartHandle) { UART2_readCancel(uartHandle); UART2_close(uartHandle); }
    uartHandle = NULL;
    TransportRxLen = 0;
    port_uart_rate = rate;
    port_uart_reconfiguring = false;
    NPITLUART_initializeTransport(TransportRxBuf, TransportTxBuf, npiTransmitCB);
    if (uartHandle) return true;
    /* Restore a working UART if the new open failed. Never leave a stale
     * handle installed and never emit an invented successful MT exchange. */
    port_uart_rate = previous;
    NPITLUART_initializeTransport(TransportRxBuf, TransportTxBuf, npiTransmitCB);
    return false;
}
'''
    save(relative, text)
    functions = ["NPITLUART_initializeTransport", "NPITLUART_readCallBack", "NPITLUART_eventCallBack",
                 "NPITLUART_writeTransport", "NPITLUART_getBaud", "NPITLUART_setBaud"]
    extracted = []
    for name in functions:
        start, end = function_span(text, name)
        extracted.append(text[start:end])
    (destination / "npi_uart_under_test.inc").write_text("\n".join(extracted) + "\n", encoding="utf-8", newline="\n")

    relative = "source/ti/common/nv/nvocmp.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, '#include "nvocmp.h"', '#include "nvocmp.h"\n#include "nv_cache.h"\n#include "nv_index.h"')
    prototype = "static uint8_t    NVOCMP_addItem(NVOCMP_nvHandle_t *pNvHandle, NVOCMP_itemHdr_t *iHdr,"
    text = once(text, prototype,
        '#include "nv_index_transaction.h"\n'
        "static bool port_nv_index_find(NVOCMP_nvHandle_t *, uint8_t, uint16_t, NVOCMP_itemHdr_t *, int8_t, NVOCMP_itemInfo_t *, int8_t *);\n" + prototype)
    definition = r"(static int8_t NVOCMP_findItem\([^;{}]+\)\s*\{)"
    text, count = re.subn(definition,
        r"\1\n    int8_t indexed_result;\n    if (port_nv_index_find(pNvHandle, pg, ofs, pHdr, flag, pInfo, &indexed_result)) return indexed_result;", text)
    if count != 2: raise ValueError("Unexpected NVOCMP lookup variants")
    for name in ["NVOCMP_createItemApi", "NVOCMP_updateItemApi", "NVOCMP_writeItemApi"]:
        start, end = function_span(text, name)
        original = text[start:end]
        signature = original[:original.index("{")]
        implementation = original.replace(name + "(", name + "_impl(", 1)
        wrapper = signature + '''{
    if (len > NVOCMP_MAXLEN) return NVINTF_BADLENGTH;
    if (NVOCMP_failF != NVINTF_SUCCESS) return ''' + name + '''_impl(id, len, pBuf);
    NVOCMP_LOCK();
    port_nv_transaction transaction;
    port_nv_transaction_begin(&transaction, id);
    uint8_t result = ''' + name + '''_impl(id, len, pBuf);
    port_nv_transaction_end(&transaction, id, len, pBuf, result);
    NVOCMP_UNLOCK(result);
}'''
        text = text[:start] + implementation + "\n" + wrapper + text[end:]
    text, count = re.subn(r"(static NVOCMP_compactStatus_t NVOCMP_compact\([^;{}]+\)\s*\{)",
                         r"\1\n    port_nv_index_barrier();", text)
    if count != 2: raise ValueError("Unexpected NVOCMP compaction variants")
    text += '\n#include "nv_index_ti.inc"\n'
    start, end = function_span(text, "NVOCMP_readItemApi")
    body = text[start:end]
    body = once(body, "    NVOCMP_LOCK();", '''    NVOCMP_LOCK();
    if (port_nv_cache_get(id.systemID, id.itemID, id.subID, ofs, len, pBuf))
    {
        NVOCMP_UNLOCK(NVINTF_SUCCESS);
    }''')
    body = once(body, "        err = NVOCMP_readItem(&iHdr, ofs, len, pBuf, false);", '''        err = NVOCMP_readItem(&iHdr, ofs, len, pBuf, false);
        if (!err) port_nv_cache_put(id.systemID, id.itemID, id.subID, ofs, len, pBuf);''')
    text = text[:start] + body + text[end:]
    start, end = function_span(text, "NVOCMP_readContItemApi")
    body = text[start:end]
    body = once(body, "    *pSubId = NVOCMP_INVALIDSUBID;", '''    if (!pSubId) return NVINTF_BADPARAM;
    *pSubId = NVOCMP_INVALIDSUBID;
    if (clen > rlen || coff > rlen - clen) return NVINTF_BADPARAM;''')
    body = once(body, "    NVOCMP_LOCK();", '''    NVOCMP_LOCK();
    if (port_nv_content_get(id.systemID, id.itemID, id.subID, ofs, rlen, coff, clen,
                            cBuf, rBuf, pSubId, &err))
    {
        NVOCMP_UNLOCK(err);
    }''')
    # Replace only the final unlock, not the early cache-hit return above.
    marker = body.rfind("    NVOCMP_UNLOCK(err);")
    body = body[:marker] + '''    port_nv_content_put(id.systemID, id.itemID, id.subID, ofs, rlen, coff, clen,
                        cBuf, rBuf, *pSubId, err);
''' + body[marker:]
    text = text[:start] + body + text[end:]
    text = once(text, "    // Optional CRC integrity check", '''    /* Validate in 32 bits before the legacy 16-bit offset addition. */
    if ((uint32_t)ofs + len > iHdr->len)
        return len > iHdr->len ? NVINTF_BADLENGTH : NVINTF_BADOFFSET;
    // Optional CRC integrity check''')
    for name in ["NVOCMP_write", "NVOCMP_erase"]:
        start, end = function_span(text, name)
        opening = text.index("{", start)
        text = text[:opening + 1] + "\n    port_nv_cache_invalidate(); /* Before any attempted NOR mutation, even a failed one. */" + text[opening + 1:]
        if name == "NVOCMP_erase":
            opening = text.index("{", start)
            text = text[:opening + 1] + "\n    port_nv_index_barrier();" + text[opening + 1:]
    # Compare BEFORE requesting space. Keep TI's flash format, CRC validation,
    # voltage checks, write ordering and recovery implementation unchanged.
    text = once(text, "    iLen = NVOCMP_ITEMHDRLEN + iHdr->len;", '''    if ((wm == NVOCMP_WRITE || wm == NVOCMP_UPDATE) && !NVOCMP_failW && iHdr->len)
    {
        NVOCMP_itemHdr_t previous = *iHdr;
        if (NVOCMP_findItem(pNvHandle, pNvHandle->actPage, pNvHandle->actOffset,
                           &previous, NVOCMP_FINDSTRICT, NULL) == NVINTF_SUCCESS &&
            previous.len == iHdr->len)
        {
            uint8_t compare[32];
            uint16_t offset = 0;
            bool same = true;
            while (offset < previous.len)
            {
                uint16_t count = previous.len - offset;
                if (count > sizeof(compare)) count = sizeof(compare);
                if (offset == 0)
                {
                    /* First read validates the entire persisted item's CRC. */
                    if (NVOCMP_readItem(&previous, 0, count, compare, false) != NVINTF_SUCCESS)
                    { same = false; break; }
                }
                else NVOCMP_read(previous.hpage, previous.hofs - previous.len + offset, compare, count);
                if (memcmp(compare, pBuf + offset, count)) { same = false; break; }
                offset += count;
            }
            if (same)
            {
                iHdr->hofs = 0; /* Caller must NOT invalidate the existing item. */
                port_nv_stats.identical_writes++;
                return NVINTF_SUCCESS;
            }
        }
    }
    iLen = NVOCMP_ITEMHDRLEN + iHdr->len;''')
    save(relative, text)
    relative = "source/ti/ti154stack/common/osal_port/osal_port_posix/osal_port.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    if text.count("#include <rtos_heaposal.h>") != 2:
        raise ValueError("Unexpected TI heap template selection")
    text = text.replace("#include <rtos_heaposal.h>", '#include "port_heap_ti.h"')
    text = once(text, '#include "osal_port.h"', '#include "osal_port.h"\n#include "port_timers.h"\n#include <stddef.h>')
    # Keep every original TaskEntry field offset and its 24-byte ARM stride.
    text = once(text, "    bool conservePower;\n    uint32_t* pEventFlag;",
                "    bool conservePower;\n    uint16_t portTailOffset; /* Reuses former padding. */\n    uint32_t* pEventFlag;")
    text = once(text, "/*static*/ uint8_t taskCnt = 0;", '''/*static*/ uint8_t taskCnt = 0;
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(TaskEntry) == 24 && offsetof(TaskEntry, pEventFlag) == 20 &&
               offsetof(TaskEntry, conservePower) == 16 && offsetof(TaskEntry, qHandle) == 8,
               "Task table legacy ABI changed");
#endif
#include "task_queue_ti.inc"''')
    text = replace_body(text, "OsalPort_registerTask", '''    uint32_t key = OsalPort_enterCS();
    if (taskCnt >= MAX_TASKS) { OsalPort_leaveCS(key); return OsalPort_TASK_NO_TASK; }
    uint8_t id = taskCnt;
    taskTbl[id].taskId = id;
    taskTbl[id].taskHndl = taskHndl;
    taskTbl[id].taskSem = (sem_t*)taskSem;
    taskTbl[id].qHandle = NULL;
    taskTbl[id].portTailOffset = 0;
    taskTbl[id].conservePower = false;
    taskTbl[id].pEventFlag = pEvent;
    taskCnt++;
    OsalPort_leaveCS(key);
    return id;''')
    text = replace_body(text, "OsalPort_msgSend", '''    if (!pMsg) return OsalPort_INVALID_MSG_POINTER;
    uint32_t key = OsalPort_enterCS();
    if (destinationTask >= taskCnt) { OsalPort_leaveCS(key); return OsalPort_INVALID_TASK; }
    OsalPort_msgEnqueue(&taskTbl[destinationTask].qHandle, pMsg);
    OsalPort_setEvent(destinationTask, OsalPort_SYS_EVENT_MSG);
    OsalPort_leaveCS(key);
    return OsalPort_SUCCESS;''')
    text = replace_body(text, "OsalPort_msgReceive", '''    uint32_t key = OsalPort_enterCS();
    if (destinationTask >= taskCnt) { OsalPort_leaveCS(key); return NULL; }
    uint8_t *message = OsalPort_msgDequeue(&taskTbl[destinationTask].qHandle);
    /* Queue state and its wakeup bit are one atomic operation. A producer
     * cannot enqueue between the empty check and clearing that event. */
    if (!taskTbl[destinationTask].qHandle) OsalPort_clearEvent(destinationTask, OsalPort_SYS_EVENT_MSG);
    else OsalPort_setEvent(destinationTask, OsalPort_SYS_EVENT_MSG);
    OsalPort_leaveCS(key);
    return message;''')
    start, end = function_span(text, "OsalPort_msgEnqueue")
    body = once(text[start:end], "    void *list;", "    void *list;\n    TaskEntry *task = port_task_queue(pQ);")
    body = once(body, "            for ( list = *pQ; OsalPort_MSG_NEXT( list ) != NULL; list = OsalPort_MSG_NEXT( list ) );", '''            list = task ? port_task_tail_decode(task->portTailOffset) : NULL;
            /* Foreign/unaligned buffers retain the original safe fallback. */
            if (!list) for (list = *pQ; OsalPort_MSG_NEXT(list) != NULL; list = OsalPort_MSG_NEXT(list));''')
    body = once(body, "    // Re-enable interrupts", "    if (task && pMsg) task->portTailOffset = port_task_tail_encode(pMsg);\n\n    // Re-enable interrupts")
    text = text[:start] + body + text[end:]
    for name, old, new in [
        ("OsalPort_msgDequeue", "      *pQ = OsalPort_MSG_NEXT( pMsg );", "      *pQ = OsalPort_MSG_NEXT( pMsg );\n      TaskEntry *task = port_task_queue(pQ);\n      if (task && !*pQ) task->portTailOffset = 0;"),
        ("OsalPort_msgPush", "    // Push message to head of queue", "    TaskEntry *task = port_task_queue(pQ);\n    if (task && !*pQ) task->portTailOffset = port_task_tail_encode(pMsg);\n\n    // Push message to head of queue"),
        ("OsalPort_msgExtract", "    OsalPort_MSG_NEXT( pMsg ) = NULL;", "    TaskEntry *task = port_task_queue(pQ);\n    if (task && !OsalPort_MSG_NEXT(pMsg)) task->portTailOffset = port_task_tail_encode(pPrev);\n    if (task && !*pQ) task->portTailOffset = 0;\n    OsalPort_MSG_NEXT( pMsg ) = NULL;")]:
        start, end = function_span(text, name)
        text = text[:start] + once(text[start:end], old, new) + text[end:]
    start, end = function_span(text, "OsalPort_msgFindDequeue")
    body = text[start:end]
    begin = body.index("                if(pPrev == NULL)")
    finish = body.index("                break;", begin)
    body = body[:begin] + "                OsalPort_msgExtract(&taskTbl[taskIdx].qHandle, pHdr, pPrev);\n" + body[finish:]
    text = text[:start] + body + text[end:]
    text = replace_body(text, "OsalPort_msgEnqueueMax", '''    if (!pQ || !pMsg || !max) return 0;
    uint32_t key = OsalPort_enterCS();
    uint16_t count = 0;
    void *message = *pQ;
    while (message && count < max) { count++; message = OsalPort_MSG_NEXT(message); }
    uint8_t accepted = count < max;
    if (accepted) OsalPort_msgEnqueue(pQ, pMsg);
    OsalPort_leaveCS(key);
    return accepted;''')
    text = replace_body(text, "OsalPort_setTimer", "    return PortTimer_set(ticks, cback, arg, pClockHandle);")
    text = replace_body(text, "OsalPort_deleteTimer", "    return PortTimer_delete(pClockHandle);")
    text = replace_body(text, "OsalPort_stopTimer", "    PortTimer_stop(pClockHandle);")
    save(relative, text)
    table = re.search(r"typedef struct\s*\{\s*uint8_t taskId;.*?\} TaskEntry;", text, re.S)
    if not table: raise ValueError("Missing private OSAL task table type")
    extracted = ["#define MAX_TASKS 15", table.group(), "static TaskEntry taskTbl[MAX_TASKS];\nstatic uint8_t taskCnt;", '#include "task_queue_ti.inc"']
    for name in ["OsalPort_registerTask", "OsalPort_msgEnqueue", "OsalPort_msgDequeue", "OsalPort_msgSend",
                 "OsalPort_msgReceive", "OsalPort_msgPush", "OsalPort_msgExtract", "OsalPort_msgFindDequeue", "OsalPort_msgEnqueueMax"]:
        start, end = function_span(text, name)
        extracted.append(text[start:end])
    (destination / "osal_queue_under_test.inc").write_text("\n".join(extracted) + "\n", encoding="utf-8", newline="\n")
    relative = "source/ti/ti154stack/common/osal_port/osal_port_posix/osal_port_timers.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, '#include "osal_port_timers.h"', '#include "osal_port_timers.h"\n#include "port_timers.h"')
    for name, body in {
        "OsalPortTimers_startTimer": "return PortEventTimer_start(taskId, eventId, timeout, false);",
        "OsalPortTimers_startReloadTimer": "return PortEventTimer_start(taskId, eventId, timeout, true);",
        "OsalPortTimers_stopTimer": "return PortEventTimer_stop(taskId, eventId);",
        "OsalPortTimers_getTimerTimeout": "return PortEventTimer_timeout(taskId, eventId);",
        "OsalPortTimers_cleanUpTimers": "PortEventTimer_cleanup();",
        "OsalPortTimers_registerCleanupEvent": "PortEventTimer_register(taskID, eventID);",
    }.items(): text = replace_body(text, name, "    " + body)
    # No unreachable heap-backed implementation is retained in this variant.
    text = text[:text.index("/***** Private function definitions *****/", text.index("void OsalPortTimers_registerCleanupEvent"))]
    save(relative, text)
    relative = "workspace/znp_CC1352P_2_LAUNCHXL_tirtos7_ticlang/default/syscfg/ti_drivers_config.c"
    text = (sdk / relative).read_text(encoding="utf-8")
    text = once(text, "    Board_shutDownExtFlash();", "    /* SONOFF has no external SPI flash to initialize or shut down. */")
    save(relative, text)
    relative = "source/ti/zstack/boards/cc13x2_cc26x2/cc13x2_cc26x2_tirtos7_ticlang.cmd"
    text = (sdk / relative).read_text(encoding="utf-8")
    # Explicitly exclude the ROM MAC's low SRAM from ordinary allocation.
    text = once(text, "SRAM (RWX) : origin = RAM_BASE, length = RAM_SIZE",
                "ROM_SRAM (RWX) : origin = RAM_BASE, length = 0x1800\n    SRAM (RWX) : origin = RAM_BASE + 0x1800, length = RAM_SIZE - 0x1800")
    text = once(text, ".ramVecs: load > 0x20000000, type = NOLOAD", ".ramVecs: load > 0x20001800, type = NOLOAD")
    text = once(text, "\tGROUP > SRAM", '''    /* Fill two unused holes in the sparse DMA control table. Both input
     * sections have fixed sizes in the pinned SDK: 155 and 372 bytes. The
     * final ELF audit rejects overlaps, including NOBITS and the heap. Keep
     * the rest of the original RAM group contiguous and its heap end marker
     * intact; automatic splitting can otherwise place data beyond that mark. */
    .dma_hole_rf 0x20001830 : { *(.data.txPowerTable_2400_pa5_20) }
    .dma_hole_power 0x200018f0 : { *(.data.PowerCC26X2_module) }
\tGROUP > SRAM''')
    save(relative, text)
    from security_overlays import apply as security_overlay
    relative = "source/ti/zstack/stack/zdo/zd_sec_mgr.c"
    text = security_overlay((sdk / relative).read_text(encoding="utf-8"), function_span, replace_body)
    save(relative, text)
    extracted = []
    for name in ["ZDSecMgrUpdateNV", "ZDSecMgrRestoreFromNV"]:
        start, end = function_span(text, name)
        extracted.append(text[start:end])
    (destination / "security_under_test.inc").write_text("\n".join(extracted) + "\n", encoding="utf-8", newline="\n")
    (destination / "overlays.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return replacements, report

def prepare_tlsf(destination):
    upstream = ROOT / "upstream/tlsf"
    lock = json.loads((ROOT / "sources.lock.json").read_text(encoding="utf-8"))["tlsf"]
    head = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
    if head != lock["commit"]: raise ValueError("TLSF commit mismatch")
    subprocess.run(["git", "-C", str(upstream), "diff", "--exit-code", "HEAD", "--", "tlsf.c", "tlsf.h"], check=True)
    destination.mkdir(parents=True, exist_ok=True)
    source = (upstream / "tlsf.c").read_text(encoding="utf-8")
    source = once(source, '#include "tlsf.h"', '''#include "tlsf.h"
#include "port_heap.h"
/* No stdio state on the MCU; assertions remain active and fail-stop. */
#define printf(...) ((void)0)
#define tlsf_assert(condition) ((condition) ? (void)0 : port_heap_fault())''')
    source = once(source, "SL_INDEX_COUNT_LOG2 = 5,", "SL_INDEX_COUNT_LOG2 = 3,")
    source = once(source, "FL_INDEX_MAX = 32,", "FL_INDEX_MAX = 16,")
    source = once(source, "FL_INDEX_MAX = 30,", "FL_INDEX_MAX = 16,")
    # Preserve 8-byte OSAL alignment when realloc needs to move a block.
    if source.count("p = tlsf_malloc(tlsf, size);") != 2: raise ValueError("TLSF realloc source mismatch")
    source = source.replace("p = tlsf_malloc(tlsf, size);", "p = tlsf_memalign(tlsf, 8, size);")
    # Do not link a printf implementation solely for an unused debug walker.
    source = once(source, 'printf("\\t%p %s size: %x (%p)\\n", ptr, used ? "used" : "free", (unsigned int)size, block_from_ptr(ptr));',
                  "(void)ptr; (void)size; (void)used;")
    (destination / "tlsf.c").write_text(source, encoding="utf-8", newline="\n")
    (destination / "tlsf.h").write_bytes((upstream / "tlsf.h").read_bytes())
    return {"commit": head, "source_sha256": hashlib.sha256((upstream / "tlsf.c").read_bytes()).hexdigest(),
            "adapted_sha256": hashlib.sha256((destination / "tlsf.c").read_bytes()).hexdigest(),
            "sl_bits": 3, "max_block_bits": 16, "realloc_alignment": 8}
