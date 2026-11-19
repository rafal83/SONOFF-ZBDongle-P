/* SPDX-License-Identifier: MIT
 * Bounded little-endian writer/reader. Overflow is sticky: once a write or
 * read would exceed the buffer, `ok` becomes false and nothing else moves.
 */
#ifndef FRT_LE_BUF_H
#define FRT_LE_BUF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *p; size_t cap, len; bool ok; } le_writer_t;
typedef struct { const uint8_t *p; size_t len, pos; bool ok; } le_reader_t;

static inline le_writer_t le_writer(uint8_t *p, size_t cap) { return (le_writer_t){p, cap, 0, true}; }
static inline bool le_room(le_writer_t *w, size_t n) {
    if (!w->ok || n > w->cap - w->len) { w->ok = false; return false; }
    return true;
}
static inline void le_u8(le_writer_t *w, uint8_t v) { if (le_room(w, 1)) w->p[w->len++] = v; }
static inline void le_u16(le_writer_t *w, uint16_t v) {
    if (le_room(w, 2)) { w->p[w->len++] = (uint8_t)v; w->p[w->len++] = (uint8_t)(v >> 8); }
}
static inline void le_u32(le_writer_t *w, uint32_t v) {
    if (le_room(w, 4)) for (unsigned i = 0; i < 4; i++) w->p[w->len++] = (uint8_t)(v >> (8u * i));
}
static inline void le_bytes(le_writer_t *w, const void *src, size_t n) {
    if (le_room(w, n)) { const uint8_t *s = src; for (size_t i = 0; i < n; i++) w->p[w->len++] = s[i]; }
}

static inline le_reader_t le_reader(const uint8_t *p, size_t len) { return (le_reader_t){p, len, 0, true}; }
static inline bool le_have(le_reader_t *r, size_t n) {
    if (!r->ok || n > r->len - r->pos) { r->ok = false; return false; }
    return true;
}
static inline uint8_t le_get_u8(le_reader_t *r) { return le_have(r, 1) ? r->p[r->pos++] : 0; }
static inline uint16_t le_get_u16(le_reader_t *r) {
    if (!le_have(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[r->pos] | (r->p[r->pos + 1] << 8));
    r->pos += 2;
    return v;
}
static inline uint32_t le_get_u32(le_reader_t *r) {
    if (!le_have(r, 4)) return 0;
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) v |= (uint32_t)r->p[r->pos + i] << (8u * i);
    r->pos += 4;
    return v;
}
static inline uint64_t le_get_u64(le_reader_t *r) {
    if (!le_have(r, 8)) return 0;
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)r->p[r->pos + i] << (8u * i);
    r->pos += 8;
    return v;
}
static inline const uint8_t *le_get_bytes(le_reader_t *r, size_t n) {
    if (!le_have(r, n)) return 0;
    const uint8_t *v = r->p + r->pos;
    r->pos += n;
    return v;
}
static inline size_t le_remaining(const le_reader_t *r) { return r->ok ? r->len - r->pos : 0; }
#endif
