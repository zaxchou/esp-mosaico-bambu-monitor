// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bmb_state.h"

/* Transport lifecycle. The device build runs Wi-Fi + MQTT; the host replay
 * build advances a scripted report stream from the UI tick instead. */

void bmb_link_start(bmb_state_t *model);
void bmb_link_stop(void);
/* Nonblocking request: the device lifecycle task performs teardown/reconnect. */
bool bmb_link_restart(bmb_state_t *model);

/* Wall-clock milliseconds for the stale-report watchdog. */
uint32_t bmb_now_ms(void);
