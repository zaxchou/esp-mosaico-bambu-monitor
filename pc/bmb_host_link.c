// SPDX-License-Identifier: Apache-2.0
// Host transport: replays a compressed print session (idle → heating → run →
// finish → offline → reconnect) so the shared UI and parser are exercised in
// the simulator without hardware.

#include "bmb_host_link.h"

#include "bmb_ctrl.h"
#include "bmb_link.h"
#include "bmb_json.h"
#include "bmb_report.h"
#include "bmb_state.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_TOPIC "device/AC1234567890/report"

typedef struct {
    int t_ms;
    int link;           /* bmb_link_t value, or -1 = unchanged */
    const char *json;
} replay_step_t;

static const replay_step_t STEPS[] = {
    {0, BMB_LINK_CONNECTING, NULL},
    {700, BMB_LINK_CONNECTING, NULL},
    {1200, -1, "{\"print\":{\"gcode_state\":\"IDLE\",\"nozzle_temper\":28,"
                "\"bed_temper\":24,\"chamber_temper\":25,\"model_id\":\"CNf0baae0571025\",\"wifi_signal\":\"-42dBm\",\"spd_lvl\":2,\"lights_report\":[{\"mode\":\"on\",\"node\":\"chamber_light\"}],"
                "\"ams\":{\"exist\":1,\"humidity\":\"4\",\"temp\":\"25.6\","
                "\"tray\":[{\"id\":0,\"remain\":72,\"tray_color\":\"F97316FF\",\"tray_type\":\"PLA\"},{\"id\":1,\"remain\":-1,\"tray_color\":\"FFFFFFFF\"},"
                "{\"id\":2,\"remain\":45,\"tray_color\":\"0EA5E9FF\",\"tray_type\":\"PLA\"},{\"id\":3,\"remain\":-1,\"tray_color\":\"FFFFFFFF\"}]}}}"},
    {2600, -1, "{\"print\":{\"gcode_state\":\"PREPARE\",\"mc_percent\":0,"
                "\"nozzle_temper\":205,\"nozzle_target_temper\":220,"
                "\"bed_temper\":55,\"bed_target_temper\":60}}"},
    {3800, -1, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":6,"
                "\"nozzle_temper\":218,\"bed_temper\":60,\"chamber_temper\":28,"
                "\"layer_num\":2,\"total_layers\":210,"
                "\"subtask_name\":\"benchy.gcode.3mf\"}}"},
    {5400, -1, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":14,"
                "\"nozzle_temper\":220,\"bed_temper\":60,\"chamber_temper\":29,"
                "\"layer_num\":6,\"total_layers\":210}}"},
    {7000, -1, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":27,\"mc_remaining_time\":83,"
                "\"nozzle_temper\":220,\"bed_temper\":60,\"chamber_temper\":30,"
                "\"layer_num\":15,\"total_layers\":210}}"},
    {8600, -1, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":43,"
                "\"nozzle_temper\":221,\"bed_temper\":60,\"chamber_temper\":31,"
                "\"layer_num\":28,\"total_layers\":210}}"},
    {10200, -1, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":61,"
                "\"nozzle_temper\":220,\"bed_temper\":60,\"chamber_temper\":31,"
                "\"layer_num\":47,\"total_layers\":210}}"},
    {11800, -1, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":79,"
                "\"nozzle_temper\":219,\"bed_temper\":60,\"chamber_temper\":32,"
                "\"layer_num\":71,\"total_layers\":210}}"},
    {13400, -1, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":93,"
                "\"nozzle_temper\":220,\"bed_temper\":60,\"chamber_temper\":32,"
                "\"layer_num\":108,\"total_layers\":210}}"},
    {14600, -1, "{\"print\":{\"gcode_state\":\"FINISH\",\"mc_percent\":100,"
                "\"nozzle_temper\":212,\"bed_temper\":58,\"chamber_temper\":33,"
                "\"layer_num\":210,\"total_layers\":210}}"},
    {16200, -1, "{\"print\":{\"gcode_state\":\"ERROR\",\"print_error\":134,"
                 "\"mc_percent\":100,\"nozzle_temper\":200}}"},
    {16800, -1, "{\"print\":{\"gcode_state\":\"IDLE\",\"print_error\":0}}"},
    {17400, BMB_LINK_OFFLINE, NULL},
    {19800, BMB_LINK_CONNECTING, NULL},
    {20800, BMB_LINK_CONNECTING, NULL},
    {21300, -1, "{\"print\":{\"gcode_state\":\"IDLE\",\"nozzle_temper\":180,"
                "\"bed_temper\":42,\"chamber_temper\":30}}"},
    {24000, 0, NULL}, /* loop point */
};

