// SPDX-License-Identifier: Apache-2.0

#include "bmb_report.h"
#include "bmb_json.h"

#include <limits.h>
#include <string.h>

/* Validate the entire document before taking the model lock. Extraction then
 * applies presence-aware deltas under that lock, with no unlocked model reads
 * or large temporary snapshot. Invalid JSON cannot apply telemetry or ACKs. */

static bool object(bmb_json_span_t s) { return s.p < s.end && *s.p == '{'; }
static bool null_value(bmb_json_span_t s)
{
    return s.end - s.p == 4 && memcmp(s.p, "null", 4) == 0;
}

static bool number_field(bmb_json_span_t obj, const char *key, bool strings, int *n)
{
    bmb_json_span_t v;
    return bmb_json_object_get(obj, key, &v) && bmb_json_rounded(v, strings, n);
}

/* Prefer a usable primary; zero totals and negative progress are printer
 * placeholders. Without a usable fallback, retain the primary's sentinel
 * behavior rather than inventing a value. */
static bool number_fallback(bmb_json_span_t obj, const char *key, const char *fallback,
                            int minimum, int *n)
{
    bool have = number_field(obj, key, false, n);
    if (!have || *n < minimum) {
        int alternate;
        if (number_field(obj, fallback, false, &alternate) && (!have || alternate >= minimum)) {
            *n = alternate;
            have = true;
        }
    }
    return have;
}

static bool string_field(bmb_json_span_t obj, const char *key, char *out, size_t cap)
{
    bmb_json_span_t v;
    return bmb_json_object_get(obj, key, &v) &&
           bmb_json_string_copy(v, out, cap) != SIZE_MAX;
}

/* A task name needs both a bounded display copy and a hash of its full text:
 * distinct long names can share the same first 55 bytes, so comparing only the
 * copy would miss a job change (and could reuse a stale STOP confirmation). */
static bool task_field(bmb_json_span_t obj, const char *key, char *out, size_t cap,
                       uint32_t *hash)
{
    bmb_json_span_t v;
    if (!bmb_json_object_get(obj, key, &v)) return false;
    if (bmb_json_string_copy(v, out, cap) == SIZE_MAX ||
        !bmb_json_string_hash(v, hash)) return false;
    return true;
}

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static bool temperature(bmb_json_span_t obj, const char *key, int16_t *out)
{
    int n;
    if (!number_field(obj, key, false, &n)) return false;
    *out = (int16_t)clamp(n, INT16_MIN, INT16_MAX);
    return true;
}

static bool layers(bmb_json_span_t obj, const char *key, uint16_t *out)
{
    int n;
    if (!number_field(obj, key, false, &n)) return false;
    *out = (uint16_t)clamp(n, 0, UINT16_MAX);
    return true;
}

static void clear_tray(bmb_state_t *state, unsigned slot)
{
    state->tray_remain[slot] = -1;
    state->tray_type[slot][0] = '\0';
    state->tray_color[slot][0] = '\0';
}

static void clear_ams(bmb_state_t *state)
{
    state->ams_present = false;
    state->ams_humidity = -1;
    state->ams_temp = INT16_MIN;
    for (unsigned i = 0; i < BMB_AMS_TRAYS; ++i) clear_tray(state, i);
}

/* Optional material fields: absence preserves the last value; empty strings
 * and null explicitly clear it. A remaining-percent delta cannot erase them. */
static void tray_string(bmb_json_span_t tray, const char *key, char *out, size_t cap)
{
    bmb_json_span_t v;
    if (!bmb_json_object_get(tray, key, &v)) return;
    if (null_value(v)) {
        out[0] = '\0';
    } else {
        char text[BMB_FILAMENT_TYPE_MAX];
        if (cap > sizeof(text) || bmb_json_string_copy(v, text, cap) == SIZE_MAX) return;
        memcpy(out, text, strlen(text) + 1U);
    }
}

