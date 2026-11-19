"""Static linked-image checks for the complete TI ZNP; never communicates with hardware."""
import re
import struct
from image import check, read_elf, read_hex

REQUIRED_SYMBOLS = ["MTVersionString", "MT_SysCommandProcessing", "MT_AfCommandProcessing",
    "MT_ZdoCommandProcessing", "MT_UtilCommandProcessing", "MT_AppCnfCommandProcessing",
    "MT_AfDataConfirm", "MT_AfIncomingMsg", "MT_SysResetInd", "NPITLUART_initializeTransport",
    "ZDApp_NetworkStartEvt", "ZDApp_RestoreNetworkState", "bdb_StartCommissioning", "NVOCMP_loadApiPtrs"]

def audit_ram_sections(elffile, heap_start, heap_end, optimized):
    """Include NOBITS: the Flash/HEX comparison cannot detect RAM overlaps."""
    raw = elffile.read_bytes()
    offset = struct.unpack_from("<I", raw, 32)[0]
    entry_size, count = struct.unpack_from("<HH", raw, 46)
    if entry_size < 40 or offset + entry_size * count > len(raw):
        raise ValueError("Invalid ELF RAM section table")
    checked = 0
    spans = []
    for i in range(count):
        _, _, flags, address, _, size, _, _, _, _ = struct.unpack_from("<10I", raw, offset + i * entry_size)
        if not flags & 2 or not size or not 0x20000000 <= address < 0x20014000:
            continue
        end = address + size
        if end > 0x20014000 or address < heap_end and end > heap_start:
            raise ValueError("Allocated ELF RAM section overlaps the runtime heap or exceeds SRAM")
        if optimized and address < 0x20001800 and (address != 0x2000010c or size != 16):
            raise ValueError("Ordinary globals overlap the reserved ROM MAC workspace")
        if any(address < b and end > a for a, b in spans):
            raise ValueError("Overlapping allocated ELF RAM sections")
        spans.append((address, end)); checked += 1
    if not checked: raise ValueError("Missing allocated ELF RAM sections")
    return checked

