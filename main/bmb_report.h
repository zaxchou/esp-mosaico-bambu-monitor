// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "bmb_state.h"

/* Parse one complete Bambu `device/<serial>/report` JSON document. Malformed,
 * truncated, oversized or excessively nested documents leave the model intact.
 * Missing fields preserve their values; explicit empty AMS strings/null clear.
 * Returns true for recognized telemetry or a complete command acknowledgement;
 * an unmatched ACK can return true without changing the model. Only telemetry
 * refreshes last_report_ms/link; command matching owns control feedback. */
bool bmb_report_apply(bmb_state_t *state, const char *topic, size_t topic_len,
                      const char *json, size_t json_len, uint32_t now_ms);
