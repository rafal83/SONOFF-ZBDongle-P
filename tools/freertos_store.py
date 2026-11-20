"""Read-only parser of the FreeRTOS firmware's nv_log store inside a full Flash
capture (storage layout 2: 0x34000-0x4FFFF, 2 x 56 KiB; layout 1, before the
migration: 0x48000-0x4FFFF, 2 x 16 KiB), following firmware/freertos/storage/nv_log.c:
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

STORE_BASE, STORE_END = 0x48000, 0x50000          # layout 1 (store-alone files use it too)
BANK_SIZE = (STORE_END - STORE_BASE) // 2
LAYOUTS = {2: (0x34000, 0xE000), 1: (STORE_BASE, BANK_SIZE)}   # layout: (base, bank size)
MAGIC, VERSION = 0x564E5246, 1
ID_NETWORK, ID_COUNTERS = 1, 2
TOMBSTONE = 0x8000


def _hcrc(header4):
    return zlib.crc32(header4, 0x5A5A) & 0xFFFF


def _records(store, base, bank_size=BANK_SIZE):
    recs, off = {}, 16
    while off + 8 <= bank_size:
        h = store[base + off: base + off + 8]
        if h == b"\xff" * 8:
            break
        rid, ln, hc = struct.unpack_from("<HHH", h)
        plen = ln & ~TOMBSTONE & 0xFFFF
        if hc != _hcrc(h[:4]) or plen > 256 or rid >= 512:
            break                                   # torn header: end of the usable log
        end = off + 8 + ((plen + 3) & ~3) + 4
        if end > bank_size:
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


def _best_bank(flash, store_base, bank_size):
    best = None
    for bank in (0, 1):
        base = store_base + bank * bank_size
        if base + 16 > len(flash):
            continue
        magic, version, _, generation, complete = struct.unpack_from("<IHHII", flash, base)
        if magic != MAGIC or version != VERSION or complete != 0:
            continue
        if best is None or 0 < ((generation - best[1]) & 0xFFFFFFFF) < 0x80000000:
            best = (base, generation)
    return best


def layout(flash):
    """Storage layout holding a valid store in a full capture (2, 1), None if none.
    Layout 2 first: after the migration, layout 1's headers are erased."""
    for lay in (2, 1):
        if _best_bank(flash, *LAYOUTS[lay]):
            return lay
    return None


def records(flash, store_base=None, bank_size=None):
    """Latest committed records {id: bytes} of the store, None if no valid bank.
    `flash` is a full capture (layout detected) or a layout-1 store alone
    (store_base=0)."""
    if store_base is None:
        lay = layout(flash)
        if lay is None:
            return None
        store_base, bank_size = LAYOUTS[lay]
    bank_size = bank_size or BANK_SIZE
    best = _best_bank(flash, store_base, bank_size)
    return None if best is None else _records(flash, best[0], bank_size)


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
