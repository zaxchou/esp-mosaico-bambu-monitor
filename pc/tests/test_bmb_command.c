// SPDX-License-Identifier: Apache-2.0
// Native regression against the original bmb_command.c and bmb_state.c.
// Single-threaded: the native model lock is a no-op. No transport/parser mocks.
#include "bmb_command.h"
#include "bmb_state.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define START_MS UINT32_C(20000)

static bmb_state_t model;
static bmb_state_t snapshot;
static unsigned failures;
static unsigned checks;
static const uint32_t sequences[BMB_COMMAND_PARTS] = {101, 102};
static const char device_id[] = "0123456789ABCDEF";
static const char report_topic[] = "device/0123456789ABCDEF/report";
static const char *const command_names[] = {
    "pause", "resume", "stop", "print_speed", "ledctrl",
};

/* Keep running other groups after a failure, with the failing line recorded. */
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
        return; \
    } \
} while (0)

static uint32_t later(uint32_t started, uint32_t elapsed)
{
    return started + elapsed;
}

static void fresh(uint32_t now_ms)
{
    bmb_state_init(&model);
    model.link = BMB_LINK_ONLINE;
    model.print_state = BMB_PRINT_RUNNING;
    model.state_received = true;
    model.has_task = true;
    model.last_report_ms = now_ms;
    snprintf(model.dev_id, sizeof(model.dev_id), "%s", device_id);
    snprintf(model.task, sizeof(model.task), "%s", "test-plate.3mf");
}

static bool begin(bmb_command_kind_t kind, unsigned parts, uint32_t now_ms)
{
    return bmb_command_begin_locked(&model, kind, sequences, parts, now_ms);
}

/* Existing transport lifecycle cases explicitly arm STOP before beginning it.
 * Confirmation guard regressions below call begin() directly to check rejection. */
static bool begin_confirmed(bmb_command_kind_t kind, unsigned parts, uint32_t now_ms)
{
    if (kind == BMB_COMMAND_STOP && !bmb_command_stop_arm_locked(&model, now_ms)) {
        return false;
    }
    return begin(kind, parts, now_ms);
}

static bool send_part(unsigned part, uint32_t now_ms)
{
    return bmb_command_can_send_locked(&model, model.command.token, part, now_ms);
}

static bool ack_part(unsigned part, const char *result, const char *reason,
                     uint32_t now_ms)
{
    return bmb_command_ack_locked(&model, report_topic, sizeof(report_topic) - 1,
                                   model.command.kind == BMB_COMMAND_LIGHT,
                                   command_names[model.command.kind],
                                   model.command.sequence[part], result, reason,
                                   now_ms);
}

static void save_model(void)
{
    /* Copy bytes rather than assigning a struct with unspecified padding. */
    memcpy(&snapshot, &model, sizeof(snapshot));
}

static bool model_unchanged(void)
{
    return memcmp(&snapshot, &model, sizeof(model)) == 0;
}

static void test_begin_prerequisites(void)
{
    bmb_state_init(&model);
    CHECK(!model.state_received && model.link == BMB_LINK_OFFLINE);
    CHECK(!model.link_resetting);
    CHECK(!model.ctrl_busy && model.command.token == 0);
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));

    fresh(START_MS);
    model.link_resetting = true;
    save_model();
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(model_unchanged());
    model.link_resetting = false;
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));

    fresh(START_MS);
    model.state_received = false;
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(!model.ctrl_busy);
    for (bmb_link_t link = BMB_LINK_OFFLINE; link <= BMB_LINK_CONNECTING; ++link) {
        fresh(START_MS);
        model.link = link;
        CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
        CHECK(!model.ctrl_busy);
    }
    fresh(START_MS);
    model.dev_id[0] = '\0';
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(!model.ctrl_busy);

    fresh(START_MS);
    model.last_report_ms = START_MS - BMB_TELEMETRY_FRESH_MS;
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(!model.ctrl_busy);
    model.last_report_ms -= 1;
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
    model.last_report_ms = START_MS - (BMB_TELEMETRY_FRESH_MS - 1);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(model.ctrl_busy);

    /* A received report at monotonic timestamp zero is established state. */
    fresh(0);
    CHECK(begin_confirmed(BMB_COMMAND_PAUSE, 1, 0));
    CHECK(send_part(0, 0));
    CHECK(ack_part(0, "SUCCESS", NULL, 0));
    CHECK(!model.ctrl_busy);
}

static void test_begin_validation_and_busy(void)
{
    static const uint32_t zero_first[] = {0, 102};
    static const uint32_t zero_second[] = {101, 0};
    static const uint32_t duplicate[] = {101, 101};
    fresh(START_MS);
    CHECK(!bmb_command_begin_locked(NULL, BMB_COMMAND_STOP, sequences, 1, START_MS));
    CHECK(!bmb_command_begin_locked(&model, BMB_COMMAND_STOP, NULL, 1, START_MS));
    CHECK(!begin((bmb_command_kind_t)-1, 1, START_MS));
    CHECK(!begin((bmb_command_kind_t)(BMB_COMMAND_LIGHT + 1), 1, START_MS));
    CHECK(!begin(BMB_COMMAND_LIGHT, 0, START_MS));
    CHECK(!begin(BMB_COMMAND_LIGHT, BMB_COMMAND_PARTS + 1, START_MS));
    CHECK(!begin(BMB_COMMAND_STOP, 2, START_MS));
    CHECK(!bmb_command_begin_locked(&model, BMB_COMMAND_PAUSE, zero_first, 1, START_MS));
    CHECK(!bmb_command_begin_locked(&model, BMB_COMMAND_LIGHT, zero_second, 2, START_MS));
    CHECK(!bmb_command_begin_locked(&model, BMB_COMMAND_LIGHT, duplicate, 2, START_MS));
    CHECK(!model.ctrl_busy);
    CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
    save_model();
    CHECK(!begin(BMB_COMMAND_PAUSE, 1, START_MS));
    CHECK(model_unchanged());
    CHECK(!send_part(BMB_COMMAND_PARTS, START_MS));
    CHECK(model_unchanged());
    CHECK(!send_part(UINT32_MAX, START_MS));
    CHECK(model_unchanged());
    bmb_command_sent_locked(&model, model.command.token, BMB_COMMAND_PARTS, false);
    CHECK(model_unchanged());
}

