// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define BMB_RX_TOPIC_CAPACITY 64u

typedef enum {
    BMB_RX_DROP,
    BMB_RX_WAIT,
    BMB_RX_COMPLETE,
} bmb_rx_result_t;

/* Caller-owned context and payload storage, owned by one MQTT task. No locking
 * or allocation is performed. Treat fields as read-only after initialization.
 * capacity includes the trailing NUL; a 64 KiB limit needs 65537 storage bytes.
 * Input byte ranges must be valid and must not overlap the context or storage.
 */
typedef struct {
    char *data;
    size_t capacity;
    size_t received;
    size_t length;
    size_t topic_len;
    char topic[BMB_RX_TOPIC_CAPACITY];
    bool active;
} bmb_rx_t;

/* NULL storage or zero capacity creates a disabled context (feed drops).
 * Storage must remain valid until the context is reinitialized or retired.
 */
void bmb_rx_init(bmb_rx_t *ctx, char *storage, size_t capacity);

/* Discard a partial or complete frame; retain storage and capacity. Call on
 * disconnect or any transport event that invalidates an in-flight message.
 * reset/feed require an initialized context. NULL ctx is harmless.
 */
void bmb_rx_reset(bmb_rx_t *ctx);

/* Lengths are byte counts, not C-string lengths; inputs need no trailing NUL.
 * Offset zero replaces any previous frame and requires a 1..63 byte topic
 * without embedded NULs. Continuations require exactly received as their
 * offset and the original total. topic_len == 0 omits the continuation topic;
 * otherwise a non-NULL, exactly matching topic is required.
 *
 * Zero-length frames/fragments, invalid ranges, and oversize frames DROP and
 * reset the context. WAIT retains an incomplete frame. Only COMPLETE exposes
 * a consumable payload: data[0..length), received == length, data[length] == 0,
 * and a terminated topic. Payload bytes may include NULs; use length to parse.
 * Consume COMPLETE before the next feed/reset/init; no continuation is valid
 * after completion. No partial payload should be passed to a parser.
 */
bmb_rx_result_t bmb_rx_feed(bmb_rx_t *ctx,
                            const char *topic, size_t topic_len,
                            const char *data, size_t data_len,
                            size_t total_data_len, size_t current_data_offset);
