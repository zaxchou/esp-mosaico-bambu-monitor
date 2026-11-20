// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "bmb_state.h"

/* Print control commands. The transport implementation publishes the Bambu
 * LAN request payload and reports progress through model->ctrl_busy /
 * model->ctrl_status. Requires Developer Mode (LAN control) on the printer;
 * rejection is surfaced as a status message, not an error code. */
typedef enum {
    BMB_CMD_PAUSE = 0,
    BMB_CMD_RESUME,
    BMB_CMD_STOP,
} bmb_cmd_t;

/* Send one command. Returns false when the link is offline, the serial is
 * unknown, or a command is still in flight. */
bool bmb_ctrl_send(bmb_state_t *model, bmb_cmd_t cmd);

/* Quick settings: print speed level (1=quiet, 2=standard, 3=sport) and the
 * chamber light. Same request topic; also gated by ctrl_busy. */
bool bmb_ctrl_set_speed(bmb_state_t *model, int level);
bool bmb_ctrl_set_light(bmb_state_t *model, bool on);
