// SPDX-License-Identifier: Apache-2.0
// Native regression for the Bambu report parser. gcc -std=c11 -O2, no IDF.
#include "bmb_report.h"
#include "bmb_state.h"
#include "bmb_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bmb_state_t model;
static uint32_t now;
static unsigned checks, groups, payloads, malformed;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        abort(); \
    } \
} while (0)

static void pass(const char *name)
{
    ++groups;
    printf("PASS %s\n", name);
}

static void reset(void)
{
    bmb_state_init(&model);
    now = 0;
}

static bool feed(const char *topic, const char *json)
{
    now += 100;
    ++payloads;
    return bmb_report_apply(&model, topic, strlen(topic), json, strlen(json), now);
}

static void test_full_width_errors(const char *topic)
{
    reset();
    CHECK(!model.state_received && !model.error_seen && !model.finish_seen);
    CHECK(model.error_generation == 0 && model.finish_generation == 0);
    CHECK(model.remaining_updated_ms == 0 && model.remain_min == -1);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"PAUSE\",\"print_error\":0}}"));
    static const uint32_t codes[] = {
        UINT32_C(32768), UINT32_C(65536), UINT32_C(50364433),
        UINT32_C(2147483647), UINT32_C(2147483648), UINT32_MAX,
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        char json[80];
        snprintf(json, sizeof(json), "{\"print\":{\"print_error\":%lu}}",
                 (unsigned long)codes[i]);
        CHECK(feed(topic, json));
        CHECK(model.print_error == codes[i] && model.error_seen);
        CHECK(model.error_generation == i + 1);
        model.error_seen = false;
        CHECK(feed(topic, json));
        CHECK(!model.error_seen && model.error_generation == i + 1);
    }
    static const char *const invalid[] = {
        "-1", "4294967296", "9999999999999999999999999999999999999",
        "1.5", "1e2", "\"134\"", "null", "true",
    };
    const uint32_t generation = model.error_generation;
    const uint32_t last_report = model.last_report_ms;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char json[100];
        snprintf(json, sizeof(json), "{\"print\":{\"print_error\":%s}}", invalid[i]);
        CHECK(!feed(topic, json));
        CHECK(model.print_error == UINT32_MAX && !model.error_seen);
        CHECK(model.error_generation == generation);
        CHECK(model.last_report_ms == last_report);
    }
    /* Absent and rejected values preserve the current error, including in
     * otherwise valid telemetry. Explicit zero resolves it. */
    CHECK(feed(topic, "{\"print\":{\"mc_percent\":12,\"print_error\":4294967296}}"));
    CHECK(model.percent == 12 && model.print_error == UINT32_MAX);
    CHECK(feed(topic, "{\"print\":{\"mc_percent\":13}}"));
    CHECK(model.print_error == UINT32_MAX && !model.error_seen);
    CHECK(feed(topic, "{\"print\":{\"print_error\":0}}"));
    CHECK(model.print_error == 0 && model.error_generation == generation);
    CHECK(feed(topic, "{\"print\":{\"print_error\":4294967295}}"));
    CHECK(model.error_seen && model.error_generation == generation + 1);
    printf("PASS full-width-errors\n");
}

static void test_signed_numbers(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"nozzle_temper\":25,"
                       "\"mc_remaining_time\":30,\"wifi_signal\":\"-42dBm\"}}"));
    CHECK(model.wifi_dbm == -42);
    const uint32_t last = model.last_report_ms;
    const uint32_t remaining_at = model.remaining_updated_ms;
    static const char *const invalid[] = {
        "2147483648", "-2147483649", "+2147483648",
        "9999999999999999999999999999999999999",
        "-9999999999999999999999999999999999999",
        "2147483647.5", "-2147483648.5",
        "2147483647.999999", "-2147483648.999999",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char json[180];
        snprintf(json, sizeof(json),
                 "{\"print\":{\"nozzle_temper\":%s,\"mc_remaining_time\":%s}}",
                 invalid[i], invalid[i]);
        CHECK(!feed(topic, json));
        CHECK(model.nozzle == 25 && model.remain_min == 30);
        CHECK(model.last_report_ms == last);
        CHECK(model.remaining_updated_ms == remaining_at);
    }
    /* Reject an overflowing field without losing the next valid field. */
    CHECK(feed(topic, "{\"print\":{\"nozzle_temper\":-2147483648.5,\"mc_percent\":17,"
                       "\"wifi_signal\":\"-9999999999999\"}}"));
    CHECK(model.nozzle == 25 && model.percent == 17 && model.wifi_dbm == -42);

    /* Exact signed limits and fractions rounding within them remain valid.
     * mc_percent clamps positives and preserves the last value for negatives,
     * avoiding dependence on narrowing huge temperatures to int16_t. */
    static const char *const valid[] = {
        "2147483647", "-2147483648", "2147483647.4", "-2147483648.4",
        "2147483646.5", "-2147483647.5",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        char json[80];
        CHECK(feed(topic, "{\"print\":{\"mc_percent\":7}}"));
        snprintf(json, sizeof(json), "{\"print\":{\"mc_percent\":%s}}", valid[i]);
        CHECK(feed(topic, json));
        CHECK(model.percent == (valid[i][0] == '-' ? 7 : 100));
    }
    CHECK(feed(topic, "{\"print\":{\"nozzle_temper\":-0.5,\"bed_temper\":-0.4,"
                       "\"chamber_temper\":0.5,\"nozzle_target_temper\":-1.5,"
                       "\"wifi_signal\":\"-43dBm\"}}"));
    CHECK(model.nozzle == -1 && model.bed == 0 && model.chamber == 1);
    CHECK(model.nozzle_target == -2 && model.wifi_dbm == -43);
    printf("PASS signed-number-bounds\n");
}

