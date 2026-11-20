// SPDX-License-Identifier: Apache-2.0
/* Host fault injection against the separately compiled, unmodified bmb_ui.c.
 * Uses pinned esp_gsp.h and the actual gspc bundle helpers/scene action decoders.
 * Only GSP resource/setter boundaries and platform I/O are faked. No UI private
 * functions or globals are inspected. See run_ui_startup_tests.py for building.
 *
 * GSP 1.5.1 esp_gsp.h: events/timers run on the render task; portable callers
 * serialize them. sim_bridge/src/transport.c: NULL replaces the event callback;
 * failed registration leaves it unchanged, failed timer creation returns NULL;
 * timers become due after one period and coalesce missed periods. These tests
 * run serially and do not claim to cover in-flight device callback teardown.
 */
#include "bmb_ui.h"
#include "bmb_buzz.h"
#include "bmb_config.h"
#include "bmb_ctrl.h"
#include "bmb_host_link.h"
#include "bmb_link.h"
#include "bmb_observe.h"
#include "bmb_report.h"
#include "bundle_gsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *case_name;
static unsigned checks, groups;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL [%s] %s:%d: %s\n", case_name, __FILE__, __LINE__, #expr); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { MAX_COMPONENTS = 96, MAX_TIMERS = 4, TEXT_CAPACITY = 192 };
typedef enum { SET_TEXT, SET_VISIBLE, SET_VALUE, SET_COLOR } setter_kind_t;
typedef struct {
    gsp_component_key_t key;
    char text[TEXT_CAPACITY];
    bool visible;
    int32_t value;
    uint32_t color;
} component_t;
typedef struct {
    bool live;
    uint32_t period, due;
    esp_gsp_timer_cb_t callback;
    void *ctx;
} timer_t;

struct gsp_ui_core {
    esp_gsp_event_cb_t callback;
    void *callback_ctx;
    timer_t timers[MAX_TIMERS];
    unsigned register_calls, unregister_calls;
    unsigned timer_attempts, timer_created, timer_deleted, live_timers;
    unsigned setters, rejected_setters;
    bool fail_register, fail_unregister, fail_timer, fail_timer_delete, fail_setter;
    setter_kind_t fail_kind;
    gsp_component_key_t fail_key;
    component_t components[MAX_COMPONENTS];
    unsigned component_count;
};

static struct gsp_ui_core engine;
static bmb_ui_t ui_state;
static bmb_state_t model;
static uint32_t now_ms;
static unsigned controls, speeds, lights, restarts, saves, buzzes, host_ticks;
static bmb_cmd_t last_control;
static char last_control_task[56];
static bmb_pairing_t saved_pairing;
static bool in_render;

static void begin_case(const char *name)
{
    case_name = name;
    /* Never hide a previous case's leaked registrations by zeroing the fake. */
    CHECK(engine.callback == NULL && engine.live_timers == 0);
    memset(&engine, 0, sizeof(engine));
    memset(&ui_state, 0, sizeof(ui_state));
    bmb_state_init(&model);
    now_ms = 1000;
    controls = speeds = lights = restarts = saves = buzzes = host_ticks = 0;
    in_render = false;
}

static void pass(void)
{
    ++groups;
    printf("PASS %s\n", case_name);
}

static component_t *component(gsp_component_key_t key)
{
    for (unsigned i = 0; i < engine.component_count; ++i) {
        if (engine.components[i].key == key) return &engine.components[i];
    }
    CHECK(engine.component_count < MAX_COMPONENTS);
    component_t *entry = &engine.components[engine.component_count++];
    *entry = (component_t){ .key = key };
    return entry;
}

static bool reject_setter(esp_gsp_handle_t ui, setter_kind_t kind,
                          gsp_component_key_t key)
{
    CHECK(ui == &engine);
    /* Initial setters on the init caller instead of the timer are a regression. */
    CHECK(in_render);
    ++engine.setters;
    if (engine.fail_setter && engine.fail_key == key && engine.fail_kind == kind) {
        ++engine.rejected_setters;
        return true;
    }
    return false;
}

esp_gsp_err_t esp_gsp_component_set_text(esp_gsp_handle_t ui,
                                        gsp_component_key_t key, const char *text)
{
    CHECK(text != NULL && strlen(text) < TEXT_CAPACITY);
    if (reject_setter(ui, SET_TEXT, key)) return ESP_GSP_ERR_TIMEOUT;
    snprintf(component(key)->text, TEXT_CAPACITY, "%s", text);
    return ESP_GSP_OK;
}