static void test_pending_before_io_and_fast_ack(void)
{
    for (bmb_command_kind_t kind = BMB_COMMAND_PAUSE; kind <= BMB_COMMAND_LIGHT;
         ++kind) {
        fresh(START_MS);
        CHECK(begin_confirmed(kind, 1, START_MS));
        const uint32_t token = model.command.token;
        CHECK(model.ctrl_busy && model.command.kind == kind);
        CHECK(model.command.started_ms == START_MS && model.command.parts == 1);
        CHECK(model.command.sequence[0] == sequences[0]);
        CHECK(model.command.sent_mask == 0 && model.command.ack_mask == 0);
        CHECK(!model.command.failed && model.command.reason[0] == '\0');
        CHECK(model.command.queued_print_state == BMB_PRINT_RUNNING);
        CHECK(strcmp(model.command.device_id, model.dev_id) == 0);
        CHECK(strcmp(model.command.task, model.task) == 0);
        CHECK(model.ctrl_status[0] != '\0');
        save_model();
        CHECK(!ack_part(0, "SUCCESS", NULL, START_MS));
        CHECK(model_unchanged());
        CHECK(send_part(0, START_MS));
        CHECK(model.command.sent_mask == 1 && model.command.ack_mask == 0);
        CHECK(model.ctrl_busy);
        save_model();
        CHECK(!send_part(0, START_MS));
        CHECK(model_unchanged());
        /* Simulate the printer ACK racing ahead of publish's return callback. */
        CHECK(ack_part(0, "SUCCESS", NULL, START_MS));
        CHECK(model.command.ack_mask == 1 && !model.ctrl_busy);
        CHECK(!model.command.failed);
        save_model();
        bmb_command_sent_locked(&model, token, 0, true);
        CHECK(model_unchanged());
        CHECK(!bmb_command_can_send_locked(&model, token, 0, START_MS));
        CHECK(!ack_part(0, "SUCCESS", NULL, START_MS));
        CHECK(model_unchanged());
    }
    /* sent(success) also changes nothing while the command is still pending. */
    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(send_part(0, START_MS));
    save_model();
    bmb_command_sent_locked(&model, model.command.token, 0, true);
    CHECK(model_unchanged());
}

static void test_unsent_expiry(void)
{
    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
    const uint32_t token = model.command.token;
    bmb_command_tick_locked(&model, later(START_MS, BMB_COMMAND_TIMEOUT_MS - 1));
    CHECK(model.ctrl_busy && model.command.sent_mask == 0);
    CHECK(!bmb_command_can_send_locked(&model, token, 0,
                                       later(START_MS, BMB_COMMAND_TIMEOUT_MS)));
    CHECK(!model.ctrl_busy && model.command.token != token);
    CHECK(model.command.sent_mask == 0 && model.command.ack_mask == 0);
    save_model();
    CHECK(!bmb_command_can_send_locked(&model, token, 0,
                                       later(START_MS, BMB_COMMAND_TIMEOUT_MS + 1)));
    bmb_command_sent_locked(&model, token, 0, true);
    CHECK(model_unchanged());

    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(send_part(0, later(START_MS, BMB_COMMAND_TIMEOUT_MS - 1)));
    CHECK(model.ctrl_busy);
    CHECK(!ack_part(0, "SUCCESS", NULL, later(START_MS, BMB_COMMAND_TIMEOUT_MS)));
    CHECK(!model.ctrl_busy && model.command.ack_mask == 0);

    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
    bmb_command_tick_locked(&model, later(START_MS, BMB_COMMAND_TIMEOUT_MS));
    CHECK(!model.ctrl_busy && model.command.sent_mask == 0);
    CHECK(!send_part(1, later(START_MS, BMB_COMMAND_TIMEOUT_MS + 1)));
}

static void test_cancel_tokens_prevent_replay(void)
{
    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
    const uint32_t old_token = model.command.token;
    CHECK(send_part(0, START_MS));
    bmb_command_cancel_locked(&model, "cancelled by test");
    CHECK(!model.ctrl_busy && model.command.token != old_token);
    CHECK(strcmp(model.ctrl_status, "cancelled by test") == 0);
    CHECK(!bmb_command_can_send_locked(&model, old_token, 1, START_MS));
    CHECK(!ack_part(0, "SUCCESS", NULL, START_MS));

    static const uint32_t next_sequences[] = {201, 202};
    CHECK(bmb_command_begin_locked(&model, BMB_COMMAND_LIGHT, next_sequences, 2,
                                   START_MS));
    CHECK(model.command.token != old_token);
    save_model();
    CHECK(!bmb_command_can_send_locked(&model, old_token, 0, START_MS));
    bmb_command_sent_locked(&model, old_token, 0, false);
    bmb_command_sent_locked(&model, old_token, 1, true);
    CHECK(model_unchanged());
    CHECK(!bmb_command_ack_locked(&model, report_topic, sizeof(report_topic) - 1,
                                   true, "ledctrl", sequences[0], "SUCCESS", NULL,
                                   START_MS));
    CHECK(model_unchanged());
    CHECK(send_part(0, START_MS));
    CHECK(send_part(1, START_MS));
    CHECK(ack_part(0, "SUCCESS", NULL, START_MS));
    CHECK(ack_part(1, "SUCCESS", NULL, START_MS));
    CHECK(!model.ctrl_busy);
}