static void test_alert_transitions(const char *topic)
{
    reset();
    const char *finished = "{\"print\":{\"gcode_state\":\"FINISH\"}}";
    /* Millisecond zero is a valid first snapshot, not an uninitialized clock. */
    ++payloads;
    CHECK(bmb_report_apply(&model, topic, strlen(topic), finished,
                            strlen(finished), 0));
    CHECK(model.state_received && !model.finish_seen);
    CHECK(model.first_report_ms == 0 && model.finish_generation == 0);
    now = 20000;
    CHECK(feed(topic, finished));
    CHECK(!model.finish_seen && model.finish_generation == 0);
    CHECK(model.first_report_ms == 0);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"}}"));
    CHECK(feed(topic, finished));
    CHECK(model.finish_seen && model.finish_generation == 1);
    model.finish_seen = false;
    CHECK(feed(topic, finished));
    CHECK(!model.finish_seen && model.finish_generation == 1);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"}}"));
    CHECK(feed(topic, finished));
    const uint32_t displayed_finish = model.finish_generation;
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"}}"));
    CHECK(feed(topic, finished));
    CHECK(model.finish_seen && model.finish_generation == displayed_finish + 1);

    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"ERROR\",\"print_error\":134}}"));
    CHECK(model.state_received && !model.error_seen && model.error_generation == 0);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"ERROR\",\"print_error\":134}}"));
    CHECK(!model.error_seen && model.error_generation == 0);
    CHECK(feed(topic, "{\"print\":{\"print_error\":135}}"));
    CHECK(model.error_seen && model.error_generation == 1);
    const uint32_t displayed_error = model.error_generation;
    CHECK(feed(topic, "{\"print\":{\"print_error\":136}}"));
    CHECK(model.error_seen && model.error_generation == displayed_error + 1);
    model.error_seen = false;
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"print_error\":0}}"));
    CHECK(!model.error_seen && model.error_generation == 2);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"PAUSE\",\"print_error\":136}}"));
    CHECK(model.error_seen && model.error_generation == 3);
    model.error_seen = false;
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"ERROR\",\"print_error\":136}}"));
    CHECK(!model.error_seen && model.error_generation == 3);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"print_error\":0}}"));
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"ERROR\"}}"));
    CHECK(model.error_seen && model.error_generation == 4);
    CHECK(feed(topic, finished));
    CHECK(model.error_seen && model.finish_seen);
    CHECK(model.error_generation == 4 && model.finish_generation == 1);

    /* Initial ERROR without a code is suppressed, too. */
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"ERROR\"}}"));
    CHECK(!model.error_seen && model.error_generation == 0);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"}}"));
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"ERROR\"}}"));
    CHECK(model.error_seen && model.error_generation == 1);
    printf("PASS alert-transitions\n");
}

static void pending(const char *topic, bmb_command_kind_t kind,
                    const uint32_t *sequences, unsigned parts, bool sent)
{
    if (!model.state_received) CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"}}"));
    bmb_state_lock();
    CHECK(bmb_command_begin_locked(&model, kind, sequences, parts, now));
    const uint32_t token = model.command.token;
    if (sent) {
        for (unsigned i = 0; i < parts; ++i) {
            CHECK(bmb_command_can_send_locked(&model, token, i, now));
            bmb_command_sent_locked(&model, token, i, true);
        }
    }
    bmb_state_unlock();
}

static void test_ack_freshness(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"system\":{\"command\":\"ledctrl\",\"sequence_id\":1,\"result\":\"success\"}}"));
    CHECK(!model.ctrl_busy && !model.state_received);
    CHECK(model.first_report_ms == 0 && model.last_report_ms == 0);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"FINISH\"}}"));
    CHECK(!model.finish_seen && model.finish_generation == 0);

    /* A real IDLE snapshot can carry the previous task name. */
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"IDLE\",\"subtask_name\":\"keep.3mf\"}}"));
    const uint32_t light_sequence = 1;
    pending(topic, BMB_COMMAND_LIGHT, &light_sequence, 1, true);
    uint32_t last = model.last_report_ms;
    const uint32_t first = model.first_report_ms;
    CHECK(feed(topic, "{\"system\":{\"command\":\"ledctrl\",\"sequence_id\":1,\"result\":\"fail\"}}"));
    CHECK(!model.ctrl_busy && model.has_task && strcmp(model.task, "keep.3mf") == 0);
    CHECK(model.print_state == BMB_PRINT_IDLE && model.last_report_ms == last);
    CHECK(model.first_report_ms == first && model.state_received);

    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_remaining_time\":83}}"));
    const uint32_t pause_sequence = 2;
    pending(topic, BMB_COMMAND_PAUSE, &pause_sequence, 1, true);
    last = model.last_report_ms;
    const uint32_t remaining_at = model.remaining_updated_ms;
    model.link = BMB_LINK_OFFLINE;
    CHECK(feed(topic, "{\"print\":{\"command\":\"pause\",\"sequence_id\":\"2\",\"result\":\"SUCCESS\"}}"));
    CHECK(!model.ctrl_busy && model.link == BMB_LINK_OFFLINE);
    CHECK(model.last_report_ms == last && model.first_report_ms == first);
    CHECK(model.remain_min == 83 && model.remaining_updated_ms == remaining_at);
    CHECK(model.print_state == BMB_PRINT_RUNNING && model.has_task);
    CHECK(!model.finish_seen && !model.error_seen);
    pass("ack-freshness");
}

static void test_remaining_time(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"a.3mf\",\"mc_remaining_time\":83}}"));
    const uint32_t updated = model.remaining_updated_ms;
    CHECK(model.remain_min == 83 && updated == now);
    CHECK(feed(topic, "{\"print\":{\"mc_percent\":5}}"));
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"a.3mf\"}}"));
    CHECK(model.remain_min == 83 && model.remaining_updated_ms == updated);
    /* Minute-precision estimates may repeat, even across a long pause in
     * delivery. An explicit current estimate refreshes its receipt timestamp;
     * equality alone is not evidence that the printer sent a stale value. */
    now += 120000;
    CHECK(feed(topic, "{\"print\":{\"mc_remaining_time\":83}}"));
    CHECK(model.remain_min == 83 && model.remaining_updated_ms == now);
    const uint32_t repeated_at = model.remaining_updated_ms;
    CHECK(feed(topic, "{\"print\":{\"nozzle_temper\":220}}"));
    CHECK(model.remaining_updated_ms == repeated_at);
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"b.3mf\"}}"));
    CHECK(model.remain_min == -1 && model.remaining_updated_ms == repeated_at);
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"c.3mf\",\"mc_remaining_time\":40}}"));
    CHECK(model.remain_min == 40 && model.remaining_updated_ms == now);
    CHECK(feed(topic, "{\"print\":{\"mc_remaining_time\":0}}"));
    CHECK(model.remain_min == 0 && model.remaining_updated_ms == now);
    CHECK(feed(topic, "{\"print\":{\"mc_remaining_time\":-1}}"));
    CHECK(model.remain_min == -1 && model.remaining_updated_ms == now);
    CHECK(feed(topic, "{\"print\":{\"mc_remaining_time\":32768}}"));
    CHECK(model.remain_min == -1 && model.remaining_updated_ms == now);

    static const char *const terminal[] = {"IDLE", "FINISH", "ERROR"};
    for (size_t i = 0; i < sizeof(terminal) / sizeof(terminal[0]); ++i) {
        char json[100];
        reset();
        CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_remaining_time\":83}}"));
        const uint32_t remaining_at = model.remaining_updated_ms;
        snprintf(json, sizeof(json), "{\"print\":{\"gcode_state\":\"%s\"}}", terminal[i]);
        CHECK(feed(topic, json));
        CHECK(model.remain_min == -1 && model.remaining_updated_ms == remaining_at);
        CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"}}"));
        CHECK(model.remain_min == -1); /* never revive the old task's ETA */
        snprintf(json, sizeof(json),
                 "{\"print\":{\"gcode_state\":\"%s\",\"mc_remaining_time\":83}}", terminal[i]);
        CHECK(feed(topic, json));
        CHECK(model.remain_min == -1 && model.remaining_updated_ms == now);
    }
    printf("PASS remaining-time\n");
}

