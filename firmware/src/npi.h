/* SPDX-License-Identifier: MIT */
#ifndef PORT_NPI_H
#define PORT_NPI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NPI_MAX_PAYLOAD 250u
#define NPI_MAX_FRAME (NPI_MAX_PAYLOAD + 5u)
#define NPI_TIMEOUT_MS 100u
#define NPI_FRAME_TIMEOUT_MS 250u
#define NPI_TX_SLOTS 8u
#define NPI_TX_CONTROL_RESERVE 2u
#define NPI_TX_CONTROL_BURST 4u

typedef struct {
    uint8_t cmd0, cmd1, length, payload[NPI_MAX_PAYLOAD];
} npi_frame;
typedef void (*npi_receive_fn)(void *context, const npi_frame *frame);
typedef struct {
    npi_frame frame;
    uint8_t wire[NPI_MAX_FRAME];
    uint16_t head, count;
    uint32_t started_ms, last_ms, accepted, bad_fcs, oversized, timeouts;
    uint32_t invalid_type, resync_discarded, rx_errors;
    npi_receive_fn receive;
    void *context;
} npi_parser;

void npi_init(npi_parser *parser, npi_receive_fn receive, void *context);
void npi_expire(npi_parser *parser, uint32_t now_ms);
void npi_feed(npi_parser *parser, uint8_t byte, uint32_t now_ms);
/* Abort a damaged UART frame. All parser calls share one execution context. */
void npi_rx_error(npi_parser *parser);
size_t npi_encode(uint8_t *out, size_t capacity, const npi_frame *frame);

typedef enum { NPI_TX_NORMAL = 0, NPI_TX_CONTROL = 1 } npi_tx_priority;

/* Single-context pool and queues. Caller serializes access, including peek.
 * Frames are never interleaved or evicted. Normal traffic cannot consume the
 * control reserve. At most CONTROL_BURST control frames precede a pending normal
 * frame. An already selected/transmitting frame always finishes first.
 */
typedef struct {
    uint8_t bytes[NPI_TX_SLOTS][NPI_MAX_FRAME];
    uint16_t length[NPI_TX_SLOTS], offset;
    uint8_t next[NPI_TX_SLOTS], priority[NPI_TX_SLOTS];
    uint8_t lane_head[2], lane_tail[2], free_head, active;
    uint8_t count, normal_count, high_water, control_streak;
    uint32_t dropped, dropped_normal, dropped_control;
} npi_tx_queue;
void npi_tx_init(npi_tx_queue *queue);
bool npi_tx_enqueue_priority(npi_tx_queue *queue, const npi_frame *frame, npi_tx_priority priority);
/* MT SRSP, AF dataConfirm and SYS resetInd use CONTROL; other frames use NORMAL. */
bool npi_tx_enqueue(npi_tx_queue *queue, const npi_frame *frame);
/* Avoid a second payload-sized temporary on small RTOS task stacks. */
bool npi_tx_enqueue_fields(npi_tx_queue *queue, uint8_t cmd0, uint8_t cmd1,
                           const uint8_t *payload, uint8_t length, npi_tx_priority priority);
/* Select a complete frame and finish it only after the transport copied it. */
size_t npi_tx_frame(npi_tx_queue *queue, const uint8_t **bytes);
void npi_tx_finish_frame(npi_tx_queue *queue);
bool npi_tx_peek(npi_tx_queue *queue, uint8_t *byte);
void npi_tx_consume(npi_tx_queue *queue);
#endif