static void tray_color(bmb_json_span_t tray, char *out, size_t cap)
{
    bmb_json_span_t v;
    if (!bmb_json_object_get(tray, "tray_color", &v)) return;
    if (null_value(v)) { out[0] = '\0'; return; }
    char color[32];
    if (bmb_json_string_copy(v, color, sizeof(color)) == SIZE_MAX) return;
    size_t used = 0;
    for (const char *p = color; *p && used + 1 < cap; ++p) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') ||
            (*p >= 'a' && *p <= 'f')) {
            out[used++] = *p;
        }
    }
    out[used] = '\0';
}

/* An explicit invalid ID is never treated as an absent ID. Legacy ID-less
 * arrays use their position, independent of which optional fields were sent. */
static bool element_id(bmb_json_span_t obj, unsigned fallback, unsigned limit,
                        unsigned *id)
{
    bmb_json_span_t v;
    uint32_t n;
    if (bmb_json_object_get(obj, "id", &v)) {
        if (!bmb_json_uint32(v, true, &n) || n >= limit) return false;
        *id = (unsigned)n;
    } else {
        if (fallback >= limit) return false;
        *id = fallback;
    }
    return true;
}

/* Select humidity and temperature independently from the first known reading.
 * Explicit unknowns clear stale values but do not hide a later valid unit. */
static void ams_unit(bmb_state_t *state, bmb_json_span_t unit, unsigned unit_id,
                      bool *humidity_seen, bool *temp_seen)
{
    bmb_json_span_t v;
    int n;
    if (!*humidity_seen && bmb_json_object_get(unit, "humidity", &v)) {
        if (null_value(v)) {
            /* Null clears the aggregate unless a later unit has a reading. */
            state->ams_humidity = -1;
        } else if (bmb_json_rounded(v, true, &n)) {
            state->ams_humidity = (int8_t)clamp(n, -1, INT8_MAX);
            *humidity_seen = state->ams_humidity != -1;
        }
    }
    if (!*temp_seen && bmb_json_object_get(unit, "temp", &v)) {
        if (null_value(v)) {
            state->ams_temp = INT16_MIN;
        } else if (bmb_json_rounded(v, true, &n)) {
            state->ams_temp = (int16_t)clamp(n, INT16_MIN, INT16_MAX);
            *temp_seen = state->ams_temp != INT16_MIN;
        }
    }
    if (!bmb_json_object_get(unit, "tray", &v)) return;
    const unsigned base = unit_id * BMB_AMS_TRAYS_PER_UNIT;
    if (null_value(v)) {
        for (unsigned i = 0; i < BMB_AMS_TRAYS_PER_UNIT; ++i) clear_tray(state, base + i);
        return;
    }
    bmb_json_span_t cursor, tray;
    if (!bmb_json_array_begin(v, &cursor)) return;
    unsigned position = 0;
    while (bmb_json_array_next(&cursor, &tray)) {
        unsigned id;
        unsigned fallback = position++;
        if (!object(tray) || !element_id(tray, fallback, BMB_AMS_TRAYS_PER_UNIT, &id)) {
            continue;
        }
        const unsigned slot = base + id;
        if (bmb_json_object_get(tray, "remain", &v)) {
            if (null_value(v)) state->tray_remain[slot] = -1;
            else if (bmb_json_rounded(v, false, &n)) {
                state->tray_remain[slot] = (int16_t)clamp(n, -1, 100);
            }
        }
        tray_color(tray, state->tray_color[slot], sizeof(state->tray_color[slot]));
        tray_string(tray, "tray_type", state->tray_type[slot], sizeof(state->tray_type[slot]));
    }
}