static void test_basic_reports(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"IDLE\",\"nozzle_temper\":28,"
                       "\"bed_temper\":24,\"chamber_temper\":25}}"));
    CHECK(model.print_state == BMB_PRINT_IDLE && model.percent == 0);
    CHECK(model.nozzle == 28 && model.bed == 24 && model.chamber == 25);
    CHECK(model.link == BMB_LINK_ONLINE && strcmp(model.dev_id, "AC1234567890") == 0);
    pass("idle");
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":6,"
                       "\"nozzle_temper\":218.4,\"nozzle_target_temper\":220,"
                       "\"bed_temper\":59.6,\"bed_target_temper\":60,"
                       "\"chamber_temper\":28.2,\"layer_num\":2,\"total_layers\":210,"
                       "\"subtask_name\":\"benchy.gcode.3mf\"}}"));
    CHECK(model.print_state == BMB_PRINT_RUNNING && model.percent == 6);
    CHECK(model.nozzle == 218 && model.nozzle_target == 220);
    CHECK(model.bed == 60 && model.bed_target == 60 && model.chamber == 28);
    CHECK(model.layer == 2 && model.layers == 210);
    CHECK(model.has_task && strcmp(model.task, "benchy.gcode.3mf") == 0);
    pass("running");
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"FINISH\",\"mc_percent\":100}}"));
    CHECK(model.print_state == BMB_PRINT_FINISH && model.finish_seen);
    CHECK(model.percent == 100 && model.finish_generation == 1);
    model.finish_seen = false;
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":99}}"));
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"FINISH\",\"mc_percent\":100}}"));
    CHECK(model.finish_seen && model.finish_generation == 2);
    pass("finish");
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"print_percent\":44,"
                       "\"unknown_nested\":{\"a\":[1,true,null,{\"b\":[]}]},\"weird\":null}}"));
    CHECK(model.percent == 44 && model.print_state == BMB_PRINT_RUNNING);
    pass("fallback");
    CHECK(!feed(topic, "{\"other\":{\"gcode_state\":\"ERROR\"}}"));
    CHECK(model.print_state == BMB_PRINT_RUNNING);
    pass("no-print-object");
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"PAUSE\"}}"));
    CHECK(model.print_state == BMB_PRINT_PAUSE);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"ERROR\"}}"));
    CHECK(model.print_state == BMB_PRINT_ERROR);
    pass("pause-error");
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\","
                       "\"subtask_name\":\"a\\\"b\\npart\\t\\\\end\\/\\b\\f\\r\"}}"));
    CHECK(strcmp(model.task, "a\"b\npart\t\\end/\b\f\r") == 0);
    pass("escaped-name");
}

static void test_ams_legacy(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"IDLE\",\"ams\":{"
                       "\"humidity\":\"4\",\"temp\":\"25.6\",\"tray\":[{\"id\":0,\"remain\":72},"
                       "{\"id\":1,\"remain\":-1},{\"id\":2,\"remain\":45},{\"id\":3,\"remain\":-1}]}}}"));
    CHECK(model.ams_present && model.ams_humidity == 4 && model.ams_temp == 26);
    CHECK(model.tray_remain[0] == 72 && model.tray_remain[1] == -1);
    CHECK(model.tray_remain[2] == 45 && model.tray_remain[3] == -1);
    pass("ams-direct");
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"IDLE\",\"ams\":{\"ams\":["
                       "{\"humidity\":\"6\",\"temp\":\"27.4\",\"tray\":["
                       "{\"id\":0,\"remain\":90,\"tray_color\":\"112233FF\"},{\"id\":1,\"remain\":10}]},"
                       "{\"humidity\":\"3\",\"temp\":\"24\",\"tray\":["
                       "{\"id\":\"3\",\"remain\":7,\"tray_type\":\"PETG\"},{\"id\":\"0\",\"remain\":33,"
                       "\"tray_color\":\"445566FF\"},{\"id\":1,\"remain\":5},{\"id\":7,\"remain\":-1}]}]}}}"));
    CHECK(model.ams_present && model.ams_humidity == 6 && model.ams_temp == 27);
    CHECK(model.tray_remain[0] == 90 && model.tray_remain[1] == 10);
    CHECK(model.tray_remain[4] == 33 && model.tray_remain[5] == 5);
    CHECK(model.tray_remain[6] == -1 && model.tray_remain[7] == 7);
    CHECK(strcmp(model.tray_color[0], "112233FF") == 0);
    CHECK(strcmp(model.tray_color[4], "445566FF") == 0);
    CHECK(strcmp(model.tray_type[7], "PETG") == 0);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"}}"));
    CHECK(model.ams_present && model.ams_humidity == 6);
    pass("ams-multi-unit");
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"tray\":["
                       "{\"id\":9,\"remain\":50,\"tray_color\":\"AABBCCDD\"},{\"id\":2,\"remain\":61,"
                       "\"tray_color\":\"00112233445566778899\"}]}}}"));
    CHECK(model.tray_remain[1] == 10 && model.tray_remain[2] == 61);
    CHECK(strcmp(model.tray_color[2], "00112233") == 0);
    pass("ams-bounds");
}

static void test_ack_results(const char *topic)
{
    const uint32_t sequence = 42;
    static const char *const verdicts[] = {"SUCCESS", "success", "OK", "FAIL", "FAILED", "ERROR", "fail"};
    for (size_t i = 0; i < sizeof(verdicts) / sizeof(verdicts[0]); ++i) {
        reset();
        pending(topic, BMB_COMMAND_PAUSE, &sequence, 1, true);
        char json[160];
        snprintf(json, sizeof(json), "{\"print\":{\"print_ack\":1,\"command\":\"pause\","
                 "\"sequence_id\":\"42\",\"result\":\"%s\",\"reason\":\"ERROR STATE\"}}", verdicts[i]);
        CHECK(feed(topic, json));
        CHECK(!model.ctrl_busy && model.command.ack_mask == 1);
        CHECK(strstr(model.ctrl_status, i < 3 ? "确认" : "拒绝") != NULL);
        if (i >= 3) CHECK(strstr(model.ctrl_status, "ERROR STATE") != NULL);
    }
    pass("print-ack-verdicts");
    reset();
    pending(topic, BMB_COMMAND_LIGHT, &sequence, 1, true);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"},\"system\":{\"command\":\"ledctrl\","
                       "\"sequence_id\":42,\"result\":\"fail\",\"reason\":\"json wrong format\"}}"));
    CHECK(!model.ctrl_busy && strstr(model.ctrl_status, "json wrong format") != NULL);
    pass("sys-ack");
    reset();
    pending(topic, BMB_COMMAND_PAUSE, &sequence, 1, true);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"command\":\"push_status\","
                       "\"sequence_id\":42,\"result\":\"SUCCESS\"}}"));
    CHECK(model.ctrl_busy && model.command.ack_mask == 0);
    CHECK(strcmp(model.ctrl_status, "发送中") == 0);
    CHECK(feed(topic, "{\"print\":{\"command\":\"pause\",\"sequence_id\":42,\"result\":\"FAIL\"}}"));
    CHECK(!model.ctrl_busy && strstr(model.ctrl_status, "拒绝") != NULL);
    pass("ack-semantics");
}

