// SPDX-License-Identifier: Apache-2.0
#include "bmb_rx.h"

#include <string.h>

void bmb_rx_reset(bmb_rx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->received = 0;
    ctx->length = 0;
    ctx->topic_len = 0;
    ctx->topic[0] = '\0';
    ctx->active = false;
    if (ctx->data != NULL && ctx->capacity > 0) {
        ctx->data[0] = '\0';
    }
}

void bmb_rx_init(bmb_rx_t *ctx, char *storage, size_t capacity)
{
    if (ctx == NULL) {
        return;
    }
    ctx->data = storage;
    ctx->capacity = capacity;
    bmb_rx_reset(ctx);
}

static bmb_rx_result_t drop(bmb_rx_t *ctx)
{
    bmb_rx_reset(ctx);
    return BMB_RX_DROP;
}

bmb_rx_result_t bmb_rx_feed(bmb_rx_t *ctx,
                            const char *topic, size_t topic_len,
                            const char *data, size_t data_len,
                            size_t total_data_len, size_t current_data_offset)
{
    if (ctx == NULL) {
        return BMB_RX_DROP;
    }
    if (current_data_offset == 0) {
        bmb_rx_reset(ctx);
    }

    /* Subtract only after checking bounds; never form offset + fragment size
     * until the complete range is known to fit inside the declared total.
     */
    if (ctx->data == NULL || ctx->capacity == 0 || data == NULL ||
        data_len == 0 || total_data_len == 0 ||
        total_data_len > ctx->capacity - 1 ||
        current_data_offset > total_data_len ||
        data_len > total_data_len - current_data_offset ||
        topic_len >= BMB_RX_TOPIC_CAPACITY ||
        (topic_len > 0 && topic == NULL)) {
        return drop(ctx);
    }

    if (current_data_offset == 0) {
        if (topic_len == 0 || memchr(topic, '\0', topic_len) != NULL) {
            return drop(ctx);
        }
        memcpy(ctx->topic, topic, topic_len);
        ctx->topic[topic_len] = '\0';
        ctx->topic_len = topic_len;
        ctx->length = total_data_len;
        ctx->active = true;
    } else if (!ctx->active || current_data_offset != ctx->received ||
               total_data_len != ctx->length ||
               (topic_len > 0 &&
                (topic_len != ctx->topic_len ||
                 memcmp(topic, ctx->topic, topic_len) != 0))) {
        return drop(ctx);
    }

    memcpy(ctx->data + current_data_offset, data, data_len);
    ctx->received = current_data_offset + data_len;
    ctx->data[ctx->received] = '\0';
    if (ctx->received == ctx->length) {
        ctx->active = false;
        return BMB_RX_COMPLETE;
    }
    return BMB_RX_WAIT;
}
