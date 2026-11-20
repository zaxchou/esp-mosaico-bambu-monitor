// SPDX-License-Identifier: Apache-2.0
#include "bmb_command.h"
#include "bmb_state.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *const COMMAND_NAMES[] = {
    "pause", "resume", "stop", "print_speed", "ledctrl",
};

static bool stop_context_ready(const bmb_state_t *model, uint32_t now_ms)
{
    if (!model || model->ctrl_busy || model->link_resetting ||
        model->link != BMB_LINK_ONLINE || !model->state_received ||
        !model->dev_id[0] ||
        (uint32_t)(now_ms - model->last_report_ms) >= BMB_TELEMETRY_FRESH_MS) {
        return false;
    }
    return model->print_state == BMB_PRINT_PREPARE ||
           model->print_state == BMB_PRINT_SLICING ||
           model->print_state == BMB_PRINT_RUNNING ||
           model->print_state == BMB_PRINT_PAUSE;
}

void bmb_command_stop_disarm_locked(bmb_state_t *model)
{
    if (model) model->stop_confirmation.armed = false;
}

bool bmb_command_stop_arm_locked(bmb_state_t *model, uint32_t now_ms)
{
    bmb_command_stop_disarm_locked(model);
    if (!stop_context_ready(model, now_ms)) return false;
    bmb_stop_confirmation_t *confirmation = &model->stop_confirmation;
    confirmation->started_ms = now_ms;
    confirmation->session_epoch = model->session_epoch;
    snprintf(confirmation->device_id, sizeof(confirmation->device_id), "%s", model->dev_id);
    snprintf(confirmation->task, sizeof(confirmation->task), "%s", model->task);
    confirmation->task_hash = model->task_hash;
    confirmation->print_state = model->print_state;
    confirmation->armed = true;
    return true;
}

bool bmb_command_stop_valid_locked(bmb_state_t *model, uint32_t now_ms)
{
    if (!model || !model->stop_confirmation.armed) return false;
    const bmb_stop_confirmation_t *confirmation = &model->stop_confirmation;
    if (!stop_context_ready(model, now_ms) ||
        (uint32_t)(now_ms - confirmation->started_ms) >= BMB_STOP_CONFIRM_MS ||
        confirmation->session_epoch != model->session_epoch ||
        confirmation->print_state != (int)model->print_state ||
        confirmation->task_hash != model->task_hash ||
        strcmp(confirmation->device_id, model->dev_id) != 0 ||
        strcmp(confirmation->task, model->task) != 0) {
        bmb_command_stop_disarm_locked(model);
        return false;
    }
    return true;
}

void bmb_command_cancel_locked(bmb_state_t *model, const char *reason)
{
    if (!model) return;
    bmb_command_stop_disarm_locked(model);
    const bool pending = model->ctrl_busy;
    model->ctrl_busy = false;
    ++model->command.token;
    if (pending && reason) {
        snprintf(model->ctrl_status, sizeof(model->ctrl_status), "%s", reason);
    }
}

void bmb_command_tick_locked(bmb_state_t *model, uint32_t now_ms)
{
    if (!model) return;
    (void)bmb_command_stop_valid_locked(model, now_ms);
    if (!model->ctrl_busy) return;
    if (model->link_resetting) {
        bmb_command_cancel_locked(model, "已取消");
    } else if (model->link != BMB_LINK_ONLINE) {
        bmb_command_cancel_locked(model, "连接已断开");
    } else if (strcmp(model->dev_id, model->command.device_id) != 0 ||
               model->task_hash != model->command.task_hash ||
               strcmp(model->task, model->command.task) != 0) {
        bmb_command_cancel_locked(model, "已取消");
    } else if ((uint32_t)(now_ms - model->command.started_ms) >= BMB_COMMAND_TIMEOUT_MS) {
        bmb_command_cancel_locked(model, "无响应，请检查打印机");
    }
}

bool bmb_command_begin_locked(bmb_state_t *model, bmb_command_kind_t kind,
                              const uint32_t *sequences, unsigned parts,
                              uint32_t now_ms)
{
    if (!model || !sequences || kind < BMB_COMMAND_PAUSE ||
        kind > BMB_COMMAND_LIGHT || parts == 0 || parts > BMB_COMMAND_PARTS ||
        (kind != BMB_COMMAND_LIGHT && parts != 1)) return false;
    bmb_command_tick_locked(model, now_ms);
    if (model->ctrl_busy || model->link_resetting || model->link != BMB_LINK_ONLINE ||
        !model->state_received || !model->dev_id[0] ||
        (uint32_t)(now_ms - model->last_report_ms) >= BMB_TELEMETRY_FRESH_MS) {
        return false;
    }
    for (unsigned i = 0; i < parts; ++i) {
        if (!sequences[i] || (i && sequences[i] == sequences[0])) return false;
    }
    /* Recheck the first tap's snapshot in the same critical section that
     * creates STOP. A UI guard checked before acquiring this lock cannot bind
     * the command to the original task. Invalid sequences do not consume it. */
    if (kind == BMB_COMMAND_STOP && !bmb_command_stop_valid_locked(model, now_ms)) {
        return false;
    }
    bmb_command_stop_disarm_locked(model);
    uint32_t token = model->command.token + 1U;
    model->command = (bmb_command_state_t){
        .kind = kind, .token = token, .started_ms = now_ms, .parts = parts,
        .queued_print_state = model->print_state,
    };
    for (unsigned i = 0; i < parts; ++i) model->command.sequence[i] = sequences[i];
    snprintf(model->command.device_id, sizeof(model->command.device_id), "%s",
             model->dev_id);
    snprintf(model->command.task, sizeof(model->command.task), "%s", model->task);
    model->command.task_hash = model->task_hash;
    /* Establish pending state before making any payload visible to MQTT. */
    model->ctrl_busy = true;
    snprintf(model->ctrl_status, sizeof(model->ctrl_status), "发送中");
    return true;
}