static void reject_unchanged(const char *topic, const char *json, size_t len)
{
    bmb_state_t before;
    memcpy(&before, &model, sizeof(before));
    ++payloads;
    ++malformed;
    CHECK(!bmb_report_apply(&model, topic, strlen(topic), json, len, now + 1));
    CHECK(memcmp(&before, &model, sizeof(model)) == 0);
}

static void test_malformed(const char *topic)
{
    reset();
    const uint32_t seq = 42;
    pending(topic, BMB_COMMAND_PAUSE, &seq, 1, true);
    CHECK(feed(topic, "{\"print\":{\"nozzle_temper\":25,\"mc_remaining_time\":30,\"print_error\":0}}"));
    static const char *const bad[] = {
        "{\"print\":{\"lights_report\":[}]}}", /* audited non-progress hang */
        "{\"print\":{\"ams\":{\"ams\":[}]}}}",
        "{\"print\":{\"ams\":{\"tray\":[}]}}}",
        "{\"print\":{\"gcode_state\":\"FINISH\",\"print_error\":134,\"nozzle_temper\":",
        "{\"print\":{\"mc_percent\":50},\"system\":{\"command\":\"ledctrl\"",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":42,\"result\":\"SUCCESS\"},\"tail\":[}",
        "{\"print\":{\"mc_percent\":50}} trailing",
        "{\"print\":{\"mc_percent\":50}}{}",
        "{\"print\":{\"mc_percent\":50},}",
        "{\"print\":{,\"mc_percent\":50}}",
        "{\"print\":{\"mc_percent\":50,,\"bed_temper\":30}}",
        "{\"print\":{\"mc_percent\" 50}}",
        "{\"print\":{\"mc_percent\"::50}}",
        "{\"print\":{\"mc_percent\":50 \"bed_temper\":30}}",
        "{\"print\":{\"mc_percent\":}}",
        "{\"print\":{\"mc_percent\":50,\"u\":[1,]}}",
        "{\"print\":{\"mc_percent\":50,\"u\":[,1]}}",
        "{\"print\":{\"mc_percent\":50,\"u\":[1 2]}}",
        "{\"print\":{\"mc_percent\":50,\"u\":[{]}}}",
        "{\"print\":{\"mc_percent\":50,\"u\":tru}}",
        "{\"print\":{\"mc_percent\":50,\"u\":falsex}}",
        "{\"print\":{\"mc_percent\":50,\"u\":NULL}}",
        "{\"print\":{\"mc_percent\":+50}}",
        "{\"print\":{\"mc_percent\":050}}",
        "{\"print\":{\"mc_percent\":.5}}",
        "{\"print\":{\"mc_percent\":5.}}",
        "{\"print\":{\"mc_percent\":5e}}",
        "{\"print\":{\"mc_percent\":5e+}}",
        "{\"print\":{\"mc_percent\":NaN}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"bad\\x\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"bad\\u123\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"bad\\uXY12\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"bad\\uD800\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"bad\\uDC00\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"bad\\uD800\\u0041\"}}",
        "{\"print\":{\"mc_percent\":50,\"subtask_name\":\"raw\nnewline\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"\x01\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"\xC0\xAF\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"\xED\xA0\x80\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"\xF4\x90\x80\x80\"}}",
        "{\"print\":{\"mc_percent\":50,\"u\":\"\xE2\x82\"}}",
        "", " ", "{", "[", "{\"print\":[]}", "null", "true", "[]",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reject_unchanged(topic, bad[i], strlen(bad[i]));
    }
    const char nul[] = "{\"print\":{\"mc_percent\":50}}\0{}";
    reject_unchanged(topic, nul, sizeof(nul) - 1);
    const char raw_nul[] = "{\"print\":{\"mc_percent\":50,\"u\":\"a\0b\"}}";
    reject_unchanged(topic, raw_nul, sizeof(raw_nul) - 1);
    pass("malformed-atomic-no-hang");
}

static void test_all_truncations(const char *topic)
{
    reset();
    const uint32_t seq = 42;
    pending(topic, BMB_COMMAND_PAUSE, &seq, 1, true);
    static const char document[] =
        "{\"print\":{\"gcode_state\":\"FINISH\",\"subtask_name\":\"done\\\"\\n\\u6D4B\\uD83D\\uDE80\","
        "\"print_error\":134,\"nozzle_temper\":28.5,\"ams\":{\"ams\":[{\"id\":1,\"tray\":["
        "{\"id\":0,\"remain\":33,\"tray_type\":\"PLA\"}]}]},\"lights_report\":[{\"node\":\"chamber_light\","
        "\"mode\":\"on\"}],\"command\":\"pause\",\"sequence_id\":42,\"result\":\"SUCCESS\"}}";
    for (size_t n = 0; n < sizeof(document) - 1; ++n) reject_unchanged(topic, document, n);
    CHECK(feed(topic, document));
    CHECK(model.finish_generation == 1 && model.error_generation == 1);
    CHECK(model.nozzle == 29 && model.light_on == 1 && model.tray_remain[4] == 33);
    CHECK(!model.ctrl_busy);
    pass("every-truncation-atomic");
}

static void test_top_level(const char *topic)
{
    reset();
    CHECK(!feed(topic, "{\"nested\":{\"print\":{\"gcode_state\":\"FINISH\"}}}"));
    CHECK(!feed(topic, "{\"text\":\"\\\"print\\\":{\\\"gcode_state\\\":\\\"FINISH\\\"}\"}"));
    CHECK(!model.state_received && model.last_report_ms == 0);
    CHECK(feed(topic, "{\"nested\":{\"print\":{\"gcode_state\":\"ERROR\"}},\"print\"\t:\r\n"
                       "{\"gcode_state\":\"RUNNING\",\"mc_percent\":12}}"));
    CHECK(model.print_state == BMB_PRINT_RUNNING && model.percent == 12);
    CHECK(feed(topic, "{\"pr\\u0069nt\":{\"mc_perc\\u0065nt\":13}}"));
    CHECK(model.percent == 13);
    CHECK(feed(topic, "{\"a\\\"print\":{\"mc_percent\":99},\"print\":{\"mc_percent\":14}}"));
    CHECK(model.percent == 14);
    pass("structural-top-level-and-escaped-keys");
}