esp_gsp_err_t esp_gsp_component_set_visible(esp_gsp_handle_t ui,
                                           gsp_component_key_t key, bool visible)
{
    if (reject_setter(ui, SET_VISIBLE, key)) return ESP_GSP_ERR_TIMEOUT;
    component(key)->visible = visible;
    return ESP_GSP_OK;
}

esp_gsp_err_t esp_gsp_component_set_value(esp_gsp_handle_t ui,
                                         gsp_component_key_t key, int32_t value)
{
    if (reject_setter(ui, SET_VALUE, key)) return ESP_GSP_ERR_TIMEOUT;
    component(key)->value = value;
    return ESP_GSP_OK;
}

esp_gsp_err_t esp_gsp_component_set_property_color_rgb888(
        esp_gsp_handle_t ui, gsp_component_key_t key,
        gsp_property_key_t property, uint32_t color)
{
    CHECK(property == GSP_PROPERTY_KEY_COLOR);
    if (reject_setter(ui, SET_COLOR, key)) return ESP_GSP_ERR_TIMEOUT;
    component(key)->color = color;
    return ESP_GSP_OK;
}

esp_gsp_err_t esp_gsp_on_event(esp_gsp_handle_t ui, esp_gsp_event_cb_t cb, void *ctx)
{
    CHECK(ui == &engine);
    if (cb) {
        ++engine.register_calls;
        if (engine.fail_register) return ESP_GSP_ERR_INVALID_STATE;
    } else {
        ++engine.unregister_calls;
        if (engine.fail_unregister) return ESP_GSP_ERR_TIMEOUT;
    }
    engine.callback = cb;
    engine.callback_ctx = ctx;
    return ESP_GSP_OK;
}

void *esp_gsp_timer_create(esp_gsp_handle_t ui, uint32_t period,
                            esp_gsp_timer_cb_t cb, void *ctx)
{
    CHECK(ui == &engine && period > 0 && cb != NULL);
    ++engine.timer_attempts;
    if (engine.fail_timer) return NULL;
    for (unsigned i = 0; i < MAX_TIMERS; ++i) {
        timer_t *timer = &engine.timers[i];
        if (!timer->live) {
            *timer = (timer_t){ .live = true, .period = period,
                .due = now_ms + period, .callback = cb, .ctx = ctx };
            ++engine.timer_created;
            ++engine.live_timers;
            return timer;
        }
    }
    return NULL;
}

esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t ui, void *handle)
{
    CHECK(ui == &engine);
    if (engine.fail_timer_delete) return ESP_GSP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < MAX_TIMERS; ++i) {
        timer_t *timer = &engine.timers[i];
        if (handle == timer && timer->live) {
            timer->live = false;
            ++engine.timer_deleted;
            --engine.live_timers;
            return ESP_GSP_OK;
        }
    }
    CHECK(!"delete of a stale or foreign timer");
    return ESP_GSP_ERR_NOT_FOUND;
}

/* Platform fakes record effects; the model, command timeout, JSON parser and
 * report transitions are linked from main/ rather than reproduced here. */
bool bmb_pairing_load(bmb_pairing_t *cfg)
{
    *cfg = (bmb_pairing_t){ .ip = "192.0.2.8", .code = "test1234", .valid = true };
    return true;
}
bool bmb_pairing_save(const bmb_pairing_t *cfg)
{
    ++saves;
    saved_pairing = *cfg;
    return true;
}
bool bmb_link_restart(bmb_state_t *state)
{
    CHECK(state == &model);
    ++restarts;
    return true;
}
uint32_t bmb_now_ms(void) { return now_ms; }
void bmb_host_link_tick(void) { ++host_ticks; }
void bmb_buzz_once(void) { ++buzzes; }
bool bmb_ctrl_send(bmb_state_t *state, bmb_cmd_t cmd)
{
    CHECK(state == &model);
    ++controls;
    last_control = cmd;
    snprintf(last_control_task, sizeof(last_control_task), "%s", state->task);
    return true;
}
bool bmb_ctrl_set_speed(bmb_state_t *state, int level)
{
    CHECK(state == &model && level >= 1 && level <= 3);
    ++speeds;
    return true;
}
bool bmb_ctrl_set_light(bmb_state_t *state, bool on)
{
    CHECK(state == &model);
    (void)on;
    ++lights;
    return true;
}
/* Observers also run when setters FAIL; assertions deliberately read the GSP
 * fake's committed state instead of trusting those attempted-write observers. */