static void test_reconnect_tokens_prevent_replay(void)
{
    for (bmb_link_t disconnected = BMB_LINK_OFFLINE;
         disconnected <= BMB_LINK_CONNECTING; ++disconnected) {
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
        const uint32_t old_token = model.command.token;
        model.link = disconnected;
        bmb_command_tick_locked(&model, START_MS + 1);
        CHECK(!model.ctrl_busy && model.command.token != old_token);
        model.link = BMB_LINK_ONLINE;
        model.last_report_ms = START_MS + 2;
        CHECK(!bmb_command_can_send_locked(&model, old_token, 0, START_MS + 2));
        static const uint32_t next_sequence[] = {301};
        CHECK(bmb_command_stop_arm_locked(&model, START_MS + 2));
        CHECK(bmb_command_begin_locked(&model, BMB_COMMAND_STOP, next_sequence, 1,
                                       START_MS + 2));
        save_model();
        CHECK(!bmb_command_can_send_locked(&model, old_token, 0, START_MS + 2));
        bmb_command_sent_locked(&model, old_token, 0, false);
        CHECK(model_unchanged());
        CHECK(send_part(0, START_MS + 2));
        save_model();
        CHECK(!bmb_command_ack_locked(&model, report_topic, sizeof(report_topic) - 1,
                                       false, "stop", sequences[0], "SUCCESS", NULL,
                                       START_MS + 2));
        CHECK(model_unchanged());
        CHECK(ack_part(0, "SUCCESS", NULL, START_MS + 2));
        CHECK(!model.ctrl_busy);
    }

    /* Ending-session callbacks must cancel even before link goes offline.
     * Exercise tick directly and its entry paths through ACK and can_send.
     */
    for (unsigned trigger = 0; trigger < 3; ++trigger) {
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
        const uint32_t old_token = model.command.token;
        CHECK(send_part(0, START_MS));
        model.link_resetting = true;
        if (trigger == 0) {
            bmb_command_tick_locked(&model, START_MS + 1);
        } else if (trigger == 1) {
            CHECK(!ack_part(0, "SUCCESS", NULL, START_MS + 1));
        } else {
            CHECK(!bmb_command_can_send_locked(&model, old_token, 1, START_MS + 1));
        }
        CHECK(model.link == BMB_LINK_ONLINE && model.link_resetting);
        CHECK(!model.ctrl_busy && model.command.token != old_token);
        CHECK(strcmp(model.ctrl_status, "已取消") == 0);
        CHECK(model.command.sent_mask == 1 && model.command.ack_mask == 0);
        save_model();
        CHECK(!ack_part(0, "SUCCESS", NULL, START_MS + 1));
        CHECK(!bmb_command_can_send_locked(&model, old_token, 1, START_MS + 1));
        bmb_command_sent_locked(&model, old_token, 0, true);
        bmb_command_sent_locked(&model, old_token, 0, false);
        CHECK(model_unchanged());
        model.link_resetting = false;
        save_model();
        CHECK(!ack_part(0, "SUCCESS", NULL, START_MS + 2));
        CHECK(!bmb_command_can_send_locked(&model, old_token, 1, START_MS + 2));
        CHECK(model_unchanged());
    }
}

static void test_ack_filters(void)
{
    static const struct {
        const char *topic;
        size_t topic_len;
        bool system_family;
        const char *command;
        uint32_t sequence;
        const char *result;
    } invalid[] = {
        {report_topic, sizeof(report_topic) - 1, false, "stop", 999, "SUCCESS"},
        {report_topic, sizeof(report_topic) - 1, false, "stop", 0, "SUCCESS"},
        {"device/OTHER/report", sizeof("device/OTHER/report") - 1,
         false, "stop", 101, "SUCCESS"},
        {"device/0123456789ABCDEF/request", sizeof("device/0123456789ABCDEF/request") - 1,
         false, "stop", 101, "SUCCESS"},
        {"Device/0123456789ABCDEF/report", sizeof(report_topic) - 1,
         false, "stop", 101, "SUCCESS"},
        {"device/0123456789ABCDEF/reportX", sizeof(report_topic),
         false, "stop", 101, "SUCCESS"},
        {report_topic, sizeof(report_topic) - 2, false, "stop", 101, "SUCCESS"},
        {report_topic, 0, false, "stop", 101, "SUCCESS"},
        {report_topic, SIZE_MAX, false, "stop", 101, "SUCCESS"},
        {NULL, sizeof(report_topic) - 1, false, "stop", 101, "SUCCESS"},
        {report_topic, sizeof(report_topic) - 1, false, "pause", 101, "SUCCESS"},
        {report_topic, sizeof(report_topic) - 1, false, "STOP", 101, "SUCCESS"},
        {report_topic, sizeof(report_topic) - 1, false, "", 101, "SUCCESS"},
        {report_topic, sizeof(report_topic) - 1, false, NULL, 101, "SUCCESS"},
        {report_topic, sizeof(report_topic) - 1, true, "stop", 101, "SUCCESS"},
        {report_topic, sizeof(report_topic) - 1, false, "stop", 101, NULL},
        {report_topic, sizeof(report_topic) - 1, false, "stop", 101, ""},
        {report_topic, sizeof(report_topic) - 1, false, "stop", 101, "UNKNOWN"},
        {report_topic, sizeof(report_topic) - 1, false, "stop", 101, "SUCCESSFUL"},
        {report_topic, sizeof(report_topic) - 1, false, "stop", 101, "true"},
        {report_topic, sizeof(report_topic) - 1, false, "stop", 101, " SUCCESS "},
    };
    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
    save_model();
    CHECK(!ack_part(0, "SUCCESS", NULL, START_MS));
    CHECK(!ack_part(0, "ERROR", "unsent", START_MS));
    CHECK(model_unchanged());
    CHECK(send_part(0, START_MS));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        save_model();
        CHECK(!bmb_command_ack_locked(&model, invalid[i].topic, invalid[i].topic_len,
                                       invalid[i].system_family, invalid[i].command,
                                       invalid[i].sequence, invalid[i].result,
                                       "must be ignored", START_MS));
        CHECK(model_unchanged());
    }
    /* Match the supplied topic range, without requiring a trailing NUL. */
    const char counted_topic[] = "device/0123456789ABCDEF/reportX";
    CHECK(bmb_command_ack_locked(&model, counted_topic, sizeof(report_topic) - 1,
                                  false, "stop", sequences[0], "SUCCESS", NULL,
                                  START_MS));
    CHECK(!model.ctrl_busy && model.command.ack_mask == 1);
    save_model();
    CHECK(!ack_part(0, "ERROR", "duplicate", START_MS));
    CHECK(model_unchanged());

    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
    CHECK(send_part(0, START_MS));
    save_model();
    CHECK(!bmb_command_ack_locked(&model, report_topic, sizeof(report_topic) - 1,
                                   false, "ledctrl", sequences[0], "SUCCESS", NULL,
                                   START_MS));
    CHECK(!ack_part(1, "SUCCESS", NULL, START_MS));
    CHECK(!ack_part(1, "FAIL", "unsent second part", START_MS));
    CHECK(model_unchanged());
}

