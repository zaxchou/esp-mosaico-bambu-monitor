// SPDX-License-Identifier: Apache-2.0

#include "bmb_ui.h"

#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

static void bmb_trace(const char *msg)
{
#ifndef ESP_PLATFORM
    FILE *fp = fopen("bmb_evt.txt", "a");
    if (fp) {
        fputs(msg, fp);
        fputc(10, fp);
        fclose(fp);
    }
#else
    (void)msg;
#endif
}

#include "bundle_gsp.h"

/* Host-only diagnostics: log the decoded callback action of every event. */
static void bmb_trace_event(const esp_gsp_event_t *event)
{
#ifdef ESP_PLATFORM
    (void)event;
#else
    gsp_bmb_call_event_t call;
    FILE *fp;
    if (!gsp_bmb_event_decode_call(event, &call)) {
        return;
    }
    fp = fopen("bmb_evt.txt", "a");
    if (fp) {
        fprintf(fp, "action=%d", (int)call.action);
        fputc(10, fp);
        fclose(fp);
    }
#endif
}
#include "bmb_buzz.h"
#include "bmb_config.h"
#include "bmb_ctrl.h"
#include "bmb_link.h"
#include "bmb_state.h"
#ifndef ESP_PLATFORM
#include "bmb_host_link.h"
#include "bmb_observe.h"
#endif

#define TICK_MS 100
#define STALE_REPORT_MS BMB_TELEMETRY_FRESH_MS
#define ETA_FRESH_MS 120000
#define STOP_ARM_TICKS 50 /* double-press confirm window for 停止 */

/* Pages are full-screen containers; one visible bind per page. */
static esp_gsp_err_t (*const PAGES[])(esp_gsp_handle_t, bool) = {
    gsp_bmb_status_page_set_visible,
    gsp_bmb_set_page_set_visible,
    gsp_bmb_pad_page_set_visible,
    gsp_bmb_finish_page_set_visible,
    gsp_bmb_ctrl_page_set_visible,
    gsp_bmb_ams_page_set_visible,
};
#define PAGE_STATUS 0
#define PAGE_SET 1
#define PAGE_PAD 2
#define PAGE_FINISH 3
#define PAGE_CTRL 4
#define PAGE_AMS 5

typedef enum { EDIT_NONE = 0, EDIT_IP = 1, EDIT_CODE = 2 } edit_t;

static bmb_ui_t *s_ui;
static bmb_state_t *s_model;
static bmb_pairing_t s_cfg;
static edit_t s_edit;
static bool s_stop_armed;
static int s_stop_arm_tick;

static esp_gsp_err_t s_last_error;
static const char *s_stage = "init";
static bool s_error_logged;
static unsigned s_error_count;
static unsigned s_reported_errors;
static int s_page;
static bool s_page_dirty;

/* UI/render-task owned desired values for one-shot text (dialogs, keypad and
 * settings). Periodic status text already retries in refresh(). */
typedef esp_gsp_err_t (*text_setter_t)(esp_gsp_handle_t, const char *);
static struct {
    text_setter_t set;
    char text[80];
    bool pending;
} s_one_shot[] = {
    { .set = gsp_bmb_ctrl_stop_btn_lbl_set_text },
    { .set = gsp_bmb_finish_task_set_text },
    { .set = gsp_bmb_finish_title_set_text },
    { .set = gsp_bmb_finish_sub_set_text },
    { .set = gsp_bmb_pad_field_set_text },
    { .set = gsp_bmb_pad_error_set_text },
    { .set = gsp_bmb_pad_title_set_text },
    { .set = gsp_bmb_set_ip_lbl_set_text },
    { .set = gsp_bmb_set_code_lbl_set_text },
    { .set = gsp_bmb_set_status_lbl_set_text },
};

/* Setters run on the render task. Record failures separately from critical
 * callback/timer setup; a failed setter does not establish its cause. */
static void remember(esp_gsp_handle_t ui, esp_gsp_err_t err)
{
    (void)ui;
    if (err == ESP_GSP_OK) {
        return;
    }
    s_last_error = err;
    ++s_error_count;
#ifdef ESP_PLATFORM
    if (!s_error_logged) {
        s_error_logged = true;
        ESP_LOGE("bmb_ui", "first gsp error 0x%x during %s", (unsigned)err, s_stage);
    }
#else
    (void)s_stage;
#endif
}

static void set_text(esp_gsp_handle_t ui,
                     esp_gsp_err_t (*set)(esp_gsp_handle_t, const char *),
                     const char *text);

static void apply_page(esp_gsp_handle_t ui)
{
    s_page_dirty = false;
    for (int i = 0; i < (int)(sizeof(PAGES) / sizeof(PAGES[0])); ++i) {
        const esp_gsp_err_t err = PAGES[i](ui, i == s_page);
        remember(ui, err);
        if (err != ESP_GSP_OK) s_page_dirty = true;
    }
}