void bmb_observe_text(const char *name, const char *text) { (void)name; (void)text; }
void bmb_observe_page(int page) { (void)page; }
time_t bmb_observe_wall_time(time_t real_time) { (void)real_time; return 1700000000; }
void bmb_observe_snapshot(const bmb_ui_t *ui, const bmb_state_t *state,
                          bool stop_armed, int last_error)
{
    (void)ui; (void)state; (void)stop_armed; (void)last_error;
}

static void advance(uint32_t elapsed)
{
    now_ms += elapsed;
    CHECK(!in_render);
    in_render = true;
    for (unsigned i = 0; i < MAX_TIMERS; ++i) {
        timer_t *timer = &engine.timers[i];
        if (timer->live && (int32_t)(now_ms - timer->due) >= 0) {
            timer->due = now_ms + timer->period;
            timer->callback(&engine, timer->ctx);
        }
    }
    in_render = false;
}

static void click(gsp_bmb_action_t action)
{
    const esp_gsp_event_t event = {
        .type = ESP_GSP_EVENT_CALL, .action_id = (uint16_t)action,
        .scene_id = 0, .list = ESP_GSP_LIST_NONE,
    };
    CHECK(!in_render);
    in_render = true;
    if (engine.callback) engine.callback(&engine, &event, engine.callback_ctx);
    in_render = false;
}

static void expect_text(gsp_component_key_t key, const char *text)
{
    const char *actual = component(key)->text;
    if (strcmp(actual, text)) {
        fprintf(stderr, "text mismatch: expected '%s', got '%s'\n", text, actual);
    }
    CHECK(strcmp(actual, text) == 0);
}

static void expect_page(gsp_component_key_t visible)
{
    static const gsp_component_key_t pages[] = {
        GSP_BMB_OBJ_KEY_STATUS_PAGE, GSP_BMB_OBJ_KEY_SET_PAGE,
        GSP_BMB_OBJ_KEY_PAD_PAGE, GSP_BMB_OBJ_KEY_FINISH_PAGE,
        GSP_BMB_OBJ_KEY_CTRL_PAGE, GSP_BMB_OBJ_KEY_AMS_PAGE,
    };
    unsigned matches = 0;
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        matches += pages[i] == visible;
        CHECK(component(pages[i])->visible == (pages[i] == visible));
    }
    CHECK(matches == 1);
}

static void feed(const char *json)
{
    static const char topic[] = "device/TEST-PRINTER/report";
    CHECK(bmb_report_apply(&model, topic, sizeof(topic) - 1,
                           json, strlen(json), now_ms));
}

static void reject_all_input(void)
{
    const unsigned setters = engine.setters;
    const bmb_state_t before = model;
    const bmb_ui_t before_ui = ui_state;
    /* Cover every authored action: navigation, keypad, modal ACK, persistence,
     * reconnect and printer commands. Even a retained callback must be inert. */
    for (unsigned i = 0; i <= (unsigned)GSP_BMB_ACTION_SET_SAVE; ++i) {
        click((gsp_bmb_action_t)i);
    }
    CHECK(engine.setters == setters);
    CHECK(!memcmp(&model, &before, sizeof(model)));
    CHECK(!memcmp(&ui_state, &before_ui, sizeof(ui_state)));
    CHECK(controls == 0 && speeds == 0 && lights == 0 && restarts == 0 && saves == 0);
}

static void start(void)
{
    CHECK(bmb_ui_init(&engine, &ui_state, &model) == ESP_GSP_OK);
    CHECK(engine.callback != NULL && engine.live_timers == 1);
    CHECK(ui_state.timer != NULL && !ui_state.initialized);
    CHECK(engine.setters == 0);  /* no render-task calls on init caller */
}

static void stop(void)
{
    bmb_ui_deinit(&engine, &ui_state);
    CHECK(engine.callback == NULL && engine.callback_ctx == NULL);
    CHECK(engine.live_timers == 0 && ui_state.timer == NULL);
    CHECK(engine.timer_created == engine.timer_deleted);
    const unsigned ticks = ui_state.tick_count, setters = engine.setters;
    advance(1000);
    click(GSP_BMB_ACTION_CTRL_OPEN);
    CHECK(ui_state.tick_count == ticks && engine.setters == setters);
    /* Deleting twice must not reuse or double-delete a timer handle. */
    bmb_ui_deinit(&engine, &ui_state);
    CHECK(engine.timer_created == engine.timer_deleted);
}