def audit(hexfile, elffile, mapfile, revision=20250321, optimized=False, baudrate=115200, auto_baud=False):
    memory = read_hex(hexfile)
    result = check(memory)
    if not result["passed"]:
        raise ValueError("Coordinator recovery configuration failed")
    if read_elf(elffile, sections_only=True) != memory:
        raise ValueError("ELF allocated sections / HEX mismatch")
    if memory[0x57FB5] != 0xC1:
        raise ValueError("Wrong oscillator trim for SONOFF launchpad RF profile")
    text = mapfile.read_text(encoding="utf-8")
    symbols = {name: int(address, 16) for address, name in
               re.findall(r"^([0-9a-fA-F]{8})\s+(\w+)\s*$", text, re.M)}
    required = REQUIRED_SYMBOLS + (["PortNPI_collect", "PortNPI_sendToHost", "PortNPI_pumpTx", "port_nv_cache_get", "port_nv_cache_invalidate", "port_nv_content_get",
        "port_heap_init", "port_heap_alloc", "port_heap_free", "PortTimer_set", "PortTimer_delete",
        "PortEventTimer_start", "PortEventTimer_cleanup", "PortWatchdog_start", "PortWatchdog_progress",
        "WatchdogCC26XX_open", "WatchdogCC26XX_clear"] if optimized else [])
    if optimized:
        required += ["gNWK_INDIRECT_MSG_MAX_PER", "gNWK_INDIRECT_MSG_MAX_ALL", "gNWK_MAX_DATABUFS_TOTAL",
                     "watchdogCC26XXHWAttrs", "Watchdog_count", "ZMacDataReqSec", "MAC_CbackEvent", "port_radio_guard_accept", "port_npi_baudrate", "NVS_count",
                     "port_nv_index_get", "port_nv_index_put", "port_nv_index_invalidate"]
    for name in required:
        if name not in symbols or symbols[name] >= 0x50000:
            raise ValueError("Missing linked Flash symbol: " + name)
    version = bytes(memory[symbols["MTVersionString"] + i] for i in range(9))
    if version[:5] != bytes([2, 1, 2, 7, 1]) or int.from_bytes(version[5:], "little") != revision:
        raise ValueError("Unexpected full ZNP identity")
    regions = {}
    for name in ["FLASH", "FLASH_NV", "FLASH_LAST", "SRAM"]:
        row = re.search(r"^\s*" + name + r"\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s", text, re.M)
        if not row:
            raise ValueError("Missing linker memory region: " + name)
        regions[name] = dict(zip(["origin", "size", "used", "unused"], (int(v, 16) for v in row.groups())))
    if regions["FLASH_NV"]["origin"] != 0x50000 or regions["FLASH_NV"]["size"] != 0x6000:
        raise ValueError("Unexpected NV layout")
    heap_bytes = symbols["heapEnd"] - symbols["heapStart"]
    result["allocated_ram_sections_checked"] = audit_ram_sections(elffile, symbols["heapStart"], symbols["heapEnd"], optimized)
    if heap_bytes < 32768:
        raise ValueError("Less than 32 KiB remain for the TI runtime heap")
    if optimized:
        quota = {"per_child": memory[symbols["gNWK_INDIRECT_MSG_MAX_PER"]],
                 "global": memory[symbols["gNWK_INDIRECT_MSG_MAX_ALL"]],
                 "total_buffers": memory[symbols["gNWK_MAX_DATABUFS_TOTAL"]]}
        if quota != {"per_child": 3, "global": 24, "total_buffers": 72}:
            raise ValueError("Unexpected compiled native NWK indirect quotas")
        result["native_indirect_quotas"] = quota
        if memory[symbols["NVS_count"]] != 1 or "spiCC26X2DMAObjects" in symbols or "Board_shutDownExtFlash" in symbols:
            raise ValueError("Unexpected external LaunchPad flash dependencies in SONOFF image")
        result["internal_nvs_only"] = True
        if not all(name in symbols for name in ["port_broadcast_send", "port_broadcast_receive"]):
            raise ValueError("Missing fixed broadcast queue implementation")
        if any(name in symbols for name in ["nwk_broadcastSend", "nwk_broadcastReceive"]):
            raise ValueError("Native broadcast queue calls were not fully redirected")
        result["broadcast_fifo"] = {"capacity": 10, "allocation": "static", "enqueue_dequeue": "constant_time"}
        required_baud = ["PortAutobaud_poll", "PortAutobaud_valid", "PortBaudCapture_start", "PortBaudCapture_stop", "NPITLUART_setBaud"]
        if not all(name in symbols and symbols[name] < 0x50000 for name in required_baud):
            raise ValueError("Missing linked auto-baud integration")
        enabled_address = symbols["port_autobaud_enabled"]
        linked_enabled = int.from_bytes(bytes(memory[enabled_address + i] for i in range(4)), "little")
        if linked_enabled != int(auto_baud):
            raise ValueError("Linked auto-baud mode does not match the selected profile")
        result["uart_autobaud"] = {"enabled": auto_baud, "rates": [115200, 460800, 1000000], "confirmation": "real_MT_frame_FCS", "hardware_tested": False}
        baud_address = symbols["port_npi_baudrate"]
        linked_baud = int.from_bytes(bytes(memory[baud_address + i] for i in range(4)), "little")
        if linked_baud != baudrate:
            raise ValueError("Linked UART speed does not match the selected profile")
        result["uart_baudrate_checked"] = linked_baud
        reload_address = symbols["watchdogCC26XXHWAttrs"] + 4
        reload_ms = int.from_bytes(bytes(memory[reload_address + i] for i in range(4)), "little")
        if reload_ms != 10000 or memory[symbols["Watchdog_count"]] != 1:
            raise ValueError("Watchdog must have one instance with a 10-second period")
        result["watchdog"] = {"period_ms": reload_ms, "participants": ["npi_task", "stack_task"],
                              "hardware_reset_tested": False}
        # Verified by native 32-bit TLSF tests: 452-byte control, 8-byte pool
        # overhead. The TI bridge aligns both ends of its arena to 8 bytes.
        aligned_span = (symbols["heapEnd"] & ~7) - ((symbols["heapStart"] + 7) & ~7)
        capacity = aligned_span - 452 - 8
        if capacity < 32768:
            raise ValueError("Less than 32 KiB remain after TLSF metadata/alignment")
        result["heap_capacity_before_os_allocations"] = capacity
    result.update(elf_hex_sections_match=True, linked_symbols_checked=required, runtime_heap_span_bytes=heap_bytes,
                  znp_version={"transportrev": version[0], "product": version[1], "majorrel": version[2],
                               "minorrel": version[3], "maintrel": version[4], "revision": int.from_bytes(version[5:], "little")},
                  linker_regions=regions)
    return result
