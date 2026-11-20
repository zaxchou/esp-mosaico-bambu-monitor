// SPDX-License-Identifier: Apache-2.0
#include "bmb_transport_policy.h"
#include "bmb_state.h"
#include "bmb_report.h"
#include "bmb_rx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned cases;

static void connected(bmb_transport_policy_t *policy, uint32_t now)
{
    bmb_transport_policy_reset(policy);
    bmb_transport_policy_attempt(policy, now);
    bmb_transport_policy_connected(policy, now);
}

static void live(bmb_transport_policy_t *policy, uint32_t now)
{
    connected(policy, now);
    bmb_transport_policy_subscribed(policy, true, now);
    bmb_transport_policy_report(policy, now);
}

static void initial_and_start_failure(void)
{
    bmb_transport_policy_t p;
    bmb_transport_policy_reset(&p);
    assert(bmb_transport_policy_step(&p, 0, false, true, false) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, 0, true, false, false) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, 0, true, true, false) == BMB_TRANSPORT_START);
    bmb_transport_policy_attempt(&p, 10);
    bmb_transport_policy_failed(&p, 10); /* init/allocation/task start failure */
    assert(bmb_transport_policy_step(&p, 5009, true, true, false) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, 5010, true, true, false) == BMB_TRANSPORT_START);
    bmb_transport_policy_attempt(&p, 5010);
    assert(!p.retry_pending && p.next_retry_ms == 10000);
    ++cases;
}

