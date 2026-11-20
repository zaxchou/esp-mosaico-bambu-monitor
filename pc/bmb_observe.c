// SPDX-License-Identifier: Apache-2.0
#include "bmb_observe.h"
#include "bmb_link.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBSERVED_TEXTS 24
#define OBSERVED_TEXT_CAPACITY 256

/* Single owner: native bridge/UI task. These are actual setter arguments,
 * captured without querying or mutating the simulator's application state. */
static struct {
    const char *name;
    char text[OBSERVED_TEXT_CAPACITY];
} s_text[OBSERVED_TEXTS];
static int s_page;

void bmb_observe_text(const char *name, const char *text)
{
    for (size_t i = 0; i < OBSERVED_TEXTS; ++i) {
        if (!s_text[i].name || strcmp(s_text[i].name, name) == 0) {
            s_text[i].name = name;
            snprintf(s_text[i].text, sizeof(s_text[i].text), "%s", text);
            return;
        }
    }
}

void bmb_observe_page(int page)
{
    s_page = page;
}

time_t bmb_observe_wall_time(time_t real_time)
{
    const char *epoch = getenv("BMB_SIM_EPOCH");
    if (!epoch || !*epoch) return real_time;
    char *end;
    long long value = strtoll(epoch, &end, 10);
    if (end == epoch || *end || value <= 1600000000) return real_time;
    return (time_t)value + (time_t)(bmb_now_ms() / 1000);
}

static void json_string(FILE *fp, const char *text)
{
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', fp);
            fputc(*p, fp);
        } else if (*p < 32) {
            fprintf(fp, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

void bmb_observe_snapshot(const bmb_ui_t *ui, const bmb_state_t *model,
                          bool stop_armed, int last_error)
{
    const char *path = getenv("BMB_SIM_OBSERVE_FILE");
    if (!path || !*path) return;
    FILE *fp = fopen(path, "ab");
    if (!fp) return;
    fprintf(fp, "{\"tick\":%u,\"now_ms\":%" PRIu32 ",\"page\":%d,"
                "\"alert_kind\":%u,\"alert_generation\":%" PRIu32 ","
                "\"stop_armed\":%s,\"last_error\":%d,"
                "\"print_state\":%d,\"link\":%d,\"print_error\":%" PRIu32 ","
                "\"error_seen\":%s,\"finish_seen\":%s,"
                "\"error_generation\":%" PRIu32 ",\"finish_generation\":%" PRIu32 ","
                "\"ctrl_busy\":%s,\"light_on\":%d,\"remain_min\":%d,"
                "\"remaining_updated_ms\":%" PRIu32 ",\"last_report_ms\":%" PRIu32 ","
                "\"task\":",
            ui->tick_count, bmb_now_ms(), s_page, ui->alert_kind,
            ui->alert_generation, stop_armed ? "true" : "false", last_error,
            model->print_state, model->link, model->print_error,
            model->error_seen ? "true" : "false",
            model->finish_seen ? "true" : "false",
            model->error_generation, model->finish_generation,
            model->ctrl_busy ? "true" : "false", model->light_on,
            model->remain_min, model->remaining_updated_ms, model->last_report_ms);
    json_string(fp, model->task);
    fprintf(fp, ",\"session_epoch\":%" PRIu32 ",\"alert_epoch\":%" PRIu32,
            model->session_epoch, ui->alert_epoch);
    fputs(",\"finish_task_snapshot\":", fp);
    json_string(fp, model->finish_task);
    fputs(",\"error_task_snapshot\":", fp);
    json_string(fp, model->error_task);
    fprintf(fp, ",\"alert_error_code\":%" PRIu32 ",\"spd_lvl\":%d,"
                "\"pad_len\":%u,\"command_kind\":%d,\"command_token\":%" PRIu32
                ",\"command_parts\":%u,\"command_sent_mask\":%u,"
                "\"command_ack_mask\":%u,\"command_sequence\":[%" PRIu32 ",%" PRIu32 "],"
                "\"text\":{", model->alert_error_code, model->spd_lvl, ui->pad_len,
            (int)model->command.kind, model->command.token, model->command.parts,
            model->command.sent_mask, model->command.ack_mask,
            model->command.sequence[0], model->command.sequence[1]);
    bool comma = false;
    for (size_t i = 0; i < OBSERVED_TEXTS && s_text[i].name; ++i) {
        if (comma) fputc(',', fp);
        json_string(fp, s_text[i].name);
        fputc(':', fp);
        json_string(fp, s_text[i].text);
        comma = true;
    }
    fputs("}}\n", fp);
    fclose(fp);
}