#define STEPS_COUNT (int)(sizeof(STEPS) / sizeof(STEPS[0]))
#define LOOP_MS 24000

static bmb_state_t *s_model;
static bool s_running;
static int s_elapsed;
static int s_cursor;
static int s_loop;
static uint32_t s_now;
/* Optional host-only report fixture for deterministic integration tests.
 * Changing the file feeds shared parsing and UI; no printer is contacted. */
static const char *s_report_file;
static char s_last_fixture[BMB_JSON_MAX_BYTES + 1U];
static char s_fixture_payload[BMB_JSON_MAX_BYTES + 1U];
static const char *s_clock_file;
static const char *s_transport_file;
static unsigned s_report_serial;

static void observe_transport(const char *event, int value)
{
    if (!s_transport_file || !*s_transport_file) return;
    FILE *fp = fopen(s_transport_file, "ab");
    if (!fp) return;
    fprintf(fp, "{\"event\":\"%s\",\"value\":%d,\"now_ms\":%" PRIu32
                ",\"report_serial\":%u,\"print_state\":%d}\n",
            event, value, s_now, s_report_serial,
            s_model ? (int)s_model->print_state : -1);
    fclose(fp);
}

/* Optional test clock: freeze or advance monotonic model time independently
 * of real rendering. Native timers and callbacks still execute normally. */
static void clock_tick(void)
{
    if (!s_clock_file) {
        s_now += 100;
        return;
    }
    FILE *fp = fopen(s_clock_file, "rb");
    if (!fp) return;
    char text[40];
    size_t n = fread(text, 1, sizeof(text) - 1, fp);
    const bool complete = !ferror(fp) && feof(fp);
    fclose(fp);
    if (!complete || !n) return;
    text[n] = '\0';
    char *end;
    unsigned long long value = strtoull(text, &end, 10);
    if (end == text) return;
    while (isspace((unsigned char)*end)) ++end;
    if (*end || value > UINT32_MAX || value < s_now) return;
    s_now = (uint32_t)value;
}

static void fixture_tick(void)
{
    FILE *fp = fopen(s_report_file, "rb");
    if (!fp) return;
    char *payload = s_fixture_payload;
    size_t n = fread(payload, 1, sizeof(s_fixture_payload) - 1, fp);
    const bool complete = !ferror(fp) && fgetc(fp) == EOF && !ferror(fp);
    fclose(fp);
    if (!complete || !n) return;
    payload[n] = '\0';
    if (strcmp(payload, s_last_fixture) == 0) return;
    const bool accepted = bmb_report_apply(s_model, DEVICE_TOPIC,
                                          sizeof(DEVICE_TOPIC) - 1,
                                          payload, n, s_now);
    memcpy(s_last_fixture, payload, n + 1);
    ++s_report_serial;
    observe_transport("report", accepted ? 1 : 0);
}
/* Native UI task owns this synthetic transport. Model command ownership and
 * matching are shared with the real transport; only printer I/O is simulated.
 * Separate LIGHT acknowledgements exercise its two-part aggregation. */
static struct {
    bmb_command_kind_t kind;
    uint32_t token;
    uint32_t sequences[BMB_COMMAND_PARTS];
    unsigned parts;
    unsigned next_ack;
    int left;
    int value;
} s_pending;
static uint32_t s_sequence;