static void expect_retry(void)
{
    engine.fail_register = engine.fail_timer = engine.fail_unregister = false;
    start();
    reject_all_input();
    advance(100);
    CHECK(ui_state.initialized && ui_state.tick_count == 1);
    expect_page(GSP_BMB_OBJ_KEY_STATUS_PAGE);
    click(GSP_BMB_ACTION_CTRL_OPEN);
    expect_page(GSP_BMB_OBJ_KEY_CTRL_PAGE);
    click(GSP_BMB_ACTION_CTRL_PAUSE);
    CHECK(controls == 1 && last_control == BMB_CMD_PAUSE);
    stop();
}

static void test_callback_failure(void)
{
    begin_case("callback registration failure is inert; retry succeeds");
    engine.fail_register = true;
    CHECK(bmb_ui_init(&engine, &ui_state, &model) == ESP_GSP_ERR_INVALID_STATE);
    CHECK(ui_state.last_error == ESP_GSP_ERR_INVALID_STATE);
    CHECK(!ui_state.initialized && ui_state.timer == NULL);
    CHECK(engine.callback == NULL && engine.timer_attempts == 0);
    CHECK(engine.live_timers == 0 && engine.setters == 0);
    reject_all_input();
    advance(500);
    CHECK(ui_state.tick_count == 0 && host_ticks == 0);
    expect_retry();
    pass();
}

static void test_timer_failure(bool cleanup_failure)
{
    begin_case(cleanup_failure ?
        "timer allocation + callback cleanup failure stay inert; retry succeeds" :
        "timer allocation failure unregisters input without leaks; retry succeeds");
    engine.fail_timer = true;
    engine.fail_unregister = cleanup_failure;
    CHECK(bmb_ui_init(&engine, &ui_state, &model) == ESP_GSP_ERR_NO_MEM);
    CHECK(ui_state.last_error == ESP_GSP_ERR_NO_MEM);
    CHECK(!ui_state.initialized && ui_state.timer == NULL);
    CHECK(engine.register_calls == 1 && engine.unregister_calls == 1);
    CHECK((engine.callback != NULL) == cleanup_failure);
    CHECK(engine.timer_attempts == 1 && engine.timer_created == 0);
    CHECK(engine.live_timers == 0 && engine.setters == 0);
    reject_all_input();
    advance(500);
    CHECK(ui_state.tick_count == 0 && host_ticks == 0);
    expect_retry();
    CHECK(engine.timer_attempts == 2 && engine.timer_created == 1);
    pass();
}

static void test_initial_setter_failure(const char *name, setter_kind_t kind,
                                         gsp_component_key_t key)
{
    begin_case(name);
    engine.fail_setter = true;
    engine.fail_kind = kind;
    engine.fail_key = key;
    /* Give the failed property a committed sentinel so accepting a rejected
     * write cannot masquerade as a successful retry. */
    component_t *entry = component(key);
    snprintf(entry->text, sizeof(entry->text), "not yet rendered");
    entry->value = -77;
    entry->color = 0xABCDEF;
    const component_t before = *entry;
    start();
    reject_all_input();
    for (unsigned i = 0; i < 2; ++i) {
        advance(100);
        CHECK(!ui_state.initialized);
        CHECK(engine.rejected_setters > i);
        CHECK(!memcmp(entry, &before, sizeof(before)));
        CHECK(engine.live_timers == 1 && engine.timer_created == 1);
        reject_all_input();
    }
    const unsigned failures = engine.rejected_setters;
    const unsigned ticks_before_retry = ui_state.tick_count;
    engine.fail_setter = false;
    advance(100);
    CHECK(ui_state.initialized && ui_state.tick_count == ticks_before_retry + 1);
    CHECK(engine.rejected_setters == failures);
    expect_page(GSP_BMB_OBJ_KEY_STATUS_PAGE);
    expect_text(GSP_BMB_OBJ_KEY_SET_IP_LBL, "IP  192.0.2.8");
    expect_text(GSP_BMB_OBJ_KEY_SET_CODE_LBL, "访问码  test1234");
    expect_text(GSP_BMB_OBJ_KEY_LINK_LBL, "离线 ");
    CHECK(component(GSP_BMB_OBJ_KEY_PROG)->value == 0);
    CHECK(component(GSP_BMB_OBJ_KEY_AMS_SWATCH_0)->color == 0x2A3743);
    click(GSP_BMB_ACTION_SET_OPEN);
    expect_page(GSP_BMB_OBJ_KEY_SET_PAGE);
    advance(100);
    expect_page(GSP_BMB_OBJ_KEY_SET_PAGE); /* successful startup stops retrying */
    stop();
    pass();
}