static void no_first_connection_or_report(void)
{
    bmb_transport_policy_t p;
    bmb_transport_policy_reset(&p);
    bmb_transport_policy_attempt(&p, 0);
    assert(bmb_transport_policy_step(&p, 29999, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, 30000, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(p.retry_pending);
    assert(bmb_transport_policy_step(&p, 35000, true, true, true) == BMB_TRANSPORT_RECREATE);
    /* Never received telemetry, still well before the old 5-minute uptime gate. */
    connected(&p, 100);
    bmb_transport_policy_subscribed(&p, true, 110);
    assert(!p.report_seen);
    assert(bmb_transport_policy_step(&p, 30099, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, 30100, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(p.retry_pending);
    assert(bmb_transport_policy_step(&p, 35100, true, true, true) == BMB_TRANSPORT_RECREATE);
    ++cases;
}

static void subscription_failures(void)
{
    bmb_transport_policy_t p;
    connected(&p, 100);
    assert(bmb_transport_policy_step(&p, 10099, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(!p.retry_pending);
    assert(bmb_transport_policy_step(&p, 10100, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(p.retry_pending); /* accepted subscribe write, missing SUBACK */
    assert(bmb_transport_policy_step(&p, 15100, true, true, true) == BMB_TRANSPORT_RECREATE);
    connected(&p, 100);
    bmb_transport_policy_subscribed(&p, false, 110); /* broker rejects SUBACK */
    assert(bmb_transport_policy_step(&p, 5109, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, 5110, true, true, true) == BMB_TRANSPORT_RECREATE);
    connected(&p, 100);
    bmb_transport_policy_failed(&p, 105); /* subscribe API returns -1/-2 */
    assert(bmb_transport_policy_step(&p, 5105, true, true, true) == BMB_TRANSPORT_RECREATE);
    ++cases;
}

static void offline_presentation_and_stale_telemetry(void)
{
    bmb_state_t model;
    bmb_state_init(&model);
    model.link = BMB_LINK_OFFLINE; /* UI/Wi-Fi presentation must not gate repair. */
    bmb_transport_policy_t p;
    live(&p, 500);
    assert(bmb_transport_policy_step(&p, 30500, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(p.retry_pending && model.link == BMB_LINK_OFFLINE);
    assert(bmb_transport_policy_step(&p, 35500, true, true, true) == BMB_TRANSPORT_RECREATE);
    /* Old telemetry retained in the model cannot expire a newly created client. */
    bmb_transport_policy_attempt(&p, 35500);
    bmb_transport_policy_connected(&p, 35600);
    bmb_transport_policy_subscribed(&p, true, 35610);
    assert(!p.report_seen);
    assert(bmb_transport_policy_step(&p, 35611, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(!p.retry_pending);
    ++cases;
}

static void bounded_backoff_and_duplicate_failures(void)
{
    const uint32_t waits[] = {5000, 10000, 20000, 40000, 80000, 160000, 300000, 300000};
    bmb_transport_policy_t p;
    bmb_transport_policy_reset(&p);
    uint32_t now = 123;
    for (unsigned i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
        bmb_transport_policy_attempt(&p, now);
        bmb_transport_policy_connected(&p, now + 1);
        /* A TLS/MQTT connection without usable telemetry is not recovery. */
        bmb_transport_policy_failed(&p, now + 2);
        assert(p.retry_wait_ms == waits[i]);
        uint32_t next = p.next_retry_ms;
        bmb_transport_policy_failed(&p, now + 1000);
        assert(p.retry_started_ms == now + 2 && p.next_retry_ms == next);
        assert(bmb_transport_policy_step(&p, now + 1 + waits[i], true, true, true) == BMB_TRANSPORT_WAIT);
        assert(bmb_transport_policy_step(&p, now + 2 + waits[i], true, true, true) == BMB_TRANSPORT_RECREATE);
        now += 2 + waits[i];
    }
    bmb_transport_policy_attempt(&p, now);
    bmb_transport_policy_connected(&p, now + 1);
    bmb_transport_policy_report(&p, now + 2);
    assert(p.next_retry_ms == BMB_TRANSPORT_RETRY_MIN_MS);
    p.connected = false;
    bmb_transport_policy_failed(&p, now + 3);
    assert(p.retry_wait_ms == BMB_TRANSPORT_RETRY_MIN_MS);
    ++cases;
}

static void late_telemetry_and_stop_interleavings(void)
{
    bmb_transport_policy_t p;
    live(&p, 100);
    assert(bmb_transport_policy_step(&p, 30100, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(p.retry_pending);
    bmb_transport_policy_report(&p, 30200); /* fresh report arrives during backoff */
    assert(!p.retry_pending);
    assert(bmb_transport_policy_step(&p, 35100, true, true, true) == BMB_TRANSPORT_WAIT);
    p.connected = false;
    bmb_transport_policy_failed(&p, 35101);
    assert(bmb_transport_policy_step(&p, 40101, false, true, true) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, 40101, true, false, true) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, 50000, false, true, false) == BMB_TRANSPORT_WAIT);
    /* An explicit restart resets backoff; queued old deadlines cannot revive stop. */
    bmb_transport_policy_reset(&p);
    assert(bmb_transport_policy_step(&p, 50001, true, true, false) == BMB_TRANSPORT_START);
    assert(p.next_retry_ms == BMB_TRANSPORT_RETRY_MIN_MS);
    ++cases;
}

static void wraparound_deadlines(void)
{
    bmb_transport_policy_t p;
    uint32_t start = UINT32_MAX - 1000U;
    connected(&p, start);
    bmb_transport_policy_subscribed(&p, true, start);
    assert(bmb_transport_policy_step(&p, start + 29999U, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(!p.retry_pending);
    assert(bmb_transport_policy_step(&p, start + 30000U, true, true, true) == BMB_TRANSPORT_WAIT);
    assert(p.retry_pending);
    assert(bmb_transport_policy_step(&p, start + 35000U, true, true, true) == BMB_TRANSPORT_RECREATE);
    connected(&p, start);
    bmb_transport_policy_failed(&p, start);
    assert(bmb_transport_policy_step(&p, start + 4999U, true, true, false) == BMB_TRANSPORT_WAIT);
    assert(bmb_transport_policy_step(&p, start + 5000U, true, true, false) == BMB_TRANSPORT_START);
    ++cases;
}

static void command_model(bmb_state_t *model, uint32_t now)
{
    bmb_state_init(model);
    model->link = BMB_LINK_ONLINE;
    model->state_received = true;
    model->last_report_ms = now;
    model->print_state = BMB_PRINT_RUNNING;
    strcpy(model->dev_id, "printer");
    strcpy(model->task, "task A");
}

static void rx_parse_before_command_interleavings(void)
{
    bmb_transport_policy_t p;
    bmb_state_t model;
    bmb_rx_t rx;
    char storage[256];
    static const char topic[] = "device/printer/report";
    static const char changed[] = "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"task B\"}}";
    const uint32_t sequences[] = {123, 124};
    live(&p, 1000);
    command_model(&model, 1000);
    bmb_rx_init(&rx, storage, sizeof(storage));
    p.rx_busy = true; /* first fragment arrives; UI submits between fragments */
    assert(bmb_rx_feed(&rx, topic, strlen(topic), changed, 10, strlen(changed), 0) == BMB_RX_WAIT);
    assert(bmb_command_begin_locked(&model, BMB_COMMAND_PAUSE, sequences, 1, 1001));
    uint32_t token = model.command.token;
    assert(!bmb_transport_policy_can_send(&p));
    assert(model.command.sent_mask == 0); /* WAIT never invokes the sender */
    assert(bmb_rx_feed(&rx, NULL, 0, changed + 10, strlen(changed) - 10,
                       strlen(changed), 10) == BMB_RX_COMPLETE);
    assert(bmb_report_apply(&model, rx.topic, rx.topic_len, rx.data, rx.received, 1002));
    bmb_transport_policy_report(&p, 1002);
    assert(!bmb_transport_policy_can_send(&p)); /* final fragment parsed under fence */
    p.rx_busy = false;
    assert(bmb_transport_policy_can_send(&p));
    /* Actual report parser changed task before the MQTT callback validates send. */
    assert(strcmp(model.task, "task B") == 0);
    assert(!bmb_command_can_send_locked(&model, token, 0, 1002));
    assert(!model.ctrl_busy && model.command.sent_mask == 0);
    assert(bmb_command_begin_locked(&model, BMB_COMMAND_PAUSE, sequences, 1, 1003));
    token = model.command.token;
    /* A complete current-task report now permits a single synchronous write. */
    assert(bmb_rx_feed(&rx, topic, strlen(topic), changed, strlen(changed), strlen(changed), 0) == BMB_RX_COMPLETE);
    assert(bmb_report_apply(&model, rx.topic, rx.topic_len, rx.data, rx.received, 1004));
    bmb_transport_policy_report(&p, 1004);
    assert(bmb_transport_policy_can_send(&p));
    assert(bmb_command_can_send_locked(&model, token, 0, 1004));
    assert(!bmb_command_can_send_locked(&model, token, 0, 1005)); /* no resend */
    ++cases;
}

static void bad_rx_and_silence_never_send(void)
{
    bmb_transport_policy_t p;
    bmb_state_t model;
    bmb_rx_t rx;
    char storage[128];
    static const char topic[] = "device/printer/report";
    static const char report[] = "{\"print\":{\"mc_percent\":3}}";
    const uint32_t sequence[] = {123};
    live(&p, 1000);
    command_model(&model, 1000);
    bmb_rx_init(&rx, storage, sizeof(storage));
    assert(bmb_command_begin_locked(&model, BMB_COMMAND_PAUSE, sequence, 1, 1001));
    uint32_t token = model.command.token;
    p.rx_busy = true;
    assert(bmb_rx_feed(&rx, topic, strlen(topic), report, 5, strlen(report), 0) == BMB_RX_WAIT);
    assert(!bmb_transport_policy_can_send(&p));
    /* A gap in fragments drops RX; it cannot release a payload to the sender. */
    assert(bmb_rx_feed(&rx, NULL, 0, report + 6, strlen(report) - 6,
                       strlen(report), 6) == BMB_RX_DROP);
    assert(model.command.sent_mask == 0);
    /* Complete bytes with invalid JSON also fail the accepted-report gate. */
    assert(bmb_rx_feed(&rx, topic, strlen(topic), "bad", 3, 3, 0) == BMB_RX_COMPLETE);
    assert(!bmb_report_apply(&model, rx.topic, rx.topic_len, rx.data, rx.received, 1002));
    p.rx_busy = false;
    assert(model.command.sent_mask == 0 && model.ctrl_busy);
    /* A lifecycle notification/tick owns expiry, never sending. No DATA leaves
     * the attempt unsent even though policy reports a currently usable session. */
    bmb_command_tick_locked(&model, 1001 + BMB_COMMAND_TIMEOUT_MS - 1);
    assert(model.ctrl_busy && model.command.sent_mask == 0);
    bmb_command_tick_locked(&model, 1001 + BMB_COMMAND_TIMEOUT_MS);
    assert(!model.ctrl_busy && model.command.sent_mask == 0);
    assert(bmb_rx_feed(&rx, topic, strlen(topic), report, strlen(report), strlen(report), 0) == BMB_RX_COMPLETE);
    assert(bmb_report_apply(&model, rx.topic, rx.topic_len, rx.data, rx.received, 9010));
    bmb_transport_policy_report(&p, 9010);
    assert(!bmb_command_can_send_locked(&model, token, 0, 9010));
    ++cases;
}

static void multipart_disconnect_and_recreate_interleavings(void)
{
    bmb_transport_policy_t p;
    bmb_state_t model;
    const uint32_t sequences[] = {123, 124};
    live(&p, 1000);
    command_model(&model, 1000);
    assert(bmb_command_begin_locked(&model, BMB_COMMAND_LIGHT, sequences, 2, 1001));
    uint32_t token = model.command.token;
    assert(bmb_transport_policy_can_send(&p));
    assert(bmb_command_can_send_locked(&model, token, 0, 1001));
    /* Disconnect/cancellation between the two QoS0 writes discards the rest. */
    p.connected = false;
    bmb_transport_policy_failed(&p, 1002);
    bmb_command_cancel_locked(&model, "disconnected");
    assert(!bmb_transport_policy_can_send(&p));
    assert(!bmb_command_can_send_locked(&model, token, 1, 1002));
    bmb_transport_policy_attempt(&p, 6002);
    bmb_transport_policy_connected(&p, 6003);
    assert(!bmb_transport_policy_can_send(&p)); /* old model is not new telemetry */
    bmb_transport_policy_report(&p, 6004);
    assert(bmb_transport_policy_can_send(&p));
    assert(!bmb_command_can_send_locked(&model, token, 0, 6004));
    assert(!bmb_command_can_send_locked(&model, token, 1, 6004));
    ++cases;
}

int main(void)
{
    initial_and_start_failure();
    no_first_connection_or_report();
    subscription_failures();
    offline_presentation_and_stale_telemetry();
    bounded_backoff_and_duplicate_failures();
    late_telemetry_and_stop_interleavings();
    wraparound_deadlines();
    rx_parse_before_command_interleavings();
    bad_rx_and_silence_never_send();
    multipart_disconnect_and_recreate_interleavings();
    printf("bmb_transport_policy: %u lifecycle/interleaving cases passed\n", cases);
    return 0;
}
