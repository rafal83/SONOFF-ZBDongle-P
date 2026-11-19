"""Read-only parser of the FreeRTOS firmware's nv_log store (0x48000-0x4FFFF)
inside a full Flash capture, following firmware/freertos/storage/nv_log.c:
two banks with a 16-byte header {magic "FRNV", version 1, generation,
complete}; records {id u16, len u16, hcrc u16, 0xFFFF, data padded to 4,
crc32}. The highest complete generation is authoritative; a record counts
only when both CRCs match; a tombstone (len bit 15) deletes an id.

Used to check, BEFORE a rollback to Z-Stack, how far the FreeRTOS firmware
has reserved its outgoing NWK frame counter (record 2), so that the backup
restored on Z-Stack never brings it back below a value already used.
"""
import struct
import zlib

STORE_BASE, STORE_END = 0x48000, 0x50000
BANK_SIZE = (STORE_END - STORE_BASE) // 2
MAGIC, VERSION = 0x564E5246, 1
ID_NETWORK, ID_COUNTERS = 1, 2
TOMBSTONE = 0x8000


def _hcrc(header4):
    return zlib.crc32(header4, 0x5A5A) & 0xFFFF


def _records(store, base):
    recs, off = {}, 16
    while off + 8 <= BANK_SIZE:
        h = store[base + off: base + off + 8]
        if h == b"\xff" * 8:
            break
        rid, ln, hc = struct.unpack_from("<HHH", h)
        plen = ln & ~TOMBSTONE & 0xFFFF
        if hc != _hcrc(h[:4]) or plen > 256 or rid >= 512:
            break                                   # torn header: end of the usable log
        end = off + 8 + ((plen + 3) & ~3) + 4
        if end > BANK_SIZE:
            break
        data = store[base + off + 8: base + off + 8 + plen]
        trailer = struct.unpack_from("<I", store, base + end - 4)[0]
        if trailer == zlib.crc32(data, zlib.crc32(h[:4])):
            if ln & TOMBSTONE:
                recs.pop(rid, None)
            else:
                recs[rid] = bytes(data)
        off = end
    return recs


def records(flash, store_base=STORE_BASE):
    """Latest committed records {id: bytes} of the store, None if no valid bank.
    `flash` is a full capture (store at store_base) or the store alone (store_base=0)."""
    best = None
    for bank in (0, 1):
        base = store_base + bank * BANK_SIZE
        magic, version, _, generation, complete = struct.unpack_from("<IHHII", flash, base)
        if magic != MAGIC or version != VERSION or complete != 0:
            continue
        if best is None or 0 < ((generation - best[1]) & 0xFFFFFFFF) < 0x80000000:
            best = (base, generation)
    return None if best is None else _records(flash, best[0])


def counters(recs):
    """(nwk_reserved, aps_reserved) of the counters record, None if absent/malformed."""
    if not recs or ID_COUNTERS not in recs:
        return None
    b = recs[ID_COUNTERS]
    if len(b) == 13 and b[0] == 2:
        return struct.unpack_from("<II", b, 1)
    if len(b) == 8:
        return struct.unpack_from("<II", b, 0)
    return None


def has_network(recs):
    return bool(recs) and ID_NETWORK in recs