static void test_normal_navigation(void)
{
    begin_case("normal render, periodic refresh, navigation, keypad and stop timer");
    start();
    reject_all_input();
    advance(99);
    CHECK(!ui_state.initialized && ui_state.tick_count == 0 && engine.setters == 0);
    advance(1);
    CHECK(ui_state.initialized && ui_state.tick_count == 1 && host_ticks == 1);
    expect_page(GSP_BMB_OBJ_KEY_STATUS_PAGE);
    feed("{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"normal-job\","
         "\"mc_percent\":37,\"layer_num\":12,\"total_layer_num\":100,"
         "\"nozzle_temper\":210,\"nozzle_target_temper\":220}}");
    advance(100);
    expect_text(GSP_BMB_OBJ_KEY_TASK2_LBL, "任务  normal-job");
    expect_text(GSP_BMB_OBJ_KEY_PCT_LBL, "37%");
    expect_text(GSP_BMB_OBJ_KEY_NOZZLE_LBL, "喷嘴 210°/220°");
    CHECK(component(GSP_BMB_OBJ_KEY_PROG)->value == 37);
    CHECK(component(GSP_BMB_OBJ_KEY_PRINT_BAR)->visible);
    CHECK(!component(GSP_BMB_OBJ_KEY_IDLE_BAR)->visible);
    click(GSP_BMB_ACTION_AMS_OPEN);
    expect_page(GSP_BMB_OBJ_KEY_AMS_PAGE);
    advance(100);
    expect_page(GSP_BMB_OBJ_KEY_AMS_PAGE);
    click(GSP_BMB_ACTION_AMS_BACK);
    expect_page(GSP_BMB_OBJ_KEY_STATUS_PAGE);
    click(GSP_BMB_ACTION_SET_OPEN);
    click(GSP_BMB_ACTION_SET_IP);
    expect_page(GSP_BMB_OBJ_KEY_PAD_PAGE);
    expect_text(GSP_BMB_OBJ_KEY_PAD_FIELD, "192.0.2.8");
    click(GSP_BMB_ACTION_PAD_CLR);
    click(GSP_BMB_ACTION_PAD_OK);
    expect_page(GSP_BMB_OBJ_KEY_PAD_PAGE);
    expect_text(GSP_BMB_OBJ_KEY_PAD_ERROR, "IP 无效");
    click(GSP_BMB_ACTION_PAD_1);
    click(GSP_BMB_ACTION_PAD_DOT);
    click(GSP_BMB_ACTION_PAD_2);
    click(GSP_BMB_ACTION_PAD_DOT);
    click(GSP_BMB_ACTION_PAD_3);
    click(GSP_BMB_ACTION_PAD_DOT);
    click(GSP_BMB_ACTION_PAD_4);
    click(GSP_BMB_ACTION_PAD_OK);
    expect_page(GSP_BMB_OBJ_KEY_SET_PAGE);
    expect_text(GSP_BMB_OBJ_KEY_SET_IP_LBL, "IP  1.2.3.4");
    click(GSP_BMB_ACTION_SET_SAVE);
    CHECK(saves == 1 && restarts == 1);
    CHECK(saved_pairing.valid && !strcmp(saved_pairing.ip, "1.2.3.4"));
    click(GSP_BMB_ACTION_SET_BACK);
    expect_page(GSP_BMB_OBJ_KEY_STATUS_PAGE);
    click(GSP_BMB_ACTION_CTRL_OPEN);
    expect_page(GSP_BMB_OBJ_KEY_CTRL_PAGE);
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 0);
    expect_text(GSP_BMB_OBJ_KEY_CTRL_STOP_BTN_LBL, "再按一次确认停止");
    for (unsigned i = 0; i < 51; ++i) advance(100);
    expect_text(GSP_BMB_OBJ_KEY_CTRL_STOP_BTN_LBL, "停止打印");
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 0); /* expired first press cannot complete a stop */
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 1 && last_control == BMB_CMD_STOP);
    click(GSP_BMB_ACTION_CTRL_BACK);
    expect_page(GSP_BMB_OBJ_KEY_STATUS_PAGE);
    feed("{\"print\":{\"mc_percent\":42,\"gcode_state\":\"PAUSE\"}}");
    advance(100);
    expect_text(GSP_BMB_OBJ_KEY_PCT_LBL, "42%");
    expect_text(GSP_BMB_OBJ_KEY_QK_PAUSE_BTN_LBL, "继续");
    click(GSP_BMB_ACTION_QK_PAUSE);
    CHECK(controls == 2 && last_control == BMB_CMD_RESUME);
    CHECK(engine.rejected_setters == 0 && ui_state.last_error == ESP_GSP_OK);
    stop();
    pass();
}