bool bmb_command_can_send_locked(bmb_state_t *model, uint32_t token,
                                 unsigned part, uint32_t now_ms)
{
    if (!model) return false;
    bmb_command_tick_locked(model, now_ms);
    if (!model->ctrl_busy || model->command.token != token ||
        part >= model->command.parts ||
        (model->command.sent_mask & (1U << part))) return false;
    if (model->link != BMB_LINK_ONLINE ||
        strcmp(model->dev_id, model->command.device_id) != 0 ||
        model->task_hash != model->command.task_hash ||
        strcmp(model->task, model->command.task) != 0 ||
        (uint32_t)(now_ms - model->last_report_ms) >= BMB_TELEMETRY_FRESH_MS ||
        (model->command.kind <= BMB_COMMAND_STOP &&
         model->command.queued_print_state != (int)model->print_state)) {
        bmb_command_cancel_locked(model, "已取消");
        return false;
    }
    /* Mark the attempt before I/O: a fast ACK cannot be overwritten later. */
    model->command.sent_mask |= 1U << part;
    return true;
}

void bmb_command_sent_locked(bmb_state_t *model, uint32_t token,
                             unsigned part, bool success)
{
    if (!model || !model->ctrl_busy || model->command.token != token ||
        part >= model->command.parts) return;
    if (!success) bmb_command_cancel_locked(model, "发送失败");
}

/* Reasons arrive as validated UTF-8 from the report parser. Truncate at a
 * codepoint boundary so diagnostics never end with a partial multibyte glyph. */
static void copy_reason(char *out, size_t cap, const char *reason)
{
    size_t length = strlen(reason);
    if (length >= cap) {
        length = cap - 1U;
        while (length && ((unsigned char)reason[length] & 0xC0U) == 0x80U) --length;
    }
    memcpy(out, reason, length);
    out[length] = '\0';
}

static bool printable_ascii(const char *text)
{
    for (; *text; ++text) {
        if ((unsigned char)*text < 0x20U || (unsigned char)*text > 0x7EU) return false;
    }
    return true;
}

static bool topic_matches(const char *topic, size_t len, const char *device)
{
    const size_t n = strlen(device);
    return topic && len == 7U + n + 7U &&
           memcmp(topic, "device/", 7) == 0 &&
           memcmp(topic + 7, device, n) == 0 &&
           memcmp(topic + 7 + n, "/report", 7) == 0;
}

bool bmb_command_ack_locked(bmb_state_t *model, const char *topic,
                            size_t topic_len, bool system_family,
                            const char *command, uint32_t sequence,
                            const char *result, const char *reason,
                            uint32_t now_ms)
{
    if (!model || !command || !result || !*result) return false;
    bmb_command_tick_locked(model, now_ms);
    if (!model->ctrl_busy ||
        !topic_matches(topic, topic_len, model->command.device_id) ||
        system_family != (model->command.kind == BMB_COMMAND_LIGHT) ||
        strcmp(command, COMMAND_NAMES[model->command.kind]) != 0) return false;
    unsigned part;
    for (part = 0; part < model->command.parts; ++part) {
        if (model->command.sequence[part] == sequence) break;
    }
    if (part == model->command.parts ||
        !(model->command.sent_mask & (1U << part)) ||
        (model->command.ack_mask & (1U << part))) return false;
    const bool success = strcasecmp(result, "SUCCESS") == 0 ||
                         strcasecmp(result, "OK") == 0;
    const bool failure = strcasecmp(result, "FAIL") == 0 ||
                         strcasecmp(result, "FAILED") == 0 ||
                         strcasecmp(result, "ERROR") == 0;
    if (!success && !failure) return false;
    model->command.ack_mask |= 1U << part;
    if (failure) {
        model->command.failed = true;
        copy_reason(model->command.reason, sizeof(model->command.reason),
                    reason && *reason ? reason : "未知原因");
    }
    if (model->command.ack_mask == (1U << model->command.parts) - 1U) {
        model->ctrl_busy = false;
        if (model->command.failed) {
            /* The control font contains ASCII and fixed UI phrases, not an
             * arbitrary printer-supplied CJK vocabulary. Preserve the reason
             * above for diagnosis and render only supported glyphs here. */
            if (printable_ascii(model->command.reason)) {
                snprintf(model->ctrl_status, sizeof(model->ctrl_status),
                         "被拒绝:%.20s", model->command.reason);
            } else {
                snprintf(model->ctrl_status, sizeof(model->ctrl_status),
                         "被拒绝，请检查打印机");
            }
        } else {
            snprintf(model->ctrl_status, sizeof(model->ctrl_status), "打印机已确认");
        }
    }
    return true;
}