static void test_unicode_and_bounds(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"\\u6D4B\\u8BD5\\uD83D\\uDE80\","
                       "\"nozzle_temper\":2.185e2,\"bed_temper\":6e1,\"mc_percent\":1e1}}"));
    CHECK(strcmp(model.task, "\xE6\xB5\x8B\xE8\xAF\x95\xF0\x9F\x9A\x80") == 0);
    CHECK(model.nozzle == 219 && model.bed == 60 && model.percent == 10);
    CHECK(feed(topic, "{\"print\":{\"gcode_file\":\"fallback.3mf\",\"layer_num\":999999,\"nozzle_temper\":40000}}"));
    CHECK(strcmp(model.task, "fallback.3mf") == 0);
    CHECK(model.layer == UINT16_MAX && model.nozzle == INT16_MAX);
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\u6D4B\"}}"));
    CHECK(strlen(model.task) == 54); /* no partial UTF-8 at the 55-byte limit */
    const char *empty = "{\"print\":{\"subtask_name\":\"a\\u0000b\"}}";
    CHECK(!feed(topic, empty));
    CHECK(strlen(model.task) == 54);
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"\"}}"));
    CHECK(!model.has_task && model.task[0] == '\0');
    pass("unicode-exponents-and-field-bounds");

    char nested[256];
    size_t used = 0;
    const char *prefix = "{\"print\":{\"mc_percent\":17,\"u\":";
    memcpy(nested, prefix, strlen(prefix));
    used = strlen(prefix);
    for (unsigned i = 0; i < BMB_JSON_MAX_DEPTH - 2; ++i) nested[used++] = '[';
    nested[used++] = '0';
    for (unsigned i = 0; i < BMB_JSON_MAX_DEPTH - 2; ++i) nested[used++] = ']';
    nested[used++] = '}'; nested[used++] = '}'; nested[used] = '\0';
    CHECK(feed(topic, nested));
    CHECK(model.percent == 17);
    memmove(nested + strlen(prefix) + 1, nested + strlen(prefix), used - strlen(prefix));
    nested[strlen(prefix)] = '[';
    nested[used - 1] = ']'; nested[used] = '}'; nested[used + 1] = '}';
    reject_unchanged(topic, nested, used + 2);
    pass("depth-limit");

    char *large = malloc(BMB_JSON_MAX_BYTES + 1U);
    CHECK(large != NULL);
    const char *valid = "{\"print\":{\"mc_percent\":18}}";
    size_t n = strlen(valid);
    memcpy(large, valid, n);
    memset(large + n, ' ', BMB_JSON_MAX_BYTES + 1U - n);
    ++payloads;
    CHECK(bmb_report_apply(&model, topic, strlen(topic), large, BMB_JSON_MAX_BYTES, now));
    CHECK(model.percent == 18);
    reject_unchanged(topic, large, BMB_JSON_MAX_BYTES + 1U);
    free(large);
    pass("payload-size-limit");
}

static void test_ams_deltas(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":0,\"tray\":[{\"id\":0,\"remain\":90}]},"
                       "{\"id\":\"1\",\"tray\":[{\"id\":0,\"remain\":44,\"tray_type\":\"PLA\","
                       "\"tray_color\":\"F97316FF\"}]}]}}}"));
    CHECK(model.tray_remain[0] == 90 && model.tray_remain[4] == 44);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":1,\"tray\":[{\"id\":0,\"remain\":33}]}]}}}"));
    CHECK(model.tray_remain[0] == 90 && model.tray_remain[4] == 33);
    CHECK(strcmp(model.tray_type[4], "PLA") == 0 && strcmp(model.tray_color[4], "F97316FF") == 0);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":1,\"tray\":[{\"id\":0,\"remain\":32}]},"
                       "{\"id\":0,\"tray\":[{\"id\":0,\"remain\":89}]}]}}}"));
    CHECK(model.tray_remain[0] == 89 && model.tray_remain[4] == 32);
    static const char *const invalid_ids[] = {"-1", "2", "4294967296", "\"bad\"", "null", "1.5"};
    for (size_t i = 0; i < sizeof(invalid_ids) / sizeof(invalid_ids[0]); ++i) {
        char json[160];
        snprintf(json, sizeof(json), "{\"print\":{\"ams\":{\"ams\":[{\"id\":%s,"
                 "\"tray\":[{\"id\":0,\"remain\":1}]}]}}}", invalid_ids[i]);
        CHECK(feed(topic, json));
        CHECK(model.tray_remain[0] == 89 && model.tray_remain[4] == 32);
    }
    pass("ams-explicit-unit-ids-and-reordered-deltas");
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":1,\"tray\":[{\"id\":0,"
                       "\"tray_type\":\"\",\"tray_color\":\"\"}]}]}}}"));
    CHECK(model.tray_type[4][0] == '\0' && model.tray_color[4][0] == '\0');
    CHECK(model.tray_remain[4] == 32);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":1,\"tray\":[{\"id\":0,"
                       "\"tray_type\":\"PETG\",\"tray_color\":\"123456FF\"}]}]}}}"));
    CHECK(model.tray_remain[4] == 32 && strcmp(model.tray_type[4], "PETG") == 0);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":1,\"tray\":[{\"id\":0,"
                       "\"tray_type\":null,\"tray_color\":null,\"remain\":null}]}]}}}"));
    CHECK(model.tray_type[4][0] == '\0' && model.tray_color[4][0] == '\0' && model.tray_remain[4] == -1);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"tray\":[{\"tray_type\":\"PLA\"},{\"remain\":55}]}}}"));
    CHECK(model.tray_remain[0] == 89 && strcmp(model.tray_type[0], "PLA") == 0);
    CHECK(model.tray_remain[1] == 55); /* ID-less position does not depend on remain presence */
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"tray\":null}}}"));
    CHECK(model.tray_type[0][0] == '\0' && model.tray_remain[1] == -1);
    CHECK(feed(topic, "{\"print\":{\"ams\":null}}"));
    CHECK(!model.ams_present && model.ams_humidity == -1 && model.ams_temp == INT16_MIN);
    for (unsigned i = 0; i < BMB_AMS_TRAYS; ++i) CHECK(model.tray_remain[i] == -1 && model.tray_type[i][0] == '\0');
    pass("ams-field-presence-explicit-clear-and-removal");
}

static void test_alert_snapshots(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"nozzle_temper\":25}}"));
    CHECK(!model.state_received && model.link == BMB_LINK_ONLINE);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"FINISH\",\"subtask_name\":\"initial\"}}"));
    CHECK(model.state_received && !model.finish_seen && model.finish_generation == 0);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"old\"}}"));
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"FINISH\",\"subtask_name\":\"finished\",\"print_error\":134}}"));
    CHECK(model.finish_generation == 1 && model.error_generation == 1);
    CHECK(strcmp(model.finish_task, "finished") == 0 && strcmp(model.error_task, "finished") == 0);
    CHECK(model.alert_error_code == 134);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"new\",\"print_error\":0}}"));
    CHECK(model.finish_seen && model.error_seen && model.print_error == 0);
    CHECK(strcmp(model.finish_task, "finished") == 0 && strcmp(model.error_task, "finished") == 0);
    CHECK(model.alert_error_code == 134);
    CHECK(feed(topic, "{\"print\":{\"print_error\":135,\"subtask_name\":\"new error\"}}"));
    CHECK(model.error_generation == 2 && model.alert_error_code == 135);
    CHECK(strcmp(model.error_task, "new error") == 0 && strcmp(model.finish_task, "finished") == 0);
    pass("initial-state-snapshot-and-stable-alert-payloads");
}

