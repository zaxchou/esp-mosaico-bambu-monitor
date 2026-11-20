// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_gsp.h"

#include "bmb_state.h"

typedef struct {
    esp_gsp_err_t last_error;
    void *timer;
    /* keypad editing */
    int edit_field;      /* 0 none, 1 = IP, 2 = access code */
    char pad_buf[16];
    unsigned pad_len;
    unsigned alert_kind;       /* 0 none, 1 completion, 2 error */
    uint32_t alert_generation;
    uint32_t alert_epoch;
    bool initialized;              /* first render-task tick applied initial UI */
    bool active;                   /* callbacks are inert after teardown */
    /* host replay driver advances on the UI tick */
    unsigned tick_count;
} bmb_ui_t;

esp_gsp_err_t bmb_ui_init(esp_gsp_handle_t ui, bmb_ui_t *state, bmb_state_t *model);
/* Application singleton. Teardown is serialized with callbacks (native bridge
 * owner thread, or a stopped render runtime); it is not a cross-task join.
 * Keep state alive through runtime shutdown if callback/timer removal fails. */
void bmb_ui_deinit(esp_gsp_handle_t ui, bmb_ui_t *state);