static void test_ack_verdicts(void)
{
    static const char *const success[] = {"SUCCESS", "OK", "success", "oK"};
    static const char *const failure[] = {"FAIL", "FAILED", "ERROR", "fail", "eRrOr"};
    for (size_t i = 0; i < sizeof(success) / sizeof(success[0]); ++i) {
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_PAUSE, 1, START_MS));
        CHECK(send_part(0, START_MS));
        CHECK(ack_part(0, success[i], NULL, START_MS));
        CHECK(!model.ctrl_busy && !model.command.failed && model.command.ack_mask == 1);
        CHECK(model.ctrl_status[0] != '\0');
    }
    for (size_t i = 0; i < sizeof(failure) / sizeof(failure[0]); ++i) {
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_RESUME, 1, START_MS));
        CHECK(send_part(0, START_MS));
        CHECK(ack_part(0, failure[i], "printer rejected", START_MS));
        CHECK(!model.ctrl_busy && model.command.failed && model.command.ack_mask == 1);
        CHECK(strcmp(model.command.reason, "printer rejected") == 0);
        CHECK(strstr(model.ctrl_status, "printer rejected") != NULL);
    }
    for (unsigned empty_reason = 0; empty_reason < 2; ++empty_reason) {
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
        CHECK(send_part(0, START_MS));
        CHECK(ack_part(0, "FAILED", empty_reason ? "" : NULL, START_MS));
        CHECK(!model.ctrl_busy && model.command.failed);
        CHECK(model.command.reason[0] != '\0');
    }
}

static void test_light_both_acks_and_reverse_order(void)
{
    for (unsigned first = 0; first < BMB_COMMAND_PARTS; ++first) {
        const unsigned second = 1 - first;
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
        const uint32_t token = model.command.token;
        CHECK(send_part(first, START_MS));
        CHECK(model.command.sent_mask == (1U << first));
        CHECK(send_part(second, START_MS));
        CHECK(model.command.sent_mask == 3);
        CHECK(ack_part(first, "SUCCESS", NULL, START_MS + 1));
        CHECK(model.ctrl_busy && model.command.ack_mask == (1U << first));
        CHECK(!model.command.failed);
        save_model();
        CHECK(!ack_part(first, "FAIL", "duplicate must not fail", START_MS + 1));
        CHECK(!send_part(first, START_MS + 1));
        bmb_command_sent_locked(&model, token, first, true);
        CHECK(model_unchanged());
        CHECK(ack_part(second, "OK", NULL, START_MS + 2));
        CHECK(!model.ctrl_busy && model.command.ack_mask == 3 && !model.command.failed);
        save_model();
        bmb_command_sent_locked(&model, token, second, true);
        CHECK(!ack_part(second, "SUCCESS", NULL, START_MS + 2));
        CHECK(model_unchanged());
    }
}

static void test_light_mixed_failure(void)
{
    for (unsigned first = 0; first < BMB_COMMAND_PARTS; ++first) {
        for (unsigned failed_part = 0; failed_part < BMB_COMMAND_PARTS; ++failed_part) {
            const unsigned second = 1 - first;
            fresh(START_MS);
            CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
            CHECK(send_part(0, START_MS));
            CHECK(send_part(1, START_MS));
            CHECK(ack_part(first, first == failed_part ? "FAIL" : "SUCCESS",
                            "light denied", START_MS + 1));
            CHECK(model.ctrl_busy && model.command.ack_mask == (1U << first));
            CHECK(model.command.failed == (first == failed_part));
            CHECK(ack_part(second, second == failed_part ? "ERROR" : "OK",
                            "light denied", START_MS + 2));
            CHECK(!model.ctrl_busy && model.command.ack_mask == 3);
            CHECK(model.command.failed);
            CHECK(strcmp(model.command.reason, "light denied") == 0);
            CHECK(strstr(model.ctrl_status, "light denied") != NULL);
            save_model();
            CHECK(!ack_part(failed_part, "SUCCESS", NULL, START_MS + 2));
            CHECK(model_unchanged());
        }
    }
}

static void test_light_first_only_timeout(void)
{
    for (unsigned second_sent = 0; second_sent < 2; ++second_sent) {
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
        const uint32_t token = model.command.token;
        CHECK(send_part(0, START_MS));
        if (second_sent) {
            CHECK(send_part(1, START_MS));
        }
        CHECK(ack_part(0, "SUCCESS", NULL, START_MS + 1));
        CHECK(model.ctrl_busy && model.command.ack_mask == 1);
        bmb_command_tick_locked(&model, later(START_MS, BMB_COMMAND_TIMEOUT_MS - 1));
        CHECK(model.ctrl_busy);
        bmb_command_tick_locked(&model, later(START_MS, BMB_COMMAND_TIMEOUT_MS));
        CHECK(!model.ctrl_busy && model.command.token != token);
        CHECK(model.command.ack_mask == 1);
        save_model();
        CHECK(!ack_part(1, "SUCCESS", NULL, later(START_MS, BMB_COMMAND_TIMEOUT_MS)));
        CHECK(!bmb_command_can_send_locked(&model, token, 1,
                                           later(START_MS, BMB_COMMAND_TIMEOUT_MS)));
        bmb_command_sent_locked(&model, token, 0, true);
        CHECK(model_unchanged());
    }
}

