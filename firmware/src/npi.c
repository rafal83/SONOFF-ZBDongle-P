/* SPDX-License-Identifier: MIT
 * Original transport implementation from the public MT framing contract.
 * Portable implementation of the documented TI MT framing protocol.
 */
#include "npi.h"

_Static_assert(NPI_TX_SLOTS < 255 && NPI_TX_CONTROL_RESERVE < NPI_TX_SLOTS,
               "Invalid fixed TX pool configuration");
_Static_assert(NPI_TX_CONTROL_BURST > 0, "Normal traffic must make progress");
#define NO_SLOT 255u

static uint8_t at(const npi_parser *p, uint16_t offset) {
    return p->wire[(p->head + offset) % NPI_MAX_FRAME];
}
static void discard(npi_parser *p, uint16_t size, uint32_t now, bool noise) {
    p->head = (uint16_t)((p->head + size) % NPI_MAX_FRAME);
    p->count = (uint16_t)(p->count - size);
    p->started_ms = now;
    if (noise) p->resync_discarded += size;
}
static void parse(npi_parser *p, uint32_t now, bool expired) {
    /* Each pass consumes bytes or returns. Work is bounded by the fixed window.
     * A complete valid outer frame owns its payload, including embedded SOF.
     */
    while (p->count) {
        if (at(p, 0) != 0xfe) { discard(p, 1, now, true); continue; }
        if (p->count >= 2 && at(p, 1) > NPI_MAX_PAYLOAD) {
            p->oversized++;
            discard(p, 1, now, true);
            continue;
        }
        if (p->count >= 3 && (at(p, 2) & 0x80u)) {
            p->invalid_type++;
            discard(p, 1, now, true);
            continue;
        }
        uint16_t size = p->count >= 2 ? (uint16_t)(at(p, 1) + 5u) : 5u;
        if (p->count < size) {
            if (!expired) return;
            p->timeouts++;
            discard(p, 1, now, true);
            continue;
        }
        uint8_t checksum = 0;
        for (uint16_t i = 1; i < size - 1u; i++) checksum ^= at(p, i);
        if (checksum != at(p, (uint16_t)(size - 1u))) {
            p->bad_fcs++;
            /* Keep the suffix, including a possible SOF used as the bad FCS. */
            discard(p, 1, now, true);
            continue;
        }
        p->frame.length = at(p, 1);
        p->frame.cmd0 = at(p, 2);
        p->frame.cmd1 = at(p, 3);
        for (uint16_t i = 0; i < p->frame.length; i++) p->frame.payload[i] = at(p, (uint16_t)(i + 4u));
        discard(p, size, now, false);
        p->accepted++;
        if (p->receive) p->receive(p->context, &p->frame);
    }
}
void npi_init(npi_parser *p, npi_receive_fn receive, void *context) {
    *p = (npi_parser){0};
    p->receive = receive;
    p->context = context;
}
void npi_expire(npi_parser *p, uint32_t now) {
    if (p->count && ((uint32_t)(now - p->last_ms) >= NPI_TIMEOUT_MS ||
                     (uint32_t)(now - p->started_ms) >= NPI_FRAME_TIMEOUT_MS))
        parse(p, now, true);
}
void npi_feed(npi_parser *p, uint8_t byte, uint32_t now) {
    npi_expire(p, now);
    if (!p->count) p->started_ms = now;
    /* At this capacity every legal frame is complete and parse() consumes it. */
    if (p->count == NPI_MAX_FRAME) discard(p, 1, now, true);
    p->wire[(p->head + p->count) % NPI_MAX_FRAME] = byte;
    p->count++;
    p->last_ms = now;
    parse(p, now, false);
}
void npi_rx_error(npi_parser *p) {
    p->rx_errors++;
    p->resync_discarded += p->count;
    p->head = p->count = 0;
}
size_t npi_encode(uint8_t *out, size_t capacity, const npi_frame *f) {
    size_t size = (size_t)f->length + 5u;
    if (f->length > NPI_MAX_PAYLOAD || capacity < size) return 0;
    out[0] = 0xfe;
    out[1] = f->length;
    out[2] = f->cmd0;
    out[3] = f->cmd1;
    uint8_t checksum = f->length ^ f->cmd0 ^ f->cmd1;
    for (size_t i = 0; i < f->length; i++) {
        out[4 + i] = f->payload[i];
        checksum ^= f->payload[i];
    }
    out[size - 1] = checksum;
    return size;
}
void npi_tx_init(npi_tx_queue *q) {
    *q = (npi_tx_queue){0};
    q->active = NO_SLOT;
    for (unsigned i = 0; i < 2; i++) q->lane_head[i] = q->lane_tail[i] = NO_SLOT;
    for (uint8_t i = 0; i < NPI_TX_SLOTS; i++) q->next[i] = (uint8_t)(i + 1u);
    q->next[NPI_TX_SLOTS - 1u] = NO_SLOT;
}
bool npi_tx_enqueue_fields(npi_tx_queue *q, uint8_t cmd0, uint8_t cmd1,
                           const uint8_t *payload, uint8_t length, npi_tx_priority priority) {
    if (priority != NPI_TX_NORMAL && priority != NPI_TX_CONTROL) { q->dropped++; return false; }
    if (q->free_head == NO_SLOT || length > NPI_MAX_PAYLOAD || (length && !payload) ||
        (priority == NPI_TX_NORMAL && q->normal_count >= NPI_TX_SLOTS - NPI_TX_CONTROL_RESERVE)) {
        q->dropped++;
        if (priority == NPI_TX_NORMAL) q->dropped_normal++;
        else q->dropped_control++;
        return false;
    }
    uint8_t slot = q->free_head;
    uint8_t *wire = q->bytes[slot];
    wire[0] = 0xfe; wire[1] = length; wire[2] = cmd0; wire[3] = cmd1;
    uint8_t fcs = length ^ cmd0 ^ cmd1;
    for (unsigned i = 0; i < length; i++) { wire[4 + i] = payload[i]; fcs ^= payload[i]; }
    wire[4 + length] = fcs;
    q->length[slot] = (uint16_t)(length + 5u);
    q->free_head = q->next[slot];
    q->next[slot] = NO_SLOT;
    q->priority[slot] = (uint8_t)priority;
    if (q->lane_tail[priority] == NO_SLOT) q->lane_head[priority] = slot;
    else q->next[q->lane_tail[priority]] = slot;
    q->lane_tail[priority] = slot;
    if (priority == NPI_TX_NORMAL) q->normal_count++;
    q->count++;
    if (q->count > q->high_water) q->high_water = q->count;
    return true;
}
bool npi_tx_enqueue_priority(npi_tx_queue *q, const npi_frame *f, npi_tx_priority priority) {
    return npi_tx_enqueue_fields(q, f->cmd0, f->cmd1, f->payload, f->length, priority);
}
bool npi_tx_enqueue(npi_tx_queue *q, const npi_frame *f) {
    bool control = (f->cmd0 & 0xe0u) == 0x60u ||
                   ((f->cmd0 == 0x44u || f->cmd0 == 0x41u) && f->cmd1 == 0x80u);
    return npi_tx_enqueue_priority(q, f, control ? NPI_TX_CONTROL : NPI_TX_NORMAL);
}
bool npi_tx_peek(npi_tx_queue *q, uint8_t *byte) {
    if (!q->count) return false;
    if (q->active == NO_SLOT) {
        bool control = q->lane_head[NPI_TX_CONTROL] != NO_SLOT;
        bool normal = q->lane_head[NPI_TX_NORMAL] != NO_SLOT;
        unsigned lane = control && (!normal || q->control_streak < NPI_TX_CONTROL_BURST)
                      ? NPI_TX_CONTROL : NPI_TX_NORMAL;
        q->active = q->lane_head[lane];
        q->lane_head[lane] = q->next[q->active];
        if (q->lane_head[lane] == NO_SLOT) q->lane_tail[lane] = NO_SLOT;
        q->next[q->active] = NO_SLOT;
        if (lane == NPI_TX_NORMAL) q->control_streak = 0;
        else if (q->control_streak < NPI_TX_CONTROL_BURST) q->control_streak++;
    }
    *byte = q->bytes[q->active][q->offset];
    return true;
}
void npi_tx_consume(npi_tx_queue *q) {
    if (q->active == NO_SLOT) return;
    uint8_t slot = q->active;
    if (++q->offset == q->length[slot]) {
        npi_tx_finish_frame(q);
    }
}
size_t npi_tx_frame(npi_tx_queue *q, const uint8_t **bytes) {
    uint8_t ignored;
    if (!npi_tx_peek(q, &ignored) || q->offset) return 0;
    *bytes = q->bytes[q->active];
    return q->length[q->active];
}
void npi_tx_finish_frame(npi_tx_queue *q) {
    if (q->active == NO_SLOT) return;
    uint8_t slot = q->active;
    q->offset = 0;
    q->active = NO_SLOT;
    if (q->priority[slot] == NPI_TX_NORMAL) q->normal_count--;
    q->next[slot] = q->free_head;
    q->free_head = slot;
    if (--q->count == 0) q->control_streak = 0;
}