static void show_page(esp_gsp_handle_t ui, int page)
{
    s_page = page;
#ifndef ESP_PLATFORM
    bmb_observe_page(page);
#endif
    bmb_state_lock();
    bmb_command_stop_disarm_locked(s_model);
    bmb_state_unlock();
    if (s_stop_armed) {
        s_stop_armed = false;
        set_text(ui, gsp_bmb_ctrl_stop_btn_lbl_set_text, "停止打印");
    }
    apply_page(ui);
}

/* ---------------------------------------------------------------- refresh -- */

/* localtime_r is POSIX; the host build (MinGW) only has localtime_s. */
static struct tm *local_time(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    return localtime_s(out, t) == 0 ? out : NULL;
#else
    return localtime_r(t, out);
#endif
}

/* Map the pushed model_id to a friendly name; unknown printers fall back to
 * the serial from the report topic. */
static const char *model_name(const bmb_state_t *model)
{
    if (strcmp(model->model_id, "CNf0baae0571025") == 0) {
        return "H2D";
    }
    return model->dev_id[0] != '\0' ? model->dev_id : "";
}

/* H2D empty slots report remain 0 with an all-white color instead of -1. */
static bool tray_empty(int remain, const char *color)
{
    return remain <= 0 &&
           (color[0] == '\0' || strcmp(color, "FFFFFFFF") == 0);
}

/* "112233FF" -> 0x112233; returns false when absent or malformed. */
static bool tray_rgb(const char *hex, uint32_t *out)
{
    uint32_t parsed = 0;
    int digits = 0;
    for (; digits < 6 && hex[digits] != '\0'; ++digits) {
        char c = hex[digits];
        if (c < '0') return false;
        int value = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        if (value > 15) return false;
        parsed = (parsed << 4) | (uint32_t)value;
    }
    if (digits != 6) return false;
    *out = parsed;
    return true;
}

static void set_text(esp_gsp_handle_t ui,
                     esp_gsp_err_t (*set)(esp_gsp_handle_t, const char *),
                     const char *text)
{
    const esp_gsp_err_t err = set(ui, text);
    remember(ui, err);
    for (size_t i = 0; i < sizeof(s_one_shot) / sizeof(s_one_shot[0]); ++i) {
        if (s_one_shot[i].set != set) continue;
        snprintf(s_one_shot[i].text, sizeof(s_one_shot[i].text), "%s", text);
        s_one_shot[i].pending = err != ESP_GSP_OK;
        break;
    }
#ifndef ESP_PLATFORM
#define OBSERVE_TEXT(name) \
    if (set == gsp_bmb_##name##_set_text) bmb_observe_text(#name, text)
    OBSERVE_TEXT(link_lbl);
    OBSERVE_TEXT(state_lbl);
    OBSERVE_TEXT(task2_lbl);
    OBSERVE_TEXT(layer_lbl);
    OBSERVE_TEXT(prog_hint);
    OBSERVE_TEXT(ctrl_status_lbl);
    OBSERVE_TEXT(ctrl_stop_btn_lbl);
    OBSERVE_TEXT(qk_pause_btn_lbl);
    OBSERVE_TEXT(qk_light_btn_lbl);
    OBSERVE_TEXT(finish_title);
    OBSERVE_TEXT(finish_sub);
    OBSERVE_TEXT(finish_task);
    OBSERVE_TEXT(ams_humidity_lbl);
    OBSERVE_TEXT(ams_temp_lbl);
    OBSERVE_TEXT(ams_remain_lbl_0);
    OBSERVE_TEXT(ams_remain_lbl_4);
    OBSERVE_TEXT(ams_type_lbl_0);
    OBSERVE_TEXT(ams_type_lbl_4);
    OBSERVE_TEXT(pad_error);
    OBSERVE_TEXT(pad_field);
    OBSERVE_TEXT(set_status_lbl);
    OBSERVE_TEXT(set_code_lbl);
#undef OBSERVE_TEXT
#endif
}

static void refresh(esp_gsp_handle_t ui)
{
    char buf[80];
    /* One coherent snapshot avoids holding the lock during GSP calls. This
     * aggregate fits the configured 12 KiB render-task stack budget. */
    bmb_state_lock();
    const bmb_state_t model = *s_model;
    bmb_state_unlock();

    snprintf(buf, sizeof(buf), "%s %s", bmb_link_text(model.link),
             model.link == BMB_LINK_ONLINE ? model_name(&model) : "");
    set_text(ui, gsp_bmb_link_lbl_set_text, buf);
    snprintf(buf, sizeof(buf), "状态  %s",
             bmb_print_state_text(model.print_state));
    set_text(ui, gsp_bmb_state_lbl_set_text, buf);
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)model.percent);
    set_text(ui, gsp_bmb_pct_lbl_set_text, buf);
    remember(ui, gsp_bmb_prog_set_value(ui, model.percent));

    if (model.has_task) {
        snprintf(buf, sizeof(buf), "任务  %s", model.task);
    } else {
        snprintf(buf, sizeof(buf), "暂无任务");
    }
    set_text(ui, gsp_bmb_task2_lbl_set_text, buf);

    if (model.layers > 0) {
        snprintf(buf, sizeof(buf), "层  %u / %u", (unsigned)model.layer,
                 (unsigned)model.layers);
    } else {
        snprintf(buf, sizeof(buf), "层  - / -");
    }
    const uint32_t remain_age = bmb_now_ms() - model.remaining_updated_ms;
    const bool eta_valid = model.print_state == BMB_PRINT_RUNNING &&
                           model.link == BMB_LINK_ONLINE &&
                           model.remain_min > 0 && remain_age < ETA_FRESH_MS &&
                           (uint32_t)(bmb_now_ms() - model.last_report_ms) <
                               STALE_REPORT_MS;
    if (eta_valid) {
        const int64_t estimate = (int64_t)model.remain_min * 60 - remain_age / 1000;
        const int64_t seconds = estimate > 0 ? estimate : 0;
        const unsigned minutes = (unsigned)((seconds + 59) / 60);
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, "  剩余 %uh%02um",
                 minutes / 60, minutes % 60);
        time_t now = time(NULL);