static void test_publish_failure_cancels(void)
{
    for (unsigned parts = 1; parts <= BMB_COMMAND_PARTS; ++parts) {
        for (unsigned failed_part = 0; failed_part < parts; ++failed_part) {
            fresh(START_MS);
            CHECK(begin_confirmed(parts == 1 ? BMB_COMMAND_STOP : BMB_COMMAND_LIGHT,
                         parts, START_MS));
            const uint32_t token = model.command.token;
            CHECK(send_part(failed_part, START_MS));
            save_model();
            bmb_command_sent_locked(&model, token, failed_part, true);
            CHECK(model_unchanged());
            bmb_command_sent_locked(&model, token, failed_part, false);
            CHECK(!model.ctrl_busy && model.command.token != token);
            CHECK(strcmp(model.ctrl_status, snapshot.ctrl_status) != 0);
            save_model();
            CHECK(!ack_part(failed_part, "SUCCESS", NULL, START_MS));
            for (unsigned part = 0; part < parts; ++part) {
                CHECK(!bmb_command_can_send_locked(&model, token, part, START_MS));
                bmb_command_sent_locked(&model, token, part, true);
            }
            CHECK(model_unchanged());
        }
    }
    /* First light ACK does not hide a failure to publish the second part. */
    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
    const uint32_t token = model.command.token;
    CHECK(send_part(0, START_MS));
    CHECK(ack_part(0, "SUCCESS", NULL, START_MS));
    CHECK(send_part(1, START_MS));
    bmb_command_sent_locked(&model, token, 1, false);
    CHECK(!model.ctrl_busy && model.command.token != token);
    CHECK(model.command.ack_mask == 1);
}

static void test_tick_and_send_context_changes(void)
{
    for (unsigned use_tick = 0; use_tick < 2; ++use_tick) {
        for (unsigned change = 0; change < 4; ++change) {
            fresh(START_MS);
            CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
            const uint32_t token = model.command.token;
            if (use_tick) {
                CHECK(send_part(0, START_MS));
            }
            switch (change) {
            case 0:
                snprintf(model.dev_id, sizeof(model.dev_id), "%s", "OTHER-PRINTER");
                break;
            case 1:
                snprintf(model.task, sizeof(model.task), "%s", "different-plate.3mf");
                break;
            case 2:
                model.link = BMB_LINK_OFFLINE;
                break;
            default:
                model.link = BMB_LINK_CONNECTING;
                break;
            }
            if (use_tick) {
                bmb_command_tick_locked(&model, START_MS + 1);
            } else {
                CHECK(!bmb_command_can_send_locked(&model, token, 0, START_MS + 1));
            }
            CHECK(!model.ctrl_busy && model.command.token != token);
            CHECK(model.command.sent_mask == (use_tick ? 1U : 0U));
            CHECK(!bmb_command_can_send_locked(&model, token, 1, START_MS + 1));
            CHECK(!ack_part(0, "SUCCESS", NULL, START_MS + 1));
            CHECK(model.command.ack_mask == 0);
        }
    }
}

static void test_stale_telemetry_prevents_send(void)
{
    fresh(START_MS);
    model.last_report_ms = START_MS - (BMB_TELEMETRY_FRESH_MS - 1);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
    const uint32_t token = model.command.token;
    CHECK(!bmb_command_can_send_locked(&model, token, 0, START_MS + 1));
    CHECK(!model.ctrl_busy && model.command.token != token);
    CHECK(model.command.sent_mask == 0);
    model.last_report_ms = START_MS + 2;
    CHECK(!bmb_command_can_send_locked(&model, token, 0, START_MS + 2));
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS + 2));
    CHECK(send_part(0, START_MS + 2));
    CHECK(ack_part(0, "SUCCESS", NULL, START_MS + 2));

    fresh(START_MS);
    model.last_report_ms = START_MS - (BMB_TELEMETRY_FRESH_MS - 1);
    CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
    CHECK(send_part(0, START_MS));
    CHECK(ack_part(0, "SUCCESS", NULL, START_MS));
    CHECK(!send_part(1, START_MS + 1));
    CHECK(!model.ctrl_busy && model.command.sent_mask == 1);
}

static void test_print_state_change_cancels_stop(void)
{
    for (bmb_print_state_t changed = BMB_PRINT_IDLE; changed <= BMB_PRINT_OTHER;
         ++changed) {
        if (changed == BMB_PRINT_RUNNING) {
            continue;
        }
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
        const uint32_t token = model.command.token;
        model.print_state = changed;
        CHECK(!bmb_command_can_send_locked(&model, token, 0, START_MS + 1));
        CHECK(!model.ctrl_busy && model.command.token != token);
        CHECK(model.command.sent_mask == 0);
        CHECK(!ack_part(0, "SUCCESS", NULL, START_MS + 1));
    }
    for (bmb_command_kind_t kind = BMB_COMMAND_PAUSE; kind <= BMB_COMMAND_RESUME;
         ++kind) {
        fresh(START_MS);
        CHECK(begin_confirmed(kind, 1, START_MS));
        model.print_state = BMB_PRINT_FINISH;
        CHECK(!send_part(0, START_MS + 1));
        CHECK(!model.ctrl_busy && model.command.sent_mask == 0);
    }
}

static void test_uint32_monotonic_and_token_wrap(void)
{
    const uint32_t started = UINT32_MAX - UINT32_C(3999);
    fresh(started);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, started));
    const uint32_t token = model.command.token;
    bmb_command_tick_locked(&model, later(started, BMB_COMMAND_TIMEOUT_MS - 1));
    CHECK(model.ctrl_busy && model.command.token == token);
    CHECK(!bmb_command_can_send_locked(&model, token, 0,
                                       later(started, BMB_COMMAND_TIMEOUT_MS)));
    CHECK(!model.ctrl_busy && model.command.sent_mask == 0);

    fresh(started);
    CHECK(begin_confirmed(BMB_COMMAND_PAUSE, 1, started));
    CHECK(send_part(0, later(started, BMB_COMMAND_TIMEOUT_MS - 1)));
    CHECK(ack_part(0, "SUCCESS", NULL, later(started, BMB_COMMAND_TIMEOUT_MS - 1)));
    CHECK(!model.ctrl_busy && !model.command.failed);

    /* Telemetry reaches its freshness limit exactly as the clock wraps to 0. */
    fresh(UINT32_MAX);
    model.last_report_ms = UINT32_MAX - (BMB_TELEMETRY_FRESH_MS - 1);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, UINT32_MAX));
    CHECK(!send_part(0, 0));
    CHECK(!model.ctrl_busy && model.command.sent_mask == 0);
    fresh(UINT32_MAX);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, UINT32_MAX));
    CHECK(send_part(0, 0));
    CHECK(ack_part(0, "SUCCESS", NULL, 0));
    CHECK(!model.ctrl_busy);

    /* Unsigned token and sequence limits are valid, including token zero. */
    fresh(START_MS);
    model.command.token = UINT32_MAX;
    static const uint32_t max_sequence[] = {UINT32_MAX};
    CHECK(bmb_command_stop_arm_locked(&model, START_MS));
    CHECK(bmb_command_begin_locked(&model, BMB_COMMAND_STOP, max_sequence, 1,
                                   START_MS));
    CHECK(model.command.token == 0);
    CHECK(!bmb_command_can_send_locked(&model, UINT32_MAX, 0, START_MS));
    CHECK(send_part(0, START_MS));
    CHECK(ack_part(0, "SUCCESS", NULL, START_MS));
    CHECK(!model.ctrl_busy);
    model.command.token = UINT32_MAX;
    bmb_command_cancel_locked(&model, NULL);
    CHECK(model.command.token == 0);
    CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(model.command.token == 1);
    CHECK(!bmb_command_can_send_locked(&model, UINT32_MAX, 0, START_MS));
}