static void pending_tick(void)
{
    bmb_state_lock();
    bmb_command_tick_locked(s_model, s_now);
    const bool active = s_model->ctrl_busy &&
                        s_model->command.token == s_pending.token;
    bmb_state_unlock();
    if (!active) {
        s_pending.left = 0;
        return;
    }
    if (s_pending.left <= 0 || --s_pending.left > 0) return;
    static const char *const COMMANDS[] = {
        "pause", "resume", "stop", "print_speed", "ledctrl"
    };
    const bool system = s_pending.kind == BMB_COMMAND_LIGHT;
    char json[160];
    snprintf(json, sizeof(json),
             "{\"%s\":{\"command\":\"%s\",\"sequence_id\":\"%" PRIu32
             "\",\"result\":\"SUCCESS\"}}",
             system ? "system" : "print", COMMANDS[s_pending.kind],
             s_pending.sequences[s_pending.next_ack]);
    bmb_report_apply(s_model, DEVICE_TOPIC, sizeof(DEVICE_TOPIC) - 1,
                     json, strlen(json), s_now);
    observe_transport("ack", (int)s_pending.kind);
    if (++s_pending.next_ack < s_pending.parts) {
        s_pending.left = 4;
        return;
    }
    bmb_state_lock();
    const bool confirmed = s_model->command.token == s_pending.token &&
                           !s_model->ctrl_busy &&
                           !s_model->command.failed &&
                           s_model->command.ack_mask == (1U << s_pending.parts) - 1;
    bmb_state_unlock();
    if (!confirmed) return;
    /* A simulated printer sends a separate telemetry update after success;
     * ACK-only traffic itself never changes freshness or online state. */
    switch (s_pending.kind) {
    case BMB_COMMAND_PAUSE:
    case BMB_COMMAND_RESUME:
        snprintf(json, sizeof(json), "{\"print\":{\"gcode_state\":\"%s\"}}",
                 s_pending.kind == BMB_COMMAND_PAUSE ? "PAUSE" : "RUNNING");
        break;
    case BMB_COMMAND_SPEED:
        snprintf(json, sizeof(json), "{\"print\":{\"spd_lvl\":%d}}", s_pending.value);
        break;
    case BMB_COMMAND_LIGHT:
        snprintf(json, sizeof(json), "{\"print\":{\"lights_report\":[{"
                 "\"node\":\"chamber_light\",\"mode\":\"%s\"}]}}",
                 s_pending.value ? "on" : "off");
        break;
    default:
        return;
    }
    bmb_report_apply(s_model, DEVICE_TOPIC, sizeof(DEVICE_TOPIC) - 1,
                     json, strlen(json), s_now);
}

void bmb_host_link_tick(void)
{
    if (!s_model) {
        /* The UI timer can fire before bmb_link_start attaches the model. */
        return;
    }
    clock_tick();
    if (!s_running) return;
    if (s_report_file) fixture_tick();
    pending_tick();
    if (s_report_file) return;
    s_elapsed += 100;
    if (s_elapsed >= LOOP_MS) {
        s_elapsed = 0;
        s_cursor = 0;
        ++s_loop;
    }
    while (s_cursor < STEPS_COUNT && STEPS[s_cursor].t_ms <= s_elapsed) {
        const replay_step_t *step = &STEPS[s_cursor];
        ++s_cursor;
        /* Host-demo only: play the FINISH alert once, then keep the loop on
         * RUNNING/IDLE so the settings and keypad stay usable. */
        if (s_loop >= 1 && step->json &&
            (strstr(step->json, "FINISH") || strstr(step->json, "ERROR"))) {
            continue;
        }
        if (step->link >= 0) {
            bmb_state_lock();
            s_model->link = (bmb_link_t)step->link;
            if (s_model->link != BMB_LINK_ONLINE) {
                bmb_command_cancel_locked(s_model, "连接已断开");
                s_pending.left = 0;
            }
            bmb_state_unlock();
        }
        if (step->json) {
            bmb_report_apply(s_model, DEVICE_TOPIC, sizeof(DEVICE_TOPIC) - 1,
                             step->json, strlen(step->json),
                             s_now);
        }
    }
}