static void test_stop_task_change(void)
{
    begin_case("STOP confirmation cannot cross tasks before the next render tick");
    start();
    advance(100);
    feed("{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"job-A\"}}");
    click(GSP_BMB_ACTION_CTRL_OPEN);
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 0);
    expect_text(GSP_BMB_OBJ_KEY_CTRL_STOP_BTN_LBL, "再按一次确认停止");
    feed("{\"print\":{\"subtask_name\":\"job-B\"}}");
    /* Input wins the race with the next tick: old first tap cannot stop B. */
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 0);
    expect_text(GSP_BMB_OBJ_KEY_CTRL_STOP_BTN_LBL, "再按一次确认停止");
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 1 && last_control == BMB_CMD_STOP);
    CHECK(!strcmp(last_control_task, "job-B"));
    expect_text(GSP_BMB_OBJ_KEY_CTRL_STOP_BTN_LBL, "停止打印");
    stop();
    pass();
}

static void test_stop_offline(void)
{
    begin_case("offline STOP taps cannot arm a future online print");
    start();
    advance(100);
    click(GSP_BMB_ACTION_CTRL_OPEN);
    click(GSP_BMB_ACTION_CTRL_STOP);
    expect_text(GSP_BMB_OBJ_KEY_CTRL_STOP_BTN_LBL, "停止打印");
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 0);
    feed("{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"online-job\"}}");
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 0);
    expect_text(GSP_BMB_OBJ_KEY_CTRL_STOP_BTN_LBL, "再按一次确认停止");
    click(GSP_BMB_ACTION_CTRL_STOP);
    CHECK(controls == 1 && last_control == BMB_CMD_STOP);
    CHECK(!strcmp(last_control_task, "online-job"));
    stop();
    pass();
}

static void test_navigation_retry(void)
{
    begin_case("failed navigation visibility retries without another tap");
    start();
    advance(100);
    engine.fail_setter = true;
    engine.fail_kind = SET_VISIBLE;
    engine.fail_key = GSP_BMB_OBJ_KEY_CTRL_PAGE;
    click(GSP_BMB_ACTION_CTRL_OPEN);
    CHECK(!component(GSP_BMB_OBJ_KEY_CTRL_PAGE)->visible);
    advance(100);
    CHECK(!component(GSP_BMB_OBJ_KEY_CTRL_PAGE)->visible);
    CHECK(engine.rejected_setters >= 2);
    engine.fail_setter = false;
    advance(100);
    expect_page(GSP_BMB_OBJ_KEY_CTRL_PAGE);
    CHECK(engine.live_timers == 1 && engine.timer_created == 1);
    stop();
    pass();
}

static void test_retained_callback_after_deinit(bool timer_failure)
{
    begin_case(timer_failure ?
        "failed callback unregister and timer deletion leave deinitialized callbacks inert" :
        "failed callback unregister leaves deinitialized input inert");
    start();
    advance(100);
    void *timer = ui_state.timer;
    engine.fail_unregister = true;
    engine.fail_timer_delete = timer_failure;
    bmb_ui_deinit(&engine, &ui_state);
    CHECK(engine.callback != NULL && engine.live_timers == (unsigned)timer_failure);
    const unsigned ticks = ui_state.tick_count;
    const unsigned replay_ticks = host_ticks;
    reject_all_input();
    advance(1000); /* also dispatch the timer left registered after delete failed */
    CHECK(ui_state.tick_count == ticks && host_ticks == replay_ticks);
    engine.fail_unregister = engine.fail_timer_delete = false;
    /* Rejected deletion must retain ownership so a subsequent deinit can
     * release the timer. The surviving callback remains inert until then. */
    if (timer_failure) CHECK(ui_state.timer == timer);
    stop();
    pass();
}

static void raise_alert(bool error, bool fresh)
{
    feed(fresh ? "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"fresh-job\"}}" :
                 "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"old-job\"}}");
    if (error) {
        feed(fresh ? "{\"print\":{\"gcode_state\":\"FAILED\",\"print_error\":2147483648}}" :
                     "{\"print\":{\"gcode_state\":\"FAILED\",\"print_error\":50364433}}");
    } else {
        feed("{\"print\":{\"gcode_state\":\"FINISH\",\"print_error\":0}}");
    }
    CHECK(error ? model.error_seen : model.finish_seen);
    CHECK((error ? model.error_generation : model.finish_generation) == 1);
}

