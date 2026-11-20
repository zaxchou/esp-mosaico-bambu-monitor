// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>

#include "bmb_link.h"
#include "bmb_ui.h"
#include "bmb_state.h"
#include "gsp_sim_bridge.h"

static bmb_ui_t ui_state;
static bmb_state_t model;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    bmb_state_init(&model);
    const esp_gsp_err_t err = bmb_ui_init(ui, &ui_state, &model);
    /* Host replay transport: feeds scripted reports into the same model. */
    bmb_link_start(&model);
    return err;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    bmb_ui_deinit(ui, &ui_state);
    fprintf(stderr, "bambu_backend: last_error=%d\n", (int)ui_state.last_error);
}
