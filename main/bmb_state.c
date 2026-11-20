// SPDX-License-Identifier: Apache-2.0

#include "bmb_state.h"

#include <limits.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
/* Application-wide model lock, initialized before UI/transport tasks start.
 * Static storage avoids an allocation failure silently disabling locking. */
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
void bmb_state_lock(void)
{
    configASSERT(s_lock);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
void bmb_state_unlock(void)
{
    configASSERT(s_lock);
    xSemaphoreGive(s_lock);
}
#else
void bmb_state_lock(void) {}
void bmb_state_unlock(void) {}
#endif

void bmb_state_init(bmb_state_t *state)
{
#ifdef ESP_PLATFORM
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        configASSERT(s_lock);
    }
#endif
    /* Snapshot/freshness flags and alert generations start at zero. */
    *state = (bmb_state_t){
        .link = BMB_LINK_OFFLINE,
        .print_state = BMB_PRINT_IDLE,
        .session_epoch = 1,
        .ams_humidity = -1,
        .ams_temp = INT16_MIN,
        .remain_min = -1,
        .spd_lvl = -1,
        .light_on = -1,
        .wifi_dbm = INT16_MIN,
    };
    for (int i = 0; i < BMB_AMS_TRAYS; ++i) {
        state->tray_remain[i] = -1;
    }
}

void bmb_state_reset_session(bmb_state_t *state)
{
    const uint32_t epoch = state->session_epoch;
    bmb_state_init(state);
    state->session_epoch = epoch + 1U;
}

const char *bmb_link_text(bmb_link_t link)
{
    switch (link) {
    case BMB_LINK_ONLINE: return "在线";
    case BMB_LINK_CONNECTING: return "连接中";
    case BMB_LINK_OFFLINE: break;
    }
    return "离线";
}

const char *bmb_print_state_text(bmb_print_state_t state)
{
    switch (state) {
    case BMB_PRINT_IDLE: return "空闲";
    case BMB_PRINT_PREPARE: return "准备中";
    case BMB_PRINT_SLICING: return "切片中";
    case BMB_PRINT_RUNNING: return "打印中";
    case BMB_PRINT_PAUSE: return "已暂停";
    case BMB_PRINT_FINISH: return "已完成";
    case BMB_PRINT_ERROR: return "打印出错";
    case BMB_PRINT_OTHER: break;
    }
    return "未知状态";
}