#ifndef ESP_PLATFORM
        now = bmb_observe_wall_time(now);
#endif
        if (now > 1600000000) {
            time_t eta = now + (time_t)seconds;
            struct tm tm_eta;
            if (local_time(&eta, &tm_eta)) {
                len = strlen(buf);
                snprintf(buf + len, sizeof(buf) - len, "  预计 %02d:%02d",
                         tm_eta.tm_hour, tm_eta.tm_min);
            }
        }
    }
    set_text(ui, gsp_bmb_layer_lbl_set_text, buf);

    if (model.nozzle > 0 || model.nozzle_target > 0) {
        snprintf(buf, sizeof(buf), "喷嘴 %d°/%d°", (int)model.nozzle,
                 (int)model.nozzle_target);
    } else {
        snprintf(buf, sizeof(buf), "喷嘴 --");
    }
    set_text(ui, gsp_bmb_nozzle_lbl_set_text, buf);
    if (model.bed > 0 || model.bed_target > 0) {
        snprintf(buf, sizeof(buf), "热床 %d°/%d°", (int)model.bed,
                 (int)model.bed_target);
    } else {
        snprintf(buf, sizeof(buf), "热床 --");
    }
    set_text(ui, gsp_bmb_bed_lbl_set_text, buf);
    if (model.chamber > 0) {
        snprintf(buf, sizeof(buf), "腔体 %d°", (int)model.chamber);
    } else {
        snprintf(buf, sizeof(buf), "腔体 --");
    }
    set_text(ui, gsp_bmb_chamber_lbl_set_text, buf);

    /* Control status line (also shown while busy). */
    set_text(ui, gsp_bmb_ctrl_status_lbl_set_text,
             model.ctrl_status[0] != '\0' ? model.ctrl_status : "就绪");
    set_text(ui, gsp_bmb_prog_hint_set_text,
             model.ctrl_status[0] != '\0' ? model.ctrl_status : "打印进度");
    set_text(ui, gsp_bmb_ctrl_light_btn_lbl_set_text,
             model.light_on == 0 ? "灯 关" : "灯 开");
    /* Contextual bottom bar: direct controls while a print is live. */
    const bool printing = model.print_state == BMB_PRINT_RUNNING ||
                          model.print_state == BMB_PRINT_PAUSE;
    remember(ui, gsp_bmb_idle_bar_set_visible(ui, !printing));
    remember(ui, gsp_bmb_print_bar_set_visible(ui, printing));
    set_text(ui, gsp_bmb_qk_pause_btn_lbl_set_text,
             model.print_state == BMB_PRINT_PAUSE ? "继续" : "暂停");
    set_text(ui, gsp_bmb_qk_light_btn_lbl_set_text,
             model.light_on == 0 ? "灯 关" : "灯 开");

    /* AMS page values; -- keeps the last snapshot when the printer omits
     * the ams object. */
    if (model.ams_humidity >= 0) {
        snprintf(buf, sizeof(buf), "湿度 %d", (int)model.ams_humidity);
    } else {
        snprintf(buf, sizeof(buf), "湿度 --");
    }
    set_text(ui, gsp_bmb_ams_humidity_lbl_set_text, buf);
    if (model.ams_temp != INT16_MIN) {
        snprintf(buf, sizeof(buf), "温度 %d°", (int)model.ams_temp);
    } else {
        snprintf(buf, sizeof(buf), "温度 --");
    }
    set_text(ui, gsp_bmb_ams_temp_lbl_set_text, buf);
    for (int slot = 0; slot < BMB_AMS_TRAYS; ++slot) {
        int remain = model.tray_remain[slot];
        const char *color = model.tray_color[slot];
        const char *type = model.tray_type[slot];
        if (tray_empty(remain, color) || (remain <= 0 && type[0] == '\0')) {
            snprintf(buf, sizeof(buf), "空");
        } else {
            snprintf(buf, sizeof(buf), "%d%%", remain);
        }
        set_text(ui, slot == 0 ? gsp_bmb_ams_remain_lbl_0_set_text :
                 slot == 1 ? gsp_bmb_ams_remain_lbl_1_set_text :
                 slot == 2 ? gsp_bmb_ams_remain_lbl_2_set_text :
                 slot == 3 ? gsp_bmb_ams_remain_lbl_3_set_text :
                 slot == 4 ? gsp_bmb_ams_remain_lbl_4_set_text :
                 slot == 5 ? gsp_bmb_ams_remain_lbl_5_set_text :
                 slot == 6 ? gsp_bmb_ams_remain_lbl_6_set_text :
                             gsp_bmb_ams_remain_lbl_7_set_text, buf);
        set_text(ui, slot == 0 ? gsp_bmb_ams_type_lbl_0_set_text :
                 slot == 1 ? gsp_bmb_ams_type_lbl_1_set_text :
                 slot == 2 ? gsp_bmb_ams_type_lbl_2_set_text :
                 slot == 3 ? gsp_bmb_ams_type_lbl_3_set_text :
                 slot == 4 ? gsp_bmb_ams_type_lbl_4_set_text :
                 slot == 5 ? gsp_bmb_ams_type_lbl_5_set_text :
                 slot == 6 ? gsp_bmb_ams_type_lbl_6_set_text :
                             gsp_bmb_ams_type_lbl_7_set_text, type);
        /* Filament color swatch: neutral gray when unknown or empty. */
        uint32_t rgb = 0x2A3743;
        if (remain > 0 && !tray_rgb(color, &rgb)) {
            rgb = 0x2A3743;
        }
        remember(ui, slot == 0 ? gsp_bmb_ams_swatch_0_set_color_rgb888(ui, rgb) :
                 slot == 1 ? gsp_bmb_ams_swatch_1_set_color_rgb888(ui, rgb) :
                 slot == 2 ? gsp_bmb_ams_swatch_2_set_color_rgb888(ui, rgb) :
                 slot == 3 ? gsp_bmb_ams_swatch_3_set_color_rgb888(ui, rgb) :
                 slot == 4 ? gsp_bmb_ams_swatch_4_set_color_rgb888(ui, rgb) :
                 slot == 5 ? gsp_bmb_ams_swatch_5_set_color_rgb888(ui, rgb) :
                 slot == 6 ? gsp_bmb_ams_swatch_6_set_color_rgb888(ui, rgb) :
                             gsp_bmb_ams_swatch_7_set_color_rgb888(ui, rgb));
    }

        remember(ui, gsp_bmb_offline_bar_set_visible(ui,
                                                 model.link != BMB_LINK_ONLINE));
}

