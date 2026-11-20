// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bmb_command.h"

/* Status model shared by transport and UI. Transport owns telemetry; UI and
 * control paths update acknowledgement flags and command feedback under the
 * same application-wide lock. Initialize before starting either task. The
 * host backend runs these operations on one thread and uses no-op locks. */

typedef enum {
    BMB_LINK_OFFLINE = 0,  /* not configured, or Wi-Fi/MQTT down */
    BMB_LINK_CONNECTING,   /* Wi-Fi up, MQTT handshake in progress */
    BMB_LINK_ONLINE,       /* subscribed, reports flowing */
} bmb_link_t;

typedef enum {
    BMB_PRINT_IDLE = 0,
    BMB_PRINT_PREPARE,
    BMB_PRINT_SLICING,
    BMB_PRINT_RUNNING,
    BMB_PRINT_PAUSE,
    BMB_PRINT_FINISH,
    BMB_PRINT_ERROR,
    BMB_PRINT_OTHER,
} bmb_print_state_t;

#define BMB_AMS_TRAYS_PER_UNIT 4
#define BMB_AMS_TRAYS (2 * BMB_AMS_TRAYS_PER_UNIT)
#define BMB_SPEED_LEVELS 3            /* quiet / standard / sport */
#define BMB_FILAMENT_TYPE_MAX 16

typedef struct bmb_state {
    bmb_link_t link;
    bmb_print_state_t print_state;
    uint8_t percent;                /* 0..100 */
    bool has_task;                  /* a task name is known */
    char task[56];                  /* subtask_name (fallback: gcode_file), display copy */
    uint32_t task_hash;             /* hash of the full task name for identity changes */
    uint16_t layer;
    uint16_t layers;
    int16_t nozzle;                 /* degrees C, rounded */
    int16_t nozzle_target;
    int16_t bed;
    int16_t bed_target;
    int16_t chamber;
    char dev_id[24];                /* printer serial, from the report topic */
    uint32_t last_report_ms;        /* monotonic ms of the last accepted report */
    uint32_t first_report_ms;       /* monotonic ms of the first telemetry report */
    bool state_received;            /* initial telemetry snapshot has arrived */
    bool link_resetting;            /* reject callbacks from an ending session */
    uint32_t session_epoch;         /* alert generations are scoped to this session */
    bool finish_seen;               /* a FINISH transition arrived since last dismiss */
    uint32_t finish_generation;     /* incremented for each live FINISH transition */
    /* AMS snapshot: only present while the printer reports the ams object.
     * remain uses the printer convention: <=0 or -1 = empty/unknown slot.
     * tray_type is the loaded filament kind ("PLA"/"PC"/...), empty=none. */
    bool ams_present;
    int8_t ams_humidity;            /* printer humidity index, -1 unknown */
    int16_t ams_temp;               /* degrees C, rounded, INT16_MIN unknown */
    int16_t tray_remain[BMB_AMS_TRAYS];
    char tray_color[BMB_AMS_TRAYS][9]; /* "RRGGBBAA" from tray_color, empty=unknown */
    char tray_type[BMB_AMS_TRAYS][BMB_FILAMENT_TYPE_MAX]; /* empty=unknown */
    /* Print control: the command path fills ctrl_status; the UI shows it. */
    bool ctrl_busy;                 /* waiting for the printer's ack */
    char ctrl_status[40];
    uint32_t print_error;           /* last print_error code, 0 = none */
    bool error_seen;                /* a new error arrived since last dismiss */
    uint32_t error_generation;      /* incremented for each new live error */
    char finish_task[56];           /* payload captured with finish_generation */
    char error_task[56];            /* payload captured with error_generation */
    uint32_t alert_error_code;      /* error code captured for the pending alert */
    bmb_command_state_t command;    /* pending request, guarded by model lock */
    bmb_stop_confirmation_t stop_confirmation; /* bound to one device/task/session */
    /* Extra status the printer pushes every second. */
    char model_id[28];              /* printer model_id ("CN..." -> H2D) */
    int16_t remain_min;             /* mc_remaining_time, minutes, -1 unknown */
    uint32_t remaining_updated_ms; /* last real mc_remaining_time update */
    int8_t spd_lvl;                 /* print speed level, -1 unknown */
    int8_t light_on;                /* chamber light, -1 unknown */
    int16_t wifi_dbm;               /* wifi_signal, INT16_MIN unknown */
} bmb_state_t;

void bmb_state_init(bmb_state_t *state);
/* Caller holds the model lock; used after the previous transport has stopped. */
void bmb_state_reset_session(bmb_state_t *state);
void bmb_state_lock(void);
void bmb_state_unlock(void);

const char *bmb_link_text(bmb_link_t link);
const char *bmb_print_state_text(bmb_print_state_t state);
const char *bmb_print_state_raw(bmb_print_state_t state);