static bool ams_apply(bmb_state_t *state, bmb_json_span_t print)
{
    bmb_json_span_t ams;
    if (!bmb_json_object_get(print, "ams", &ams)) return false;
    if (null_value(ams)) { clear_ams(state); return true; }
    if (!object(ams)) return false;
    state->ams_present = true;
    bool humidity_seen = false, temp_seen = false;
    /* The direct single-unit layout has no enclosing AMS-unit array. */
    unsigned id;
    if (element_id(ams, 0, BMB_AMS_TRAYS / BMB_AMS_TRAYS_PER_UNIT, &id)) {
        ams_unit(state, ams, id, &humidity_seen, &temp_seen);
    }
    bmb_json_span_t units, cursor, unit;
    if (!bmb_json_object_get(ams, "ams", &units) ||
        !bmb_json_array_begin(units, &cursor)) return true;
    unsigned position = 0;
    while (bmb_json_array_next(&cursor, &unit)) {
        unsigned fallback = position++;
        if (object(unit) && element_id(unit, fallback,
                                      BMB_AMS_TRAYS / BMB_AMS_TRAYS_PER_UNIT, &id)) {
            ams_unit(state, unit, id, &humidity_seen, &temp_seen);
        }
    }
    return true;
}

static bool light_apply(bmb_state_t *state, bmb_json_span_t print)
{
    bmb_json_span_t array, cursor, el, node, mode;
    if (!bmb_json_object_get(print, "lights_report", &array) ||
        !bmb_json_array_begin(array, &cursor)) return false;
    while (bmb_json_array_next(&cursor, &el)) {
        if (!bmb_json_object_get(el, "node", &node) ||
            !bmb_json_string_equal(node, "chamber_light") ||
            !bmb_json_object_get(el, "mode", &mode)) continue;
        if (bmb_json_string_equal(mode, "on")) state->light_on = 1;
        else if (bmb_json_string_equal(mode, "off")) state->light_on = 0;
        else continue;
        return true;
    }
    return false;
}

static void copy_dev_id(bmb_state_t *state, const char *topic, size_t topic_len)
{
    static const char prefix[] = "device/";
    static const char suffix[] = "/report";
    if (!topic || topic_len <= sizeof(prefix) + sizeof(suffix) - 2U ||
        memcmp(topic, prefix, sizeof(prefix) - 1U) != 0) return;
    const char *start = topic + sizeof(prefix) - 1U;
    size_t len = topic_len - (sizeof(prefix) - 1U) - (sizeof(suffix) - 1U);
    if (len == 0 || len >= sizeof(state->dev_id) ||
        memcmp(start + len, suffix, sizeof(suffix) - 1U) != 0 ||
        memchr(start, '/', len) || memchr(start, '\0', len)) return;
    memcpy(state->dev_id, start, len);
    state->dev_id[len] = '\0';
}

static bmb_print_state_t print_state(const char *raw)
{
    if (strcmp(raw, "IDLE") == 0) return BMB_PRINT_IDLE;
    if (strcmp(raw, "RUNNING") == 0) return BMB_PRINT_RUNNING;
    if (strcmp(raw, "PAUSE") == 0) return BMB_PRINT_PAUSE;
    if (strcmp(raw, "FINISH") == 0) return BMB_PRINT_FINISH;
    if (strcmp(raw, "SLICING") == 0) return BMB_PRINT_SLICING;
    if (strcmp(raw, "PREPARE") == 0) return BMB_PRINT_PREPARE;
    if (strcmp(raw, "ERROR") == 0 || strcmp(raw, "FAILED") == 0) return BMB_PRINT_ERROR;
    return BMB_PRINT_OTHER;
}

static bool wifi_apply(bmb_state_t *state, bmb_json_span_t print)
{
    char text[24];
    if (!string_field(print, "wifi_signal", text, sizeof(text))) return false;
    size_t len = strlen(text);
    if (len >= 3 && memcmp(text + len - 3, "dBm", 3) == 0) len -= 3;
    bmb_json_span_t v = {text, text + len};
    int n;
    if (!bmb_json_rounded(v, false, &n)) return false;
    state->wifi_dbm = (int16_t)clamp(n, INT16_MIN, INT16_MAX);
    return true;
}

static bool command_reply(bmb_json_span_t print)
{
    bmb_json_span_t command;
    return bmb_json_object_get(print, "command", &command) &&
           command.p < command.end && *command.p == '"' &&
           !bmb_json_string_equal(command, "") &&
           !bmb_json_string_equal(command, "push_status");
}