static void test_ack_matching(const char *topic)
{
    const uint32_t sequence = UINT32_MAX;
    static const char *const ignored[] = {
        "{\"system\":{\"command\":\"ledctrl\",\"sequence_id\":4294967295,\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"command\":\"stop\",\"sequence_id\":4294967295,\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":99,\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"command\":\"pause\",\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"print_ack\":1}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":4294967295}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":4294967295,\"result\":\"\"}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":4294967295,\"result\":\"maybe\"}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":4294967295,\"result\":true}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":4294967296,\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":\"4294967296\",\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":\"1x\",\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":-1,\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":1.5,\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":1e2,\"result\":\"SUCCESS\"}}",
        "{\"nested\":{\"print\":{\"command\":\"pause\",\"sequence_id\":4294967295,\"result\":\"SUCCESS\"}}}",
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        reset();
        pending(topic, BMB_COMMAND_PAUSE, &sequence, 1, true);
        bmb_state_t before;
        memcpy(&before, &model, sizeof(before));
        (void)feed(topic, ignored[i]);
        CHECK(memcmp(&before, &model, sizeof(model)) == 0);
    }
    reset();
    pending(topic, BMB_COMMAND_PAUSE, &sequence, 1, false);
    const char *ack = "{\"print\":{\"command\":\"pause\",\"sequence_id\":\"4294967295\",\"result\":\"SUCCESS\"}}";
    CHECK(feed(topic, ack) && model.ctrl_busy && model.command.ack_mask == 0);
    CHECK(bmb_command_can_send_locked(&model, model.command.token, 0, now));
    bmb_command_sent_locked(&model, model.command.token, 0, true);
    CHECK(feed("device/OTHER/report", ack) && model.ctrl_busy);
    CHECK(feed(topic, ack) && !model.ctrl_busy && model.command.ack_mask == 1);
    bmb_state_t done;
    memcpy(&done, &model, sizeof(done));
    CHECK(feed(topic, ack));
    CHECK(memcmp(&done, &model, sizeof(done)) == 0);
    pass("ack-matches-family-command-sequence-device-and-sent-part");

    reset();
    const uint32_t parts[] = {7, 8};
    pending(topic, BMB_COMMAND_LIGHT, parts, 2, true);
    CHECK(feed(topic, "{\"system\":{\"command\":\"ledctrl\",\"sequence_id\":\"8\",\"result\":\"fail\",\"reason\":\"second light\"}}"));
    CHECK(model.ctrl_busy && model.command.ack_mask == 2);
    CHECK(feed(topic, "{\"system\":{\"command\":\"ledctrl\",\"sequence_id\":8,\"result\":\"SUCCESS\"}}"));
    CHECK(model.ctrl_busy && model.command.ack_mask == 2);
    CHECK(feed(topic, "{\"system\":{\"command\":\"ledctrl\",\"sequence_id\":7,\"result\":\"SUCCESS\"}}"));
    CHECK(!model.ctrl_busy && model.command.ack_mask == 3);
    CHECK(strstr(model.ctrl_status, "second light") != NULL);
    pass("two-part-ack-aggregation");
}

static void test_report_fallbacks(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":21,\"total_layers\":100}}"));
    CHECK(feed(topic, "{\"print\":{\"mc_percent\":-1,\"print_percent\":42,"
                       "\"total_layers\":0,\"total_layer_num\":210}}"));
    CHECK(model.percent == 42 && model.layers == 210);
    CHECK(feed(topic, "{\"print\":{\"mc_percent\":-2147483648,\"print_percent\":43,"
                       "\"total_layers\":-1,\"total_layer_num\":211}}"));
    CHECK(model.percent == 43 && model.layers == 211);
    CHECK(feed(topic, "{\"print\":{\"mc_percent\":0,\"print_percent\":99,"
                       "\"total_layers\":212,\"total_layer_num\":999}}"));
    CHECK(model.percent == 0 && model.layers == 212);
    CHECK(feed(topic, "{\"print\":{\"mc_percent\":null,\"print_percent\":44,"
                       "\"total_layers\":\"bad\",\"total_layer_num\":213}}"));
    CHECK(model.percent == 44 && model.layers == 213);
    CHECK(feed(topic, "{\"print\":{\"mc_percent\":-1,\"print_percent\":null,\"total_layers\":0}}"));
    CHECK(model.percent == 44 && model.layers == 0);
    CHECK(feed(topic, "{\"print\":{\"total_layer_num\":65536,\"layer_num\":0}}"));
    CHECK(model.layers == UINT16_MAX && model.layer == 0);
    pass("percent-and-total-layer-sentinel-fallbacks");
}

static void test_task_precedence(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"named task\","
                       "\"gcode_file\":\"plate.gcode\",\"mc_remaining_time\":40}}"));
    CHECK(strcmp(model.task, "named task") == 0 && model.has_task);
    const uint32_t updated = model.remaining_updated_ms;
    /* An explicit empty primary is a clear, even with a nonempty fallback. */
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"\",\"gcode_file\":\"plate.gcode\"}}"));
    CHECK(!model.has_task && model.task[0] == '\0');
    CHECK(model.remain_min == -1 && model.remaining_updated_ms == updated);
    CHECK(feed(topic, "{\"print\":{\"gcode_file\":\"fallback.gcode\",\"mc_remaining_time\":30}}"));
    CHECK(model.has_task && strcmp(model.task, "fallback.gcode") == 0);
    const uint32_t fallback_updated = model.remaining_updated_ms;
    CHECK(feed(topic, "{\"print\":{\"gcode_file\":\"\",\"mc_percent\":1}}"));
    CHECK(strcmp(model.task, "fallback.gcode") == 0 && model.remain_min == 30);
    CHECK(model.remaining_updated_ms == fallback_updated);
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":null,\"gcode_file\":\"next.gcode\"}}"));
    CHECK(strcmp(model.task, "next.gcode") == 0 && model.remain_min == -1);
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"next task\",\"gcode_file\":\"ignored\","
                       "\"mc_remaining_time\":20}}"));
    CHECK(strcmp(model.task, "next task") == 0 && model.remain_min == 20);
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"\",\"gcode_file\":\"\"}}"));
    CHECK(!model.has_task && model.remain_min == -1);
    pass("explicit-task-clear-and-filename-fallback-precedence");
}