/* ---------------------------------------------------------------- keypad ---- */

static const char *pad_title(edit_t edit)
{
    return edit == EDIT_IP ? "输入打印机 IP" : "输入访问码";
}

static void pad_refresh(esp_gsp_handle_t ui)
{
    set_text(ui, gsp_bmb_pad_field_set_text, s_ui->pad_buf);
    set_text(ui, gsp_bmb_pad_error_set_text, "");
}

static void pad_open(esp_gsp_handle_t ui, edit_t edit)
{
    s_edit = edit;
    memset(s_ui->pad_buf, 0, sizeof(s_ui->pad_buf));
    s_ui->pad_len = 0;
    if (edit == EDIT_IP && s_cfg.ip[0] != '\0') {
        snprintf(s_ui->pad_buf, sizeof(s_ui->pad_buf), "%s", s_cfg.ip);
        s_ui->pad_len = strlen(s_ui->pad_buf);
    } else if (edit == EDIT_CODE && s_cfg.code[0] != '\0') {
        snprintf(s_ui->pad_buf, sizeof(s_ui->pad_buf), "%s", s_cfg.code);
        s_ui->pad_len = strlen(s_ui->pad_buf);
    }
    set_text(ui, gsp_bmb_pad_title_set_text, pad_title(edit));
    pad_refresh(ui);
    show_page(ui, PAGE_PAD);
}