/* Called only while holding the model lock and after complete validation. */
static bool telemetry_apply(bmb_state_t *state, bmb_json_span_t print, uint32_t now_ms,
                            bool report_has_reply)
{
    const bmb_print_state_t previous = state->print_state;
    const uint32_t previous_error = state->print_error;
    const bool had_state = state->state_received;
    char raw[24] = {0};
    bool have_state = string_field(print, "gcode_state", raw, sizeof(raw)) && raw[0];
    if (have_state) state->print_state = print_state(raw);
    bool any = have_state;
    int n;
    if (number_fallback(print, "mc_percent", "print_percent", 0, &n)) {
        if (n >= 0) state->percent = (uint8_t)clamp(n, 0, 100);
        any = true;
    }
    any |= temperature(print, "nozzle_temper", &state->nozzle);
    any |= temperature(print, "nozzle_target_temper", &state->nozzle_target);
    any |= temperature(print, "bed_temper", &state->bed);
    any |= temperature(print, "bed_target_temper", &state->bed_target);
    any |= temperature(print, "chamber_temper", &state->chamber);
    any |= layers(print, "layer_num", &state->layer);
    if (number_fallback(print, "total_layers", "total_layer_num", 1, &n)) {
        state->layers = (uint16_t)clamp(n, 0, UINT16_MAX);
        any = true;
    }

    char task[sizeof(state->task)] = {0};
    uint32_t task_hash = 0;
    bool have_task = task_field(print, "subtask_name", task, sizeof(task), &task_hash);
    /* A valid primary, including an explicit empty clear, wins over gcode_file.
     * An empty fallback alone is not evidence that the task was removed. */
    if (!have_task) {
        char file[sizeof(state->task)] = {0};
        if (task_field(print, "gcode_file", file, sizeof(file), &task_hash) && file[0]) {
            memcpy(task, file, sizeof(task));
            have_task = true;
        }
    }
    const bool task_changed = have_task &&
        (task_hash != state->task_hash || strcmp(task, state->task) != 0);
    if (have_task) {
        memcpy(state->task, task, sizeof(state->task));
        state->task_hash = task_hash;
        state->has_task = task[0] != '\0';
        any = true;
    } else if (have_state && state->print_state == BMB_PRINT_IDLE) {
        state->task[0] = '\0';
        state->task_hash = 0;
        state->has_task = false;
    }

    if (number_field(print, "mc_remaining_time", false, &n)) {
        state->remain_min = (int16_t)(n < 0 || n > INT16_MAX ? -1 : n);
        /* Receipt of an explicit minute-resolution estimate refreshes it even
         * when equal: equality cannot distinguish an update from a replay. */
        state->remaining_updated_ms = now_ms;
        any = true;
    } else if (task_changed) {
        state->remain_min = -1;
    }
    any |= ams_apply(state, print);
    any |= light_apply(state, print);
    if (number_field(print, "spd_lvl", false, &n)) {
        state->spd_lvl = (int8_t)clamp(n, -1, INT8_MAX);
        any = true;
    }
    char model_id[sizeof(state->model_id)] = {0};
    if (string_field(print, "model_id", model_id, sizeof(model_id))) {
        memcpy(state->model_id, model_id, sizeof(state->model_id));
        any = true;
    }
    any |= wifi_apply(state, print);
    bmb_json_span_t v;
    uint32_t code;
    /* Command replies in either family (print.command or system.command) may
     * carry rejection codes in print_error. Alone they prove neither live
     * printer state nor telemetry freshness. The caller supplies whether ANY
     * family in this report is a reply, so a system ACK (e.g. ledctrl) cannot
     * smuggle a print_error into the model. Mixed telemetry/ACK and error-only
     * command-less deltas still apply. */
    if ((any || !report_has_reply) &&
        bmb_json_object_get(print, "print_error", &v) && bmb_json_uint32(v, false, &code)) {
        state->print_error = code;
        any = true;
    }
    if (!any) return false;
    if (state->print_state == BMB_PRINT_IDLE || state->print_state == BMB_PRINT_FINISH ||
        state->print_state == BMB_PRINT_ERROR) state->remain_min = -1;

    /* A temperature/AMS-only delta does not consume startup suppression. The
     * first actual gcode_state snapshot establishes the transition baseline. */
    if (had_state) {
        if (have_state && state->print_state == BMB_PRINT_FINISH && previous != BMB_PRINT_FINISH) {
            state->finish_seen = true;
            ++state->finish_generation;
            memcpy(state->finish_task, state->task, sizeof(state->finish_task));
        }
        const bool code_changed = state->print_error != 0 && state->print_error != previous_error;
        const bool entered_error = have_state && state->print_state == BMB_PRINT_ERROR &&
                                   previous != BMB_PRINT_ERROR && previous_error == 0;
        if (code_changed || entered_error) {
            state->error_seen = true;
            ++state->error_generation;
            memcpy(state->error_task, state->task, sizeof(state->error_task));
            state->alert_error_code = state->print_error;
        }
    } else if (have_state) {
        state->first_report_ms = now_ms;
    }
    if (have_state) state->state_received = true;
    state->last_report_ms = now_ms;
    state->link = BMB_LINK_ONLINE;
    return true;
}