static void test_failed_state(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"FAILED\",\"subtask_name\":\"initial\","
                       "\"mc_remaining_time\":10}}"));
    CHECK(model.print_state == BMB_PRINT_ERROR && model.remain_min == -1);
    CHECK(model.state_received && !model.error_seen && model.error_generation == 0);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_remaining_time\":10}}"));
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"FAILED\",\"subtask_name\":\"failed task\"}}"));
    CHECK(model.print_state == BMB_PRINT_ERROR && model.remain_min == -1);
    CHECK(model.error_seen && model.error_generation == 1);
    CHECK(strcmp(model.error_task, "failed task") == 0 && model.alert_error_code == 0);
    model.error_seen = false;
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"ERROR\"}}"));
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"FAILED\"}}"));
    CHECK(!model.error_seen && model.error_generation == 1);
    pass("failed-state-is-terminal-error");
}

static void test_ack_error_isolation(const char *topic)
{
    static const char *const replies[] = {
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":42,\"result\":\"FAIL\",\"print_error\":134}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":42,\"result\":\"SUCCESS\",\"print_error\":0}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":99,\"result\":\"FAIL\",\"print_error\":134}}",
        "{\"print\":{\"command\":\"pause\",\"print_error\":134}}",
    };
    const uint32_t sequence = 42;
    for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); ++i) {
        reset();
        CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"keep task\","
                           "\"mc_remaining_time\":83,\"print_error\":135}}"));
        pending(topic, BMB_COMMAND_PAUSE, &sequence, 1, true);
        const uint32_t last = model.last_report_ms;
        const uint32_t first = model.first_report_ms;
        const uint32_t remaining_at = model.remaining_updated_ms;
        if (i == 0) model.link = BMB_LINK_OFFLINE;
        const bmb_link_t link = model.link;
        (void)feed(topic, replies[i]);
        CHECK(model.link == link && model.last_report_ms == last);
        CHECK(model.first_report_ms == first && model.state_received);
        CHECK(model.print_error == 135 && !model.error_seen && model.error_generation == 0);
        CHECK(model.remain_min == 83 && model.remaining_updated_ms == remaining_at);
        CHECK(strcmp(model.task, "keep task") == 0 && model.print_state == BMB_PRINT_RUNNING);
        CHECK(model.ctrl_busy == (i >= 2));
    }
    reset();
    CHECK(feed(topic, replies[0]));
    CHECK(!model.state_received && model.link == BMB_LINK_OFFLINE);
    CHECK(model.last_report_ms == 0 && model.print_error == 0);
    /* push_status and error-only deltas are real telemetry; a command envelope
     * can also contain independent telemetry, which still applies as a unit. */
    CHECK(feed(topic, "{\"print\":{\"command\":\"push_status\",\"print_error\":134}}"));
    CHECK(model.print_error == 134 && model.last_report_ms == now);
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"print_error\":0}}"));
    CHECK(feed(topic, "{\"print\":{\"command\":\"pause\",\"sequence_id\":42,\"result\":\"FAIL\","
                       "\"gcode_state\":\"PAUSE\",\"print_error\":135}}"));
    CHECK(model.print_state == BMB_PRINT_PAUSE && model.print_error == 135);
    CHECK(model.error_seen && model.error_generation == 1 && model.last_report_ms == now);
    pass("command-error-codes-do-not-refresh-telemetry");
}

static void test_ams_sensor_selection(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":0,\"humidity\":null,\"temp\":null},"
                       "{\"id\":1,\"humidity\":\"5\",\"temp\":\"28.6\"}]}}}"));
    CHECK(model.ams_humidity == 5 && model.ams_temp == 29);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"humidity\":null,\"temp\":null,\"ams\":["
                       "{\"id\":1,\"humidity\":\"3\",\"temp\":\"26\"}]}}}"));
    CHECK(model.ams_humidity == 3 && model.ams_temp == 26);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":0,\"humidity\":\"4\",\"temp\":null},"
                       "{\"id\":1,\"humidity\":null,\"temp\":\"27\"}]}}}"));
    CHECK(model.ams_humidity == 4 && model.ams_temp == 27);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":0,\"humidity\":\"bad\",\"temp\":{}},"
                       "{\"id\":1,\"humidity\":\"2\",\"temp\":\"24\"}]}}}"));
    CHECK(model.ams_humidity == 2 && model.ams_temp == 24);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":0,\"humidity\":null,\"temp\":null},"
                       "{\"id\":1,\"humidity\":null,\"temp\":null}]}}}"));
    CHECK(model.ams_humidity == -1 && model.ams_temp == INT16_MIN);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"humidity\":\"6\",\"temp\":\"30\",\"ams\":["
                       "{\"id\":1,\"humidity\":\"2\",\"temp\":\"24\"}]}}}"));
    CHECK(model.ams_humidity == 6 && model.ams_temp == 30);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"ams\":[{\"id\":0,\"humidity\":-1,\"temp\":-32768},"
                       "{\"id\":1,\"humidity\":\"3\",\"temp\":\"25\"}]}}}"));
    CHECK(model.ams_humidity == 3 && model.ams_temp == 25);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"humidity\":-1,\"temp\":-32768}}}"));
    CHECK(model.ams_humidity == -1 && model.ams_temp == INT16_MIN);
    pass("ams-first-valid-sensors-and-null-clear");
}

static void test_material_name_capacity(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"tray\":[{\"id\":0,\"tray_type\":\"PLA-AERO\"},"
                       "{\"id\":1,\"tray_type\":\"ASA-AERO\"}]}}}"));
    CHECK(strcmp(model.tray_type[0], "PLA-AERO") == 0);
    CHECK(strcmp(model.tray_type[1], "ASA-AERO") == 0);
    CHECK(feed(topic, "{\"print\":{\"ams\":{\"tray\":[{\"id\":0,\"tray_type\":\"invalid\\u0000suffix\"},"
                       "{\"id\":1,\"tray_type\":99}]}}}"));
    CHECK(strcmp(model.tray_type[0], "PLA-AERO") == 0);
    CHECK(strcmp(model.tray_type[1], "ASA-AERO") == 0);
    pass("full-material-names-and-invalid-delta-preservation");
}

static void test_link_resetting(const char *topic)
{
    reset();
    const uint32_t sequence = 42;
    pending(topic, BMB_COMMAND_PAUSE, &sequence, 1, true);
    model.link_resetting = true;
    bmb_state_t before;
    memcpy(&before, &model, sizeof(before));
    static const char *const ignored[] = {
        "{\"print\":{\"gcode_state\":\"FINISH\",\"subtask_name\":\"old callback\",\"print_error\":134}}",
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":42,\"result\":\"SUCCESS\"}}",
        "{\"print\":{\"gcode_state\":\"FINISH\",\"print_error\":134,\"command\":\"pause\","
        "\"sequence_id\":42,\"result\":\"SUCCESS\"},\"system\":{\"command\":\"ledctrl\","
        "\"sequence_id\":42,\"result\":\"fail\"}}",
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        CHECK(!feed(topic, ignored[i]));
        CHECK(memcmp(&before, &model, sizeof(model)) == 0);
    }
    model.link_resetting = false;
    CHECK(feed(topic, ignored[1]));
    CHECK(!model.ctrl_busy && model.command.ack_mask == 1);
    pass("link-reset-suppresses-in-flight-telemetry-and-ack");
}