static void test_rejection_reason_utf8_and_display(void)
{
    static const struct {
        const char *reason;
        const char *stored;
        const char *display;
    } cases[] = {
        {"打印机拒绝控制", "打印机拒绝控制", "被拒绝，请检查打印机"},
        {"打印机拒绝控制！", "打印机拒绝控制", "被拒绝，请检查打印机"},
        {"拒绝123456789012345\xF0\x9F\x9A\x80", "拒绝123456789012345", "被拒绝，请检查打印机"},
        {"printer rejected", "printer rejected", "被拒绝:printer rejected"},
        {"12345678901234567890123456789", "12345678901234567890123",
         "被拒绝:12345678901234567890"},
        {"bad\nrequest", "bad\nrequest", "被拒绝，请检查打印机"},
        {"", "未知原因", "被拒绝，请检查打印机"},
        {NULL, "未知原因", "被拒绝，请检查打印机"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fresh(START_MS);
        CHECK(begin_confirmed(BMB_COMMAND_PAUSE, 1, START_MS));
        CHECK(send_part(0, START_MS));
        CHECK(ack_part(0, "FAIL", cases[i].reason, START_MS));
        CHECK(!model.ctrl_busy && model.command.failed);
        /* Exact equality to complete codepoint strings checks both copies;
         * the old byte precision split the seventh Chinese codepoint. */
        CHECK(strcmp(model.command.reason, cases[i].stored) == 0);
        CHECK(strcmp(model.ctrl_status, cases[i].display) == 0);
    }
    fresh(START_MS);
    CHECK(begin_confirmed(BMB_COMMAND_LIGHT, 2, START_MS));
    CHECK(send_part(0, START_MS) && send_part(1, START_MS));
    CHECK(ack_part(0, "FAIL", cases[1].reason, START_MS));
    CHECK(model.ctrl_busy && strcmp(model.command.reason, cases[1].stored) == 0);
    CHECK(ack_part(1, "SUCCESS", NULL, START_MS));
    CHECK(!model.ctrl_busy && strcmp(model.command.reason, cases[1].stored) == 0);
    CHECK(strcmp(model.ctrl_status, cases[1].display) == 0);
}

static void test_stop_arm_prerequisites(void)
{
    CHECK(!bmb_command_stop_arm_locked(NULL, START_MS));
    CHECK(!bmb_command_stop_valid_locked(NULL, START_MS));
    bmb_command_stop_disarm_locked(NULL);
    fresh(START_MS);
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(!model.ctrl_busy && !model.stop_confirmation.armed);
    CHECK(!bmb_command_stop_valid_locked(&model, START_MS));

    for (bmb_print_state_t state = BMB_PRINT_IDLE; state <= BMB_PRINT_OTHER; ++state) {
        fresh(START_MS);
        model.print_state = state;
        model.session_epoch = 27;
        const bool allowed = state == BMB_PRINT_PREPARE || state == BMB_PRINT_SLICING ||
                             state == BMB_PRINT_RUNNING || state == BMB_PRINT_PAUSE;
        CHECK(bmb_command_stop_arm_locked(&model, START_MS) == allowed);
        CHECK(model.stop_confirmation.armed == allowed);
        CHECK(bmb_command_stop_valid_locked(&model, START_MS) == allowed);
        if (allowed) {
            CHECK(model.stop_confirmation.started_ms == START_MS);
            CHECK(model.stop_confirmation.session_epoch == 27);
            CHECK(model.stop_confirmation.print_state == (int)state);
            CHECK(strcmp(model.stop_confirmation.device_id, model.dev_id) == 0);
            CHECK(strcmp(model.stop_confirmation.task, model.task) == 0);
        }
    }
    for (unsigned rejected = 0; rejected < 7; ++rejected) {
        fresh(START_MS);
        CHECK(bmb_command_stop_arm_locked(&model, START_MS));
        switch (rejected) {
        case 0: model.link = BMB_LINK_OFFLINE; break;
        case 1: model.link = BMB_LINK_CONNECTING; break;
        case 2: model.link_resetting = true; break;
        case 3: model.state_received = false; break;
        case 4: model.dev_id[0] = '\0'; break;
        case 5: model.last_report_ms = START_MS - BMB_TELEMETRY_FRESH_MS; break;
        default: model.ctrl_busy = true; break;
        }
        CHECK(!bmb_command_stop_arm_locked(&model, START_MS));
        CHECK(!model.stop_confirmation.armed);
    }
    fresh(START_MS);
    model.has_task = false;
    model.task[0] = '\0';
    model.last_report_ms = START_MS - (BMB_TELEMETRY_FRESH_MS - 1);
    CHECK(bmb_command_stop_arm_locked(&model, START_MS));
    CHECK(bmb_command_stop_valid_locked(&model, START_MS));
    CHECK(model.stop_confirmation.task[0] == '\0');
    bmb_command_stop_disarm_locked(&model);
    CHECK(!model.stop_confirmation.armed && !begin(BMB_COMMAND_STOP, 1, START_MS));
    save_model();
    bmb_command_stop_disarm_locked(&model);
    CHECK(model_unchanged());
}

static void test_stop_guard_consumption(void)
{
    static const uint32_t invalid_sequence[] = {0};
    fresh(START_MS);
    CHECK(bmb_command_stop_arm_locked(&model, START_MS));
    save_model();
    CHECK(!bmb_command_begin_locked(&model, BMB_COMMAND_STOP, NULL, 1, START_MS));
    CHECK(!begin(BMB_COMMAND_STOP, 0, START_MS));
    CHECK(!begin(BMB_COMMAND_STOP, 2, START_MS));
    CHECK(!bmb_command_begin_locked(&model, BMB_COMMAND_STOP, invalid_sequence, 1, START_MS));
    CHECK(model_unchanged());
    CHECK(bmb_command_stop_valid_locked(&model, START_MS));
    CHECK(begin(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(model.ctrl_busy && !model.stop_confirmation.armed);
    CHECK(!bmb_command_stop_valid_locked(&model, START_MS));
    CHECK(send_part(0, START_MS));
    CHECK(ack_part(0, "SUCCESS", NULL, START_MS));
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
    CHECK(!model.ctrl_busy);

    /* An intervening command consumes the pending first tap too, even if it
     * completes before the user taps STOP again. */
    for (bmb_command_kind_t kind = BMB_COMMAND_PAUSE; kind <= BMB_COMMAND_LIGHT; ++kind) {
        if (kind == BMB_COMMAND_STOP) continue;
        fresh(START_MS);
        CHECK(bmb_command_stop_arm_locked(&model, START_MS));
        CHECK(begin(kind, 1, START_MS));
        CHECK(!model.stop_confirmation.armed);
        CHECK(send_part(0, START_MS));
        CHECK(ack_part(0, "SUCCESS", NULL, START_MS));
        CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS));
    }
    fresh(START_MS);
    CHECK(bmb_command_stop_arm_locked(&model, START_MS));
    bmb_command_cancel_locked(&model, NULL);
    CHECK(!model.stop_confirmation.armed);
}

static void test_stop_guard_context_changes(void)
{
    /* Exercise invalidation through validation, idle tick, and STOP begin.
     * Begin must repeat the guard after a successful earlier UI check. */
    for (unsigned entry = 0; entry < 3; ++entry) {
        for (unsigned changed = 0; changed < 13; ++changed) {
            fresh(START_MS);
            model.session_epoch = 4;
            CHECK(bmb_command_stop_arm_locked(&model, START_MS));
            CHECK(bmb_command_stop_valid_locked(&model, START_MS + 1));
            uint32_t check_ms = START_MS + 2;
            switch (changed) {
            case 0: snprintf(model.task, sizeof(model.task), "%s", "new-job.3mf"); break;
            case 1: ++model.session_epoch; break;
            case 2: snprintf(model.dev_id, sizeof(model.dev_id), "%s", "OTHER-PRINTER"); break;
            case 3: model.print_state = BMB_PRINT_PAUSE; break;
            case 4: model.link = BMB_LINK_OFFLINE; break;
            case 5: model.link = BMB_LINK_CONNECTING; break;
            case 6: model.link_resetting = true; break;
            case 7: model.state_received = false; break;
            case 8: model.dev_id[0] = '\0'; break;
            case 9: model.last_report_ms = check_ms - BMB_TELEMETRY_FRESH_MS; break;
            case 10: model.ctrl_busy = true; break;
            case 11: model.print_state = BMB_PRINT_FINISH; break;
            default: check_ms = START_MS + BMB_STOP_CONFIRM_MS; break;
            }
            if (entry == 0) {
                CHECK(!bmb_command_stop_valid_locked(&model, check_ms));
            } else if (entry == 1) {
                bmb_command_tick_locked(&model, check_ms);
            } else {
                CHECK(!begin(BMB_COMMAND_STOP, 1, check_ms));
            }
            CHECK(!model.stop_confirmation.armed);
            CHECK(model.command.kind != BMB_COMMAND_STOP && model.command.sent_mask == 0);
            /* Returning to the old context cannot revive a disarmed tap. */
            const bmb_stop_confirmation_t invalidated = model.stop_confirmation;
            fresh(check_ms);
            model.session_epoch = 4;
            model.stop_confirmation = invalidated;
            CHECK(!bmb_command_stop_valid_locked(&model, check_ms));
            CHECK(!begin(BMB_COMMAND_STOP, 1, check_ms));
        }
    }
}

static void test_stop_second_tap_new_job(void)
{
    fresh(START_MS);
    CHECK(bmb_command_stop_arm_locked(&model, START_MS));
    CHECK(bmb_command_stop_valid_locked(&model, START_MS + 1));
    /* The report owner can change A to B after the UI guard and before begin.
     * The command owner must reject B without enqueueing a STOP for it. */
    snprintf(model.task, sizeof(model.task), "%s", "job-B.3mf");
    model.last_report_ms = START_MS + 2;
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS + 2));
    CHECK(!model.ctrl_busy && !model.stop_confirmation.armed);
    CHECK(model.command.token == 0 && model.command.sent_mask == 0);
    CHECK(bmb_command_stop_arm_locked(&model, START_MS + 3));
    CHECK(strcmp(model.stop_confirmation.task, "job-B.3mf") == 0);
    CHECK(begin(BMB_COMMAND_STOP, 1, START_MS + 4));
    CHECK(model.ctrl_busy && strcmp(model.command.task, "job-B.3mf") == 0);
}