void bmb_link_start(bmb_state_t *model)
{
    s_model = model;
    s_running = true;
    s_elapsed = 0;
    s_cursor = 0;
    s_loop = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    s_report_file = getenv("BMB_SIM_REPORT_FILE");
    s_clock_file = getenv("BMB_SIM_CLOCK_FILE");
    s_transport_file = getenv("BMB_SIM_TRANSPORT_FILE");
    s_report_serial = 0;
    s_last_fixture[0] = '\0';
    bmb_state_lock();
    model->link = BMB_LINK_CONNECTING;
    bmb_state_unlock();
}

void bmb_link_stop(void)
{
    if (!s_model) return;
    bmb_state_lock();
    bmb_command_cancel_locked(s_model, "已取消");
    s_model->link = BMB_LINK_OFFLINE;
    bmb_state_unlock();
    s_pending.left = 0;
    s_running = false;
    observe_transport("stop", 0);
}

bool bmb_link_restart(bmb_state_t *model)
{
    if (!model) return false;
    bmb_link_stop();
    bmb_state_lock();
    const uint32_t token = model->command.token;
    const bool canceled = strcmp(model->ctrl_status, "已取消") == 0;
    bmb_state_reset_session(model);
    model->command.token = token + 1U;
    if (canceled) snprintf(model->ctrl_status, sizeof(model->ctrl_status), "已取消");
    bmb_state_unlock();
    bmb_link_start(model);
    observe_transport("restart", 1);
    return true;
}

static bool begin_command(bmb_state_t *model, bmb_command_kind_t kind, int value)
{
    if (!model || model != s_model) return false;
    const unsigned parts = kind == BMB_COMMAND_LIGHT ? 2U : 1U;
    uint32_t sequences[BMB_COMMAND_PARTS];
    for (unsigned i = 0; i < parts; ++i) {
        if (++s_sequence == 0) ++s_sequence;
        sequences[i] = s_sequence;
    }
    bmb_state_lock();
    if (!bmb_command_begin_locked(model, kind, sequences, parts, s_now)) {
        bmb_state_unlock();
        return false;
    }
    s_pending.kind = kind;
    s_pending.token = model->command.token;
    memcpy(s_pending.sequences, sequences, parts * sizeof(sequences[0]));
    s_pending.parts = parts;
    s_pending.next_ack = 0;
    s_pending.value = value;
    for (unsigned i = 0; i < parts; ++i) {
        if (!bmb_command_can_send_locked(model, s_pending.token, i, s_now)) {
            bmb_command_cancel_locked(model, "已取消");
            bmb_state_unlock();
            return false;
        }
        bmb_command_sent_locked(model, s_pending.token, i, true);
    }
    s_pending.left = 12; /* synthetic printer latency, independent of test clock */
    bmb_state_unlock();
    return true;
}

bool bmb_ctrl_send(bmb_state_t *model, bmb_cmd_t cmd)
{
    if (cmd < BMB_CMD_PAUSE || cmd > BMB_CMD_STOP ||
        !begin_command(model, (bmb_command_kind_t)cmd, (int)cmd)) return false;
    observe_transport("command", (int)cmd);
    return true;
}

bool bmb_ctrl_set_speed(bmb_state_t *model, int level)
{
    if (level < 1 || level > BMB_SPEED_LEVELS ||
        !begin_command(model, BMB_COMMAND_SPEED, level)) return false;
    observe_transport("speed", level);
    return true;
}

bool bmb_ctrl_set_light(bmb_state_t *model, bool on)
{
    if (!begin_command(model, BMB_COMMAND_LIGHT, on ? 1 : 0)) return false;
    observe_transport("light", on ? 1 : 0);
    return true;
}

uint32_t bmb_now_ms(void)
{
    return s_now;
}