static bool ack_apply(bmb_state_t *state, const char *topic, size_t topic_len,
                       bmb_json_span_t obj, bool system_family, uint32_t now_ms)
{
    char command[24], result[24], reason[24] = {0};
    bmb_json_span_t v;
    uint32_t sequence;
    if (!bmb_json_object_get(obj, "command", &v)) return false;
    size_t len = bmb_json_string_copy(v, command, sizeof(command));
    if (len == SIZE_MAX || len == 0 || len >= sizeof(command) ||
        strcmp(command, "push_status") == 0) return false;
    if (!bmb_json_object_get(obj, "sequence_id", &v) ||
        !bmb_json_uint32(v, true, &sequence) ||
        !bmb_json_object_get(obj, "result", &v)) return false;
    len = bmb_json_string_copy(v, result, sizeof(result));
    if (len == SIZE_MAX || len == 0 || len >= sizeof(result)) return false;
    (void)string_field(obj, "reason", reason, sizeof(reason));
    /* Matching, timeout, device/session and multi-part aggregation belong to
     * the command owner; no other parser path changes busy/status. */
    (void)bmb_command_ack_locked(state, topic, topic_len, system_family, command,
                                 sequence, result, reason, now_ms);
    return true;
}

bool bmb_report_apply(bmb_state_t *state, const char *topic, size_t topic_len,
                      const char *json, size_t json_len, uint32_t now_ms)
{
    bmb_json_span_t root;
    if (!state || !bmb_json_validate(json, json_len, &root) || !object(root)) return false;
    bmb_json_span_t print, system;
    bool have_print = bmb_json_object_get(root, "print", &print) && object(print);
    bool have_system = bmb_json_object_get(root, "system", &system) && object(system);
    if (!have_print && !have_system) return false;
    bmb_state_lock();
    if (state->link_resetting) {
        bmb_state_unlock();
        return false;
    }
    /* A reply in either family quarantines a bare print_error in the print
     * object. Determine this before applying telemetry so a system-family ACK
     * (chamber light) cannot raise a false printer error. */
    bool report_has_reply = (have_print && command_reply(print)) ||
                            (have_system && command_reply(system));
    bool telemetry = have_print && telemetry_apply(state, print, now_ms, report_has_reply);
    if (telemetry) copy_dev_id(state, topic, topic_len);
    bool ack = have_print && ack_apply(state, topic, topic_len, print, false, now_ms);
    if (have_system) ack |= ack_apply(state, topic, topic_len, system, true, now_ms);
    bmb_state_unlock();
    return telemetry || ack;
}