static void test_stop_guard_clock_boundaries(void)
{
    static const uint32_t starts[] = {0, START_MS, UINT32_MAX - UINT32_C(1999)};
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        const uint32_t started = starts[i];
        fresh(started);
        model.session_epoch = UINT32_MAX;
        CHECK(bmb_command_stop_arm_locked(&model, started));
        CHECK(model.stop_confirmation.started_ms == started);
        CHECK(bmb_command_stop_valid_locked(&model, later(started, BMB_STOP_CONFIRM_MS - 1)));
        CHECK(begin(BMB_COMMAND_STOP, 1, later(started, BMB_STOP_CONFIRM_MS - 1)));
        CHECK(!model.stop_confirmation.armed && model.ctrl_busy);

        fresh(started);
        CHECK(bmb_command_stop_arm_locked(&model, started));
        CHECK(!begin(BMB_COMMAND_STOP, 1, later(started, BMB_STOP_CONFIRM_MS)));
        CHECK(!model.stop_confirmation.armed && !model.ctrl_busy);
        model.last_report_ms = later(started, BMB_STOP_CONFIRM_MS + 1);
        CHECK(!begin(BMB_COMMAND_STOP, 1, model.last_report_ms));
    }
}

/* Command tests link only bmb_state.c/bmb_command.c, so compute a distinct
 * FNV-1a directly instead of calling bmb_json_string_hash (bmb_json.c is not
 * linked here). The point is that two long names with the same bounded display
 * copy still differ by task_hash. */
