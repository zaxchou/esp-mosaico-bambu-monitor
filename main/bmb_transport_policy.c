// SPDX-License-Identifier: Apache-2.0
#include "bmb_transport_policy.h"

void bmb_transport_policy_reset(bmb_transport_policy_t *policy)
{
    *policy = (bmb_transport_policy_t){.next_retry_ms = BMB_TRANSPORT_RETRY_MIN_MS};
}

void bmb_transport_policy_attempt(bmb_transport_policy_t *policy, uint32_t now_ms)
{
    uint32_t next_retry_ms = policy->next_retry_ms;
    *policy = (bmb_transport_policy_t){
        .attempt_ms = now_ms,
        .next_retry_ms = next_retry_ms ? next_retry_ms : BMB_TRANSPORT_RETRY_MIN_MS,
    };
}

void bmb_transport_policy_connected(bmb_transport_policy_t *policy, uint32_t now_ms)
{
    policy->connected = true;
    policy->connected_ms = now_ms;
}

void bmb_transport_policy_failed(bmb_transport_policy_t *policy, uint32_t now_ms)
{
    /* Several ERROR/DISCONNECTED callbacks may describe the same failure.
     * Never keep postponing its deadline or accelerate the backoff per event. */
    if (policy->retry_pending) return;
    policy->retry_pending = true;
    policy->retry_started_ms = now_ms;
    policy->retry_wait_ms = policy->next_retry_ms;
    uint32_t next = policy->next_retry_ms * 2U;
    policy->next_retry_ms = next < BMB_TRANSPORT_RETRY_MAX_MS ?
                            next : BMB_TRANSPORT_RETRY_MAX_MS;
}

void bmb_transport_policy_subscribed(bmb_transport_policy_t *policy, bool accepted,
                                      uint32_t now_ms)
{
    policy->subscribed = accepted;
    if (!accepted) bmb_transport_policy_failed(policy, now_ms);
}

void bmb_transport_policy_report(bmb_transport_policy_t *policy, uint32_t now_ms)
{
    policy->report_seen = true;
    policy->report_ms = now_ms;
    /* Live telemetry is stronger evidence than an absent SUBACK. Only usable
     * telemetry, not a TCP/MQTT connection or command ACK, resets the backoff. */
    policy->subscribed = true;
    if (policy->connected) {
        policy->retry_pending = false;
        policy->next_retry_ms = BMB_TRANSPORT_RETRY_MIN_MS;
    }
}

bmb_transport_action_t bmb_transport_policy_step(bmb_transport_policy_t *policy,
    uint32_t now_ms, bool active, bool wifi_ready, bool has_client)
{
    if (!active || !wifi_ready) return BMB_TRANSPORT_WAIT;
    if (has_client && !policy->retry_pending) {
        bool expired;
        if (!policy->connected) {
            expired = (uint32_t)(now_ms - policy->attempt_ms) >= BMB_TRANSPORT_CONNECT_MS;
        } else if (!policy->subscribed) {
            expired = (uint32_t)(now_ms - policy->connected_ms) >= BMB_TRANSPORT_SUBSCRIBE_MS;
        } else {
            uint32_t baseline = policy->report_seen ? policy->report_ms : policy->connected_ms;
            expired = (uint32_t)(now_ms - baseline) >= BMB_TRANSPORT_TELEMETRY_MS;
        }
        if (expired) bmb_transport_policy_failed(policy, now_ms);
    }
    if (policy->retry_pending &&
        (uint32_t)(now_ms - policy->retry_started_ms) < policy->retry_wait_ms) {
        return BMB_TRANSPORT_WAIT;
    }
    if (!has_client) return BMB_TRANSPORT_START;
    return policy->retry_pending ? BMB_TRANSPORT_RECREATE : BMB_TRANSPORT_WAIT;
}

bool bmb_transport_policy_can_send(const bmb_transport_policy_t *policy)
{
    return policy->connected && policy->subscribed && policy->report_seen &&
           !policy->retry_pending && !policy->rx_busy;
}
