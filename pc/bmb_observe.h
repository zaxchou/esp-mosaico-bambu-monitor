// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bmb_ui.h"
#include <time.h>

/* Host-only diagnostics. The native UI task owns all observations. No state
 * changes or printer I/O; unset BMB_SIM_OBSERVE_FILE disables file output. */
void bmb_observe_text(const char *name, const char *text);
void bmb_observe_page(int page);
time_t bmb_observe_wall_time(time_t real_time);
void bmb_observe_snapshot(const bmb_ui_t *ui, const bmb_state_t *model,
                          bool stop_armed, int last_error);