static void expect_alert(bool error, bool fresh)
{
    expect_page(GSP_BMB_OBJ_KEY_FINISH_PAGE);
    expect_text(GSP_BMB_OBJ_KEY_FINISH_TASK, fresh ? "fresh-job" : "old-job");
    expect_text(GSP_BMB_OBJ_KEY_FINISH_TITLE, error ? "打印出错" : "打印完成");
    expect_text(GSP_BMB_OBJ_KEY_FINISH_SUB, error ?
        (fresh ? "错误码 0x80000000" : "错误码 0x03008011") : "可以取件了");
}

static void test_session_collision(bool old_error, bool fresh_error)
{
    static const char *const names[2][2] = {
        {"session reset: old finish cannot dismiss fresh finish generation 1",
         "session reset: old finish cannot dismiss fresh error generation 1"},
        {"session reset: old error cannot dismiss fresh finish generation 1",
         "session reset: old error cannot dismiss fresh error generation 1"},
    };
    begin_case(names[old_error][fresh_error]);
    start();
    advance(100);
    raise_alert(old_error, false);
    advance(100);
    expect_alert(old_error, false);
    CHECK(buzzes == 1 && ui_state.alert_generation == 1);
    const uint32_t old_epoch = model.session_epoch;
    bmb_state_lock();
    bmb_state_reset_session(&model);  /* real reset restarts generation namespace */
    bmb_state_unlock();
    CHECK(model.session_epoch != old_epoch);
    CHECK(!model.error_seen && !model.finish_seen);
    CHECK(model.error_generation == 0 && model.finish_generation == 0);
    raise_alert(fresh_error, true);
    CHECK(ui_state.alert_epoch == old_epoch); /* deliberately no intervening tick */
    expect_alert(old_error, false);
    click(GSP_BMB_ACTION_FINISH_OK);
    CHECK(fresh_error ? model.error_seen : model.finish_seen);
    CHECK((fresh_error ? model.error_generation : model.finish_generation) == 1);
    CHECK(buzzes == 1);
    advance(100);
    expect_alert(fresh_error, true);
    CHECK(ui_state.alert_epoch == model.session_epoch);
    CHECK(ui_state.alert_generation == 1 && buzzes == 2);
    advance(100);
    expect_alert(fresh_error, true);
    CHECK(buzzes == 2); /* same pending alert is not replayed each frame */
    click(GSP_BMB_ACTION_FINISH_OK);
    CHECK(!model.error_seen && !model.finish_seen);
    expect_page(GSP_BMB_OBJ_KEY_STATUS_PAGE);
    advance(100);
    expect_page(GSP_BMB_OBJ_KEY_STATUS_PAGE);
    CHECK(buzzes == 2);
    stop();
    pass();
}

typedef enum { TRIGGER_STOP, TRIGGER_ALERT, TRIGGER_PAD,
               TRIGGER_PAD_ERROR, TRIGGER_SET } runtime_trigger_t;

static void test_one_shot_retry(const char *name, gsp_component_key_t key,
                                runtime_trigger_t trigger, const char *expected)
{
    begin_case(name);
    start();
    advance(100);
    snprintf(component(key)->text, TEXT_CAPACITY, "last committed text");
    engine.fail_setter = true;
    engine.fail_kind = SET_TEXT;
    engine.fail_key = key;
    switch (trigger) {
    case TRIGGER_STOP:
        feed("{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"job\"}}");
        click(GSP_BMB_ACTION_CTRL_OPEN);
        click(GSP_BMB_ACTION_CTRL_STOP);
        CHECK(controls == 0);
        break;
    case TRIGGER_ALERT:
        raise_alert(true, true);
        advance(100);
        CHECK(buzzes == 1);
        break;
    case TRIGGER_PAD:
    case TRIGGER_PAD_ERROR:
        click(GSP_BMB_ACTION_SET_OPEN);
        click(GSP_BMB_ACTION_SET_IP);
        if (trigger == TRIGGER_PAD_ERROR) {
            click(GSP_BMB_ACTION_PAD_CLR);
            click(GSP_BMB_ACTION_PAD_OK);
        } else if (key == GSP_BMB_OBJ_KEY_PAD_FIELD) {
            /* The retry must use the latest desired input, not the first failure. */
            click(GSP_BMB_ACTION_PAD_CLR);
            click(GSP_BMB_ACTION_PAD_1);
        }
        break;
    case TRIGGER_SET:
        click(GSP_BMB_ACTION_SET_OPEN);
        break;
    }
    expect_text(key, "last committed text");
    CHECK(engine.rejected_setters > 0);
    const unsigned failures = engine.rejected_setters;
    advance(100);
    CHECK(engine.rejected_setters > failures);
    expect_text(key, "last committed text");
    engine.fail_setter = false;
    advance(100);
    expect_text(key, expected);
    const unsigned setters = engine.setters;
    advance(100);
    expect_text(key, expected);
    CHECK(engine.setters > setters); /* ordinary refresh still advances */
    CHECK(engine.live_timers == 1 && engine.timer_created == 1);
    if (trigger == TRIGGER_ALERT) CHECK(buzzes == 1);
    stop();
    pass();
}

