// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Private transport policy, shared with native tests. The transport singleton
 * owns this state and serializes all calls with the model mutex. No SDK I/O or
 * UI link enum belongs here: stale/offline presentation cannot disable repair. */
#define BMB_TRANSPORT_CONNECT_MS 30000U
#define BMB_TRANSPORT_SUBSCRIBE_MS 10000U
#define BMB_TRANSPORT_TELEMETRY_MS 30000U
#define BMB_TRANSPORT_RETRY_MIN_MS 5000U
#define BMB_TRANSPORT_RETRY_MAX_MS 300000U

typedef enum {
    BMB_TRANSPORT_WAIT,
    BMB_TRANSPORT_START,
    BMB_TRANSPORT_RECREATE,
} bmb_transport_action_t;

typedef struct {
    uint32_t attempt_ms, connected_ms, report_ms;
    uint32_t retry_started_ms, retry_wait_ms, next_retry_ms;
    bool connected, subscribed, report_seen, retry_pending, rx_busy;
} bmb_transport_policy_t;

void bmb_transport_policy_reset(bmb_transport_policy_t *policy);
void bmb_transport_policy_attempt(bmb_transport_policy_t *policy, uint32_t now_ms);
void bmb_transport_policy_connected(bmb_transport_policy_t *policy, uint32_t now_ms);
void bmb_transport_policy_failed(bmb_transport_policy_t *policy, uint32_t now_ms);
void bmb_transport_policy_subscribed(bmb_transport_policy_t *policy, bool accepted,
                                      uint32_t now_ms);
void bmb_transport_policy_report(bmb_transport_policy_t *policy, uint32_t now_ms);
bmb_transport_action_t bmb_transport_policy_step(bmb_transport_policy_t *policy,
    uint32_t now_ms, bool active, bool wifi_ready, bool has_client);
/* RX remains busy through parsing the final fragment. A command admitted while
 * RX is in progress must be revalidated against the resulting model first. */
bool bmb_transport_policy_can_send(const bmb_transport_policy_t *policy);