static void pad_close(esp_gsp_handle_t ui)
{
    s_edit = EDIT_NONE;
    show_page(ui, PAGE_SET);
}

static void pad_apply_fields(esp_gsp_handle_t ui)
{
    char buf[72];
    snprintf(buf, sizeof(buf), "IP  %s", s_cfg.ip);
    set_text(ui, gsp_bmb_set_ip_lbl_set_text, buf);
    snprintf(buf, sizeof(buf), "访问码  %s", s_cfg.code);
    set_text(ui, gsp_bmb_set_code_lbl_set_text, buf);
}

static bool valid_ipv4(const char *text)
{
    int octets = 0;
    int value = -1;
    int digits = 0;
    for (const char *p = text;; ++p) {
        if (*p >= '0' && *p <= '9') {
            value = value < 0 ? 0 : value;
            value = value * 10 + (*p - '0');
            if (value > 255) return false;
            ++digits;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || value < 0) return false;
            ++octets;
            value = -1;
            digits = 0;
            if (*p == '\0') break;
        } else {
            return false;
        }
    }
    return octets == 4;
}

static void pad_ok(esp_gsp_handle_t ui)
{
    if (s_edit == EDIT_IP && !valid_ipv4(s_ui->pad_buf)) {
        set_text(ui, gsp_bmb_pad_error_set_text, "IP 无效");
        return;
    }
    if (s_edit == EDIT_CODE && s_ui->pad_len == 0) {
        set_text(ui, gsp_bmb_pad_error_set_text, "访问码不能为空");
        return;
    }
    if (s_edit == EDIT_IP) {
        snprintf(s_cfg.ip, sizeof(s_cfg.ip), "%s", s_ui->pad_buf);
    } else if (s_edit == EDIT_CODE) {
        snprintf(s_cfg.code, sizeof(s_cfg.code), "%s", s_ui->pad_buf);
    }
    s_cfg.valid = s_cfg.ip[0] != '\0' && s_cfg.code[0] != '\0';
    pad_close(ui);
    pad_apply_fields(ui);
}

/* ------------------------------------------------------------------ tick ---- */

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    bmb_ui_t *ui_state = ctx;
    s_stage = "tick";
    if (!ui_state->active || s_ui != ui_state || !s_model) {
        /* Ignore callbacks retained after a failed teardown operation. */
        return;
    }
    if (!ui_state->initialized) {
        const unsigned errors = s_error_count;
        s_stage = "initial page";
        show_page(ui, PAGE_STATUS);
        s_stage = "initial fields";
        pad_apply_fields(ui);
        s_stage = "initial refresh";
        refresh(ui);
        ui_state->initialized = s_error_count == errors;
        s_stage = "tick";
    }
    if (!ui_state->initialized) return;
    if (s_page_dirty) apply_page(ui);
    for (size_t i = 0; i < sizeof(s_one_shot) / sizeof(s_one_shot[0]); ++i) {
        if (!s_one_shot[i].pending) continue;
        const esp_gsp_err_t err = s_one_shot[i].set(ui, s_one_shot[i].text);
        remember(ui, err);
        s_one_shot[i].pending = err != ESP_GSP_OK;
    }
    ++ui_state->tick_count;
#ifndef ESP_PLATFORM
    bmb_host_link_tick();
