// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Command metadata belongs to the status model. All *_locked functions require
 * bmb_state_lock(); native regression runs them on one thread. Payload storage
 * and MQTT I/O belong to the transport, outside the model lock. */
#define BMB_COMMAND_PARTS 2
#define BMB_COMMAND_TIMEOUT_MS 8000U
#define BMB_TELEMETRY_FRESH_MS 15000U
#define BMB_STOP_CONFIRM_MS 5000U

/* First-tap snapshot. The second tap must begin STOP while this exact
 * device/task/session/state is still current; all access uses the model lock. */
typedef struct {
    bool armed;
    uint32_t started_ms;
    uint32_t session_epoch;
    char device_id[24];
    char task[56];
    uint32_t task_hash;
    int print_state;
} bmb_stop_confirmation_t;

typedef enum {
    BMB_COMMAND_PAUSE = 0,
    BMB_COMMAND_RESUME,
    BMB_COMMAND_STOP,
    BMB_COMMAND_SPEED,
    BMB_COMMAND_LIGHT,
} bmb_command_kind_t;

typedef struct {
    bmb_command_kind_t kind;
    uint32_t token;
    uint32_t started_ms;
    uint32_t sequence[BMB_COMMAND_PARTS];
    unsigned parts;
    unsigned sent_mask;
    unsigned ack_mask;
    bool failed;
    char device_id[24];
    char task[56];
    uint32_t task_hash;
    int queued_print_state;
    char reason[24];
} bmb_command_state_t;

struct bmb_state;
bool bmb_command_stop_arm_locked(struct bmb_state *model, uint32_t now_ms);
/* Validation also disarms an expired or invalid confirmation. STOP begin
 * repeats this check and consumes it only after validating command sequences. */
bool bmb_command_stop_valid_locked(struct bmb_state *model, uint32_t now_ms);
void bmb_command_stop_disarm_locked(struct bmb_state *model);
bool bmb_command_begin_locked(struct bmb_state *model, bmb_command_kind_t kind,
                              const uint32_t *sequences, unsigned parts,
                              uint32_t now_ms);
bool bmb_command_can_send_locked(struct bmb_state *model, uint32_t token,
                                 unsigned part, uint32_t now_ms);
void bmb_command_sent_locked(struct bmb_state *model, uint32_t token,
                             unsigned part, bool success);
void bmb_command_cancel_locked(struct bmb_state *model, const char *reason);
void bmb_command_tick_locked(struct bmb_state *model, uint32_t now_ms);
bool bmb_command_ack_locked(struct bmb_state *model, const char *topic,
                            size_t topic_len, bool system_family,
                            const char *command, uint32_t sequence,
                            const char *result, const char *reason,
                            uint32_t now_ms);