/* A reply envelope in either family quarantines a bare print_error: a system
 * ACK (ledctrl) must not raise a false printer error or refresh link freshness. */
static void test_reply_print_error_quarantine(const char *topic)
{
    reset();
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\"}}"));
    const uint32_t last = model.last_report_ms;
    const uint32_t error_generation = model.error_generation;
    const uint32_t remaining_at = model.remaining_updated_ms;
    const uint32_t print_error = model.print_error;
    const bmb_link_t link = model.link;
    const bool error_seen = model.error_seen;

    /* System-family ACK carrying only a print_error: fully quarantined. */
    CHECK(feed(topic, "{\"print\":{\"print_error\":134},"
                      "\"system\":{\"command\":\"ledctrl\",\"sequence_id\":1,"
                      "\"result\":\"FAIL\"}}"));
    CHECK(model.print_error == print_error && model.error_seen == error_seen);
    CHECK(model.error_generation == error_generation);
    CHECK(model.last_report_ms == last && model.link == link);
    CHECK(model.remaining_updated_ms == remaining_at);

    /* The equivalent print-family command reply is quarantined the same way. */
    CHECK(feed(topic, "{\"print\":{\"print_error\":134,\"command\":\"pause\","
                      "\"sequence_id\":1,\"result\":\"FAIL\"}}"));
    CHECK(model.print_error == print_error && model.error_seen == error_seen);
    CHECK(model.error_generation == error_generation);
    CHECK(model.last_report_ms == last && model.link == link);
    CHECK(model.remaining_updated_ms == remaining_at);

    /* Mixed independent telemetry + ACK: the code still applies as a unit. */
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\",\"print_error\":134},"
                      "\"system\":{\"command\":\"ledctrl\",\"sequence_id\":1,"
                      "\"result\":\"FAIL\"}}"));
    CHECK(model.print_error == 134 && model.error_seen);
    CHECK(model.error_generation == error_generation + 1);
    CHECK(model.last_report_ms == now && model.link == BMB_LINK_ONLINE);
    pass("reply-print-error-quarantine");
}

/* Two long subtask_names sharing the first 55 bytes have an identical bounded
 * display copy; only the full-name hash can signal the job change. */
static void test_long_task_hash_identity(const char *topic)
{
    char job_a[64], job_b[64], json[180];
    memset(job_a, 'a', 60);
    job_a[60] = '\0';
    memcpy(job_b, job_a, sizeof(job_a));
    job_b[58] = 'X';
    job_b[59] = 'Y';

    reset();
    /* A short task yields a nonzero, stable hash; repeating it keeps the hash. */
    CHECK(feed(topic, "{\"print\":{\"gcode_state\":\"RUNNING\","
                      "\"subtask_name\":\"short.3mf\"}}"));
    const uint32_t short_hash = model.task_hash;
    CHECK(short_hash != 0);
    CHECK(feed(topic, "{\"print\":{\"subtask_name\":\"short.3mf\"}}"));
    CHECK(model.task_hash == short_hash);

    snprintf(json, sizeof(json),
             "{\"print\":{\"subtask_name\":\"%s\",\"mc_remaining_time\":83}}", job_a);
    CHECK(feed(topic, json));
    const uint32_t hash_a = model.task_hash;
    CHECK(hash_a != 0 && hash_a != short_hash);
    CHECK(model.remain_min == 83 && strlen(model.task) == 55);
    char copy_a[sizeof(model.task)];
    memcpy(copy_a, model.task, sizeof(copy_a));

    /* Same 55-byte display copy, different full text => task change. */
    snprintf(json, sizeof(json), "{\"print\":{\"subtask_name\":\"%s\"}}", job_b);
    CHECK(feed(topic, json));
    CHECK(memcmp(copy_a, model.task, sizeof(copy_a)) == 0);
    CHECK(model.task_hash != hash_a);
    CHECK(model.remain_min == -1); /* new job without ETA invalidates stale ETA */

    const uint32_t hash_b = model.task_hash;
    CHECK(feed(topic, json));
    CHECK(model.task_hash == hash_b); /* repeating job B keeps its hash */
    snprintf(json, sizeof(json),
             "{\"print\":{\"subtask_name\":\"%s\",\"mc_remaining_time\":42}}", job_a);
    CHECK(feed(topic, json));
    CHECK(model.task_hash == hash_a && model.remain_min == 42);
    pass("long-task-name-full-hash-identity");
}

/* Direct check of bmb_json_string_hash over decoded escaped/UTF-8 text. */
static void test_json_string_hash_direct(void)
{
    static const char doc[] =
        "{\"a\":\"line\\nname\",\"b\":\"\\u6D4B\\u8BD5\\uD83D\\uDE80\","
        "\"c\":\"line\\nname\"}";
    bmb_json_span_t root, a, b, c;
    CHECK(bmb_json_validate(doc, sizeof(doc) - 1, &root));
    CHECK(bmb_json_object_get(root, "a", &a));
    CHECK(bmb_json_object_get(root, "b", &b));
    CHECK(bmb_json_object_get(root, "c", &c));
    uint32_t ha, hb, hc;
    CHECK(bmb_json_string_hash(a, &ha));
    CHECK(bmb_json_string_hash(b, &hb));
    CHECK(bmb_json_string_hash(c, &hc));
    CHECK(ha != 0 && hb != 0 && hc != 0);
    CHECK(ha == hc); /* equal decoded text is deterministic */
    CHECK(ha != hb); /* different UTF-8 text differs */
    uint32_t ignored = 0;
    CHECK(!bmb_json_string_hash(root, &ignored)); /* object is not a string */
    CHECK(!bmb_json_string_hash(a, NULL));
    pass("json-string-hash-direct");
}

int main(void)
{
    const char *topic = "device/AC1234567890/report";
    test_basic_reports(topic);
    test_ams_legacy(topic);
    test_ack_results(topic);
    test_full_width_errors(topic); ++groups;
    test_signed_numbers(topic); ++groups;
    test_alert_transitions(topic); ++groups;
    test_ack_freshness(topic);
    test_remaining_time(topic); ++groups;
    test_malformed(topic);
    test_all_truncations(topic);
    test_top_level(topic);
    test_unicode_and_bounds(topic);
    test_ams_deltas(topic);
    test_alert_snapshots(topic);
    test_ack_matching(topic);
    test_link_resetting(topic);
    test_report_fallbacks(topic);
    test_task_precedence(topic);
    test_failed_state(topic);
    test_ack_error_isolation(topic);
    test_ams_sensor_selection(topic);
    test_material_name_capacity(topic);
    test_reply_print_error_quarantine(topic);
    test_long_task_hash_identity(topic);
    test_json_string_hash_direct();
    printf("PASS bmb_report: %u groups, %u checks, %u payloads, %u rejection/atomicity cases\n",
           groups, checks, payloads, malformed);
    return 0;
}