int main(void)
{
    test_callback_failure();
    test_timer_failure(false);
    test_timer_failure(true);
    test_initial_setter_failure("initial page setter failure retries on render ticks",
                               SET_VISIBLE, GSP_BMB_OBJ_KEY_STATUS_PAGE);
    test_initial_setter_failure("initial IP setter failure retries on render ticks",
                               SET_TEXT, GSP_BMB_OBJ_KEY_SET_IP_LBL);
    test_initial_setter_failure("initial access-code setter failure retries on render ticks",
                               SET_TEXT, GSP_BMB_OBJ_KEY_SET_CODE_LBL);
    test_initial_setter_failure("initial telemetry text failure retries on render ticks",
                               SET_TEXT, GSP_BMB_OBJ_KEY_LINK_LBL);
    test_initial_setter_failure("initial progress setter failure retries on render ticks",
                               SET_VALUE, GSP_BMB_OBJ_KEY_PROG);
    test_initial_setter_failure("initial AMS color setter failure retries on render ticks",
                               SET_COLOR, GSP_BMB_OBJ_KEY_AMS_SWATCH_0);
    test_normal_navigation();
    test_stop_task_change();
    test_stop_offline();
    test_navigation_retry();
    test_retained_callback_after_deinit(false);
    test_retained_callback_after_deinit(true);
    test_one_shot_retry("STOP confirmation label retries after setter failure",
        GSP_BMB_OBJ_KEY_CTRL_STOP_BTN_LBL, TRIGGER_STOP, "再按一次确认停止");
    test_one_shot_retry("alert task retries after setter failure",
        GSP_BMB_OBJ_KEY_FINISH_TASK, TRIGGER_ALERT, "fresh-job");
    test_one_shot_retry("alert title retries after setter failure",
        GSP_BMB_OBJ_KEY_FINISH_TITLE, TRIGGER_ALERT, "打印出错");
    test_one_shot_retry("alert error code retries after setter failure",
        GSP_BMB_OBJ_KEY_FINISH_SUB, TRIGGER_ALERT, "错误码 0x80000000");
    test_one_shot_retry("keypad field retries latest input after setter failure",
        GSP_BMB_OBJ_KEY_PAD_FIELD, TRIGGER_PAD, "1");
    test_one_shot_retry("keypad validation error retries after setter failure",
        GSP_BMB_OBJ_KEY_PAD_ERROR, TRIGGER_PAD_ERROR, "IP 无效");
    test_one_shot_retry("keypad title retries after setter failure",
        GSP_BMB_OBJ_KEY_PAD_TITLE, TRIGGER_PAD, "输入打印机 IP");
    test_one_shot_retry("settings IP retries after setter failure",
        GSP_BMB_OBJ_KEY_SET_IP_LBL, TRIGGER_SET, "IP  192.0.2.8");
    test_one_shot_retry("settings access code retries after setter failure",
        GSP_BMB_OBJ_KEY_SET_CODE_LBL, TRIGGER_SET, "访问码  test1234");
    test_one_shot_retry("settings status retries after setter failure",
        GSP_BMB_OBJ_KEY_SET_STATUS_LBL, TRIGGER_SET, "未保存");
    for (unsigned old_error = 0; old_error < 2; ++old_error) {
        for (unsigned fresh_error = 0; fresh_error < 2; ++fresh_error) {
            test_session_collision(old_error != 0, fresh_error != 0);
        }
    }
    printf("UI startup regressions: PASS (%u cases, %u checks)\n", groups, checks);
    return EXIT_SUCCESS;
}