#endif
    /* Recheck current telemetry under the lock: a concurrent fresh report must
     * win over an earlier stale snapshot. Only telemetry owns last_report_ms.
     * Native fixtures advance the same clock before executing this branch. */
    bmb_state_lock();
    const uint32_t now_ms = bmb_now_ms();
    if (s_model->link == BMB_LINK_ONLINE &&
        (uint32_t)(now_ms - s_model->last_report_ms) >= STALE_REPORT_MS) {
        s_model->link = BMB_LINK_CONNECTING;
    }
    bmb_command_tick_locked(s_model, now_ms);
    const bool stop_valid = bmb_command_stop_valid_locked(s_model, now_ms);
    if (s_stop_armed &&
        (!stop_valid || ui_state->tick_count - s_stop_arm_tick > STOP_ARM_TICKS)) {
        bmb_command_stop_disarm_locked(s_model);
    }
    bmb_state_unlock();
    if (s_stop_armed &&
        (!stop_valid || ui_state->tick_count - s_stop_arm_tick > STOP_ARM_TICKS)) {
        s_stop_armed = false;
        set_text(ui, gsp_bmb_ctrl_stop_btn_lbl_set_text, "停止打印");
    }
    /* Errors keep priority until acknowledged. Track the displayed generation
     * so an error received between a frame and a tap cannot be dismissed unseen. */
    bmb_state_lock();
    const uint32_t session_epoch = s_model->session_epoch;
    const bool session_ending = s_model->link_resetting;
    const bool error_pending = !session_ending && s_model->error_seen;
    const bool finish_pending = !session_ending && s_model->finish_seen;
    const uint32_t error_generation = s_model->error_generation;
    const uint32_t finish_generation = s_model->finish_generation;
    const uint32_t error_code = s_model->alert_error_code;
    char alert_task[sizeof(s_model->finish_task)];
    snprintf(alert_task, sizeof(alert_task), "%s",
             error_pending ? s_model->error_task : s_model->finish_task);
    bmb_state_unlock();
    if (session_ending || ui_state->alert_epoch != session_epoch) {
        if (ui_state->alert_kind) show_page(ui, PAGE_STATUS);
        ui_state->alert_kind = 0;
        ui_state->alert_generation = 0;
        ui_state->alert_epoch = session_epoch;
    }
    const unsigned next_alert = error_pending ? 2 : finish_pending ? 1 : 0;
    const uint32_t next_generation = error_pending ? error_generation :
                                                    finish_generation;
    if (next_alert &&
        (ui_state->alert_kind != next_alert ||
         ui_state->alert_generation != next_generation)) {
        ui_state->alert_kind = next_alert;
        ui_state->alert_generation = next_generation;
        set_text(ui, gsp_bmb_finish_task_set_text, alert_task);
        if (next_alert == 2) {
            char sub[48];
            if (error_code) {
                snprintf(sub, sizeof(sub), "错误码 0x%08" PRIX32, error_code);
            } else {
                snprintf(sub, sizeof(sub), "请检查打印机");
            }
            set_text(ui, gsp_bmb_finish_title_set_text, "打印出错");
            set_text(ui, gsp_bmb_finish_sub_set_text, sub);
            bmb_trace("show-error");
        } else {
            set_text(ui, gsp_bmb_finish_title_set_text, "打印完成");
            set_text(ui, gsp_bmb_finish_sub_set_text, "可以取件了");
            bmb_trace("show-finish");
        }
        bmb_buzz_once();
        show_page(ui, PAGE_FINISH);
    }
    s_stage = "refresh";
    refresh(ui);
    ui_state->last_error = s_last_error;
#ifdef ESP_PLATFORM
    /* Count failures without flooding the transport with per-setter logs. */
    if (ui_state->tick_count % 600U == 0 && s_error_count != s_reported_errors) {
        ESP_LOGW("bmb_ui", "gsp errors since report: %u (last 0x%x)",
                 s_error_count - s_reported_errors, (unsigned)s_last_error);
        s_reported_errors = s_error_count;
    }
#endif
#ifndef ESP_PLATFORM
    bmb_observe_snapshot(ui_state, s_model, s_stop_armed, s_last_error);
#endif
}

/* -------------------------------------------------------------- callbacks -- */

static bool pad_push(char c)
{
    if (s_ui->pad_len + 1 >= sizeof(s_ui->pad_buf)) {
        return false;
    }
    s_ui->pad_buf[s_ui->pad_len++] = c;
    s_ui->pad_buf[s_ui->pad_len] = '\0';
    return true;
}

/* Restart opens a new telemetry/alert generation namespace. Discard pending
 * old-session alerts immediately; the device lifecycle resets its model later
 * on the background task after stopping the old MQTT client. */
static bool restart_link(void)
{
    if (!bmb_link_restart(s_model)) return false;
    bmb_state_lock();
    s_model->finish_seen = false;
    s_model->error_seen = false;
    bmb_state_unlock();
    s_ui->alert_kind = 0;
    s_ui->alert_generation = 0;
    return true;
}