static uint32_t task_hash_direct(const char *name)
{
    uint32_t h = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h = (h ^ *p) * UINT32_C(16777619);
    }
    return h;
}

static void report_task(const char *name)
{
    /* Mirrors the report parser: a 55-byte display copy plus a full-name hash. */
    size_t n = strlen(name);
    if (n >= sizeof(model.task)) n = sizeof(model.task) - 1;
    memcpy(model.task, name, n);
    model.task[n] = '\0';
    model.task_hash = task_hash_direct(name);
}

static void test_task_hash_identity_invalidates_stop(void)
{
    char job_a[64], job_b[64];
    memset(job_a, 'a', 60);
    job_a[60] = '\0';
    memcpy(job_b, job_a, sizeof(job_a));
    job_b[58] = 'X';
    job_b[59] = 'Y';
    const uint32_t hash_a = task_hash_direct(job_a);
    const uint32_t hash_b = task_hash_direct(job_b);
    CHECK(hash_a != 0 && hash_b != 0 && hash_a != hash_b);

    /* Arm STOP for job A; the report owner then swaps A -> B. The bounded task
     * copy is byte-identical, so only task_hash can invalidate the first tap. */
    fresh(START_MS);
    report_task(job_a);
    CHECK(model.task_hash == hash_a);
    CHECK(bmb_command_stop_arm_locked(&model, START_MS));
    CHECK(model.stop_confirmation.task_hash == hash_a);
    char copy_a[sizeof(model.task)];
    memcpy(copy_a, model.task, sizeof(copy_a));
    report_task(job_b);
    CHECK(memcmp(copy_a, model.task, sizeof(copy_a)) == 0);
    CHECK(model.task_hash == hash_b);
    /* begin() repeats the guard under lock: B's hash must fail and disarm. */
    CHECK(!begin(BMB_COMMAND_STOP, 1, START_MS + 1));
    CHECK(!model.stop_confirmation.armed && !model.ctrl_busy);
    CHECK(model.command.sent_mask == 0);
    /* A fresh arm binds to B and can then proceed. */
    CHECK(bmb_command_stop_arm_locked(&model, START_MS + 2));
    CHECK(model.stop_confirmation.task_hash == hash_b);
    CHECK(begin(BMB_COMMAND_STOP, 1, START_MS + 3));
    CHECK(model.command.task_hash == hash_b && model.ctrl_busy);

    /* An in-flight command snapshot also fails when task_hash changes, through
     * both the idle tick and the can_send entry path. */
    for (unsigned use_tick = 0; use_tick < 2; ++use_tick) {
        fresh(START_MS);
        report_task(job_a);
        CHECK(begin_confirmed(BMB_COMMAND_STOP, 1, START_MS));
        const uint32_t token = model.command.token;
        CHECK(model.command.task_hash == hash_a);
        if (use_tick) CHECK(send_part(0, START_MS));
        report_task(job_b);
        CHECK(memcmp(copy_a, model.task, sizeof(copy_a)) == 0);
        CHECK(model.task_hash == hash_b);
        if (use_tick) {
            bmb_command_tick_locked(&model, START_MS + 1);
        } else {
            CHECK(!bmb_command_can_send_locked(&model, token, 0, START_MS + 1));
        }
        CHECK(!model.ctrl_busy && model.command.token != token);
        CHECK(!bmb_command_can_send_locked(&model, token, 0, START_MS + 1));
    }
}

int main(void)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } groups[] = {
        {"begin prerequisites and freshness", test_begin_prerequisites},
        {"begin validation and busy rejection", test_begin_validation_and_busy},
        {"pending state, sent bit, and fast ACK", test_pending_before_io_and_fast_ack},
        {"unsent expiry and exact 8-second boundary", test_unsent_expiry},
        {"cancel tokens prevent replay", test_cancel_tokens_prevent_replay},
        {"reconnect tokens prevent replay", test_reconnect_tokens_prevent_replay},
        {"ACK identity, verdict, duplicate, and unsent filters", test_ack_filters},
        {"success and failure verdicts", test_ack_verdicts},
        {"two light ACKs in either order", test_light_both_acks_and_reverse_order},
        {"light mixed failure in either order", test_light_mixed_failure},
        {"light first-only ACK timeout", test_light_first_only_timeout},
        {"publish failure cancels", test_publish_failure_cancels},
        {"tick/send cancel device, task, and link changes", test_tick_and_send_context_changes},
        {"stale telemetry prevents send", test_stale_telemetry_prevents_send},
        {"print-state change cancels STOP", test_print_state_change_cancels_stop},
        {"uint32 monotonic, token, and sequence boundaries", test_uint32_monotonic_and_token_wrap},
        {"UTF-8 rejection diagnostics and supported display text", test_rejection_reason_utf8_and_display},
        {"STOP arm prerequisites and explicit disarm", test_stop_arm_prerequisites},
        {"STOP confirmation sequence validation and consumption", test_stop_guard_consumption},
        {"STOP context invalidation through valid, tick, and begin", test_stop_guard_context_changes},
        {"STOP second tap cannot target a replacement job", test_stop_second_tap_new_job},
        {"STOP confirmation expiry, zero, and clock wrap", test_stop_guard_clock_boundaries},
        {"full task hash distinguishes long names for STOP and in-flight guards",
         test_task_hash_identity_invalidates_stop},
    };
    unsigned passed = 0;
    const unsigned count = (unsigned)(sizeof(groups) / sizeof(groups[0]));
    for (unsigned i = 0; i < count; ++i) {
        const unsigned before = failures;
        groups[i].run();
        const bool ok = failures == before;
        printf("%s %s\n", ok ? "PASS" : "FAIL", groups[i].name);
        if (ok) {
            ++passed;
        }
    }
    printf("bmb_command: %u/%u groups passed; %u checks; %u failures\n",
           passed, count, checks, failures);
    return failures == 0 ? 0 : 1;
}