static void send_ctrl(bmb_cmd_t cmd)
{
    if (s_model && bmb_ctrl_send(s_model, cmd)) {
        bmb_trace("ctrl-sent");
    }
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    bmb_ui_t *ui_state = ctx;
    if (!ui_state->active || !ui_state->initialized ||
        s_ui != ui_state || !s_model) return;
    s_stage = "event";
    bmb_trace("evt");
    bmb_trace_event(event);

    if (gsp_bmb_event_is_ctrl_open(event)) {
        show_page(ui, PAGE_CTRL);
        return;
    }
    if (gsp_bmb_event_is_ctrl_back(event)) {
        show_page(ui, PAGE_STATUS);
        return;
    }
    if (gsp_bmb_event_is_ctrl_pause(event)) {
        send_ctrl(BMB_CMD_PAUSE);
        return;
    }
    if (gsp_bmb_event_is_ctrl_resume(event)) {
        send_ctrl(BMB_CMD_RESUME);
        return;
    }
    if (gsp_bmb_event_is_ctrl_spd1(event)) {
        (void)bmb_ctrl_set_speed(s_model, 1);
        return;
    }
    if (gsp_bmb_event_is_ctrl_spd2(event)) {
        (void)bmb_ctrl_set_speed(s_model, 2);
        return;
    }
    if (gsp_bmb_event_is_ctrl_spd3(event)) {
        (void)bmb_ctrl_set_speed(s_model, 3);
        return;
    }
    if (gsp_bmb_event_is_ctrl_light(event)) {
        bmb_state_lock();
        const bool light_on = s_model->light_on == 1;
        bmb_state_unlock();
        (void)bmb_ctrl_set_light(s_model, !light_on);
        return;
    }
    if (gsp_bmb_event_is_ctrl_stop(event)) {
        bmb_state_lock();
        const uint32_t now = bmb_now_ms();
        const bool confirmed = s_stop_armed &&
            ui_state->tick_count - s_stop_arm_tick <= STOP_ARM_TICKS &&
            bmb_command_stop_valid_locked(s_model, now);
        if (!confirmed) {
            s_stop_armed = bmb_command_stop_arm_locked(s_model, now);
            s_stop_arm_tick = ui_state->tick_count;
        }
        bmb_state_unlock();
        if (!confirmed) {
            set_text(ui, gsp_bmb_ctrl_stop_btn_lbl_set_text,
                     s_stop_armed ? "再按一次确认停止" : "停止打印");
            return;
        }
        s_stop_armed = false;
        set_text(ui, gsp_bmb_ctrl_stop_btn_lbl_set_text, "停止打印");
        /* Command admission rechecks and consumes the same confirmation under
         * the model lock, including changes after the UI check above. */
        send_ctrl(BMB_CMD_STOP);
        return;
    }
    if (gsp_bmb_event_is_ams_open(event)) {
        show_page(ui, PAGE_AMS);
        return;
    }
    if (gsp_bmb_event_is_ams_back(event)) {
        show_page(ui, PAGE_STATUS);
        return;
    }
    if (gsp_bmb_event_is_link_rst(event)) {
        if (!restart_link()) {
            bmb_state_lock();
            snprintf(s_model->ctrl_status, sizeof(s_model->ctrl_status),
                     "连接失败");
            bmb_state_unlock();
        } else {
            show_page(ui, PAGE_STATUS);
        }
        return;
    }
    if (gsp_bmb_event_is_set_open(event)) {
        pad_apply_fields(ui);
        set_text(ui, gsp_bmb_set_status_lbl_set_text, "未保存");
        show_page(ui, PAGE_SET);
        return;
    }
    if (gsp_bmb_event_is_set_back(event)) {
        show_page(ui, PAGE_STATUS);
        return;
    }
    if (gsp_bmb_event_is_set_ip(event)) {
        pad_open(ui, EDIT_IP);
        return;
    }
    if (gsp_bmb_event_is_set_code(event)) {
        pad_open(ui, EDIT_CODE);
        return;
    }
    if (gsp_bmb_event_is_set_save(event)) {
        if (bmb_pairing_save(&s_cfg)) {
            set_text(ui, gsp_bmb_set_status_lbl_set_text,
                     restart_link() ? "已保存，正在连接…" : "连接失败");
        } else {
            set_text(ui, gsp_bmb_set_status_lbl_set_text, "保存失败");
        }
        show_page(ui, PAGE_SET);
        return;
    }
    if (gsp_bmb_event_is_finish_ok(event)) {
        bmb_state_lock();
        if (!s_model->link_resetting &&
            ui_state->alert_epoch == s_model->session_epoch &&
            ui_state->alert_kind == 2 &&
            ui_state->alert_generation == s_model->error_generation) {
            s_model->error_seen = false;
        } else if (!s_model->link_resetting &&
                   ui_state->alert_epoch == s_model->session_epoch &&
                   ui_state->alert_kind == 1 &&
                   ui_state->alert_generation == s_model->finish_generation) {
            s_model->finish_seen = false;
        }
        bmb_state_unlock();
        ui_state->alert_kind = 0;
        show_page(ui, PAGE_STATUS);
        return;
    }
    if (gsp_bmb_event_is_qk_pause(event)) {
        bmb_state_lock();
        const bmb_print_state_t st = s_model->print_state;
        bmb_state_unlock();
        send_ctrl(st == BMB_PRINT_PAUSE ? BMB_CMD_RESUME
                                                      : BMB_CMD_PAUSE);
        return;
    }
    if (gsp_bmb_event_is_qk_light(event)) {
        bmb_state_lock();
        const bool light_on = s_model->light_on == 1;
        bmb_state_unlock();
        (void)bmb_ctrl_set_light(s_model, !light_on);
        return;
    }
    if (gsp_bmb_event_is_pad_dot(event)) {
        if (pad_push('.')) pad_refresh(ui);
        return;
    }
    if (gsp_bmb_event_is_pad_del(event)) {
        if (s_ui->pad_len > 0) {
            s_ui->pad_buf[--s_ui->pad_len] = '\0';
        }
        pad_refresh(ui);
        return;
    }
    if (gsp_bmb_event_is_pad_clr(event)) {
        s_ui->pad_len = 0;
        s_ui->pad_buf[0] = '\0';
        pad_refresh(ui);
        return;
    }
    if (gsp_bmb_event_is_pad_ok(event)) {
        pad_ok(ui);
        return;
    }
    if (gsp_bmb_event_is_pad_cancel(event)) {
        pad_close(ui);
        return;
    }
    if (gsp_bmb_event_is_pad_a(event)) { if (pad_push('a')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_b(event)) { if (pad_push('b')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_c(event)) { if (pad_push('c')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_d(event)) { if (pad_push('d')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_e(event)) { if (pad_push('e')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_f(event)) { if (pad_push('f')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_g(event)) { if (pad_push('g')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_h(event)) { if (pad_push('h')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_i(event)) { if (pad_push('i')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_j(event)) { if (pad_push('j')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_k(event)) { if (pad_push('k')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_l(event)) { if (pad_push('l')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_m(event)) { if (pad_push('m')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_n(event)) { if (pad_push('n')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_o(event)) { if (pad_push('o')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_p(event)) { if (pad_push('p')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_q(event)) { if (pad_push('q')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_r(event)) { if (pad_push('r')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_s(event)) { if (pad_push('s')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_t(event)) { if (pad_push('t')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_u(event)) { if (pad_push('u')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_v(event)) { if (pad_push('v')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_w(event)) { if (pad_push('w')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_x(event)) { if (pad_push('x')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_y(event)) { if (pad_push('y')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_z(event)) { if (pad_push('z')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_1(event)) { if (pad_push('1')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_2(event)) { if (pad_push('2')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_3(event)) { if (pad_push('3')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_4(event)) { if (pad_push('4')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_5(event)) { if (pad_push('5')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_6(event)) { if (pad_push('6')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_7(event)) { if (pad_push('7')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_8(event)) { if (pad_push('8')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_9(event)) { if (pad_push('9')) pad_refresh(ui); return; }
    if (gsp_bmb_event_is_pad_0(event)) { if (pad_push('0')) pad_refresh(ui); return; }
}

/* ------------------------------------------------------------------- init -- */

esp_gsp_err_t bmb_ui_init(esp_gsp_handle_t ui, bmb_ui_t *state, bmb_state_t *model)
{
    if (!ui || !state || !model) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *state = (bmb_ui_t){ .active = true };
    s_ui = state;
    s_model = model;
    s_edit = EDIT_NONE;
    s_stop_armed = false;
    s_stop_arm_tick = 0;
    s_error_logged = false;
    s_error_count = 0;
    s_reported_errors = 0;
    s_last_error = ESP_GSP_OK;
    s_page = PAGE_STATUS;
    s_page_dirty = false;
    for (size_t i = 0; i < sizeof(s_one_shot) / sizeof(s_one_shot[0]); ++i) {
        s_one_shot[i].pending = false;
    }
    bmb_pairing_load(&s_cfg);

    /* Apply the first frame in the timer's render-task context. Initialization
     * returns only callback/timer setup errors, never an aggregated setter error.
     * No UI events are accepted until the complete initial frame succeeds. */
    s_stage = "on_event";
    esp_gsp_err_t err = esp_gsp_on_event(ui, on_event, state);
    if (err != ESP_GSP_OK) {
        remember(ui, err);
        state->last_error = err;
        s_ui = NULL;
        s_model = NULL;
        return err;
    }
    s_stage = "timer";
    state->timer = esp_gsp_timer_create(ui, TICK_MS, tick, state);
    if (!state->timer) {
        remember(ui, ESP_GSP_ERR_NO_MEM);
        state->last_error = ESP_GSP_ERR_NO_MEM;
        const esp_gsp_err_t cleanup = esp_gsp_on_event(ui, NULL, NULL);
        remember(ui, cleanup);
        s_ui = NULL;
        s_model = NULL;
        return state->last_error;
    }
    return ESP_GSP_OK;
}

void bmb_ui_deinit(esp_gsp_handle_t ui, bmb_ui_t *state)
{
    if (ui && state) {
        state->active = false;
        state->initialized = false;
        s_stage = "deinit";
        remember(ui, esp_gsp_on_event(ui, NULL, NULL));
        if (state->timer) {
            const esp_gsp_err_t err = esp_gsp_timer_delete(ui, state->timer);
            remember(ui, err);
            if (err == ESP_GSP_OK) state->timer = NULL;
        }
        state->last_error = s_last_error;
    }
    s_ui = NULL;
    s_model = NULL;
}
