// SPDX-License-Identifier: Apache-2.0
// Application-lifetime transport singleton. The lifecycle task owns client and
// Wi-Fi setup/teardown; MQTT callbacks own RX parsing and command I/O.
// The model lock guards handoff, policy, diagnostics and the small command outbox.
// SDK calls that may block always run outside that lock.
#include "bmb_link.h"
#include "bmb_ctrl.h"

#ifdef ESP_PLATFORM
#include "bmb_command.h"
#include "bmb_config.h"
#include "bmb_report.h"
#include "bmb_rx.h"
#include "bmb_transport_policy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "mqtt_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CTRL_PAYLOAD_MAX 192
#define RX_PAYLOAD_MAX (64U * 1024U)
#define LIFECYCLE_QUEUE_SLOTS 4
#define LIFECYCLE_STACK_BYTES 8192
#define LIFECYCLE_TICK_MS 100
#define MQTT_IO_TIMEOUT_MS 5000
/* Ticks to let a freshly started SDK MQTT task reach run=true before destroy. */
#define MQTT_START_SETTLE_TICKS 50

typedef enum { LINK_RESTART, LINK_STOP } link_action_t;
typedef struct { link_action_t action; uint32_t epoch; } link_request_t;
typedef struct {
    uint32_t token;
    unsigned parts, cursor;
    char topic[40];
    char payload[BMB_COMMAND_PARTS][CTRL_PAYLOAD_MAX];
} tx_batch_t;

typedef struct {
    bmb_state_t *model;
    QueueHandle_t requests;
    TaskHandle_t task;
    esp_mqtt_client_handle_t client;
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_handler, ip_handler;
    bool wifi_inited, wifi_started, wifi_ready, active, sntp_inited;
    uint32_t epoch, sequence;
    tx_batch_t outbox;
    /* MQTT-task scratch; the UI/lifecycle tasks never publish or touch it. */
    char tx_payload[CTRL_PAYLOAD_MAX];
    char tx_topic[40];
    char *rx_storage;
    bmb_rx_t rx;
    bmb_transport_policy_t policy;
    /* All counters are guarded by the model lock, including log snapshots. */
    uint32_t frames, connects, disconnects, stats_ms;
} bmb_transport_t;

static const char *TAG = "bmb_link";
static bmb_transport_t s_link;

uint32_t bmb_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void wake_lifecycle(void)
{
    if (s_link.task) xTaskNotifyGive(s_link.task);
}

static void set_failure(const char *message)
{
    bmb_state_lock();
    if (s_link.model) {
        s_link.model->link = BMB_LINK_OFFLINE;
        bmb_command_cancel_locked(s_link.model, message);
        snprintf(s_link.model->ctrl_status, sizeof(s_link.model->ctrl_status),
                 "%s", message);
    }
    bmb_state_unlock();
}

static void clock_synced(struct timeval *tv)
{
    ESP_LOGI(TAG, "clock synchronized: %lld", (long long)tv->tv_sec);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    bool connect = false, sync = false;
    bmb_state_lock();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        connect = s_link.active;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        s_link.wifi_ready = false;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_link.wifi_ready = false;
        if (s_link.active) {
            s_link.model->link = BMB_LINK_OFFLINE;
            bmb_command_cancel_locked(s_link.model, "连接已断开");
            s_link.outbox.parts = 0;
            s_link.policy.connected = false;
            bmb_transport_policy_failed(&s_link.policy, bmb_now_ms());
            connect = true;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && s_link.active) {
        s_link.wifi_ready = true;
        sync = s_link.sntp_inited;
    }
    bmb_state_unlock();
    if (connect) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "wifi connect: %s", esp_err_to_name(err));
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = data;
        ESP_LOGI(TAG, "wifi ready " IPSTR, IP2STR(&evt->ip_info.ip));
    }
    if (sync) {
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) ESP_LOGW(TAG, "clock start: %s", esp_err_to_name(err));
    }
    wake_lifecycle();
}

/* Called only by the 8 KiB lifecycle task. wifi_config_t and SDK setup frames
 * fit this budget; network TLS runs on MQTT's own task. */
static esp_err_t wifi_start(void)
{
    if (!s_link.wifi_inited) {
        char ssid[33], password[65];
        if (!bmb_wifi_creds(ssid, sizeof(ssid), password, sizeof(password))) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        err = esp_netif_init();
        if (err != ESP_OK) return err;
        /* The application owns the default STA service for its lifetime. */
        s_link.netif = esp_netif_create_default_wifi_sta();
        if (!s_link.netif) return ESP_ERR_NO_MEM;
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init);
        if (err != ESP_OK) goto fail_netif;
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    wifi_event, NULL, &s_link.wifi_handler);
        if (err != ESP_OK) goto fail_driver;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    wifi_event, NULL, &s_link.ip_handler);
        if (err != ESP_OK) goto fail_wifi_handler;
        wifi_config_t config = {0};
        const size_t ssid_len = strlen(ssid), password_len = strlen(password);
        if (ssid_len > sizeof(config.sta.ssid) ||
            password_len > sizeof(config.sta.password)) {
            err = ESP_ERR_INVALID_SIZE;
            goto fail_handlers;
        }
        /* IDF accepts full-width SSID/PSK fields without a trailing NUL. */
        memcpy(config.sta.ssid, ssid, ssid_len);
        memcpy(config.sta.password, password, password_len);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) goto fail_handlers;
        s_link.wifi_inited = true;
        if (setenv("TZ", "CST-8", 1) == 0) tzset();
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        sntp.start = false;
        sntp.sync_cb = clock_synced;
        err = esp_netif_sntp_init(&sntp);
        bmb_state_lock();
        s_link.sntp_inited = err == ESP_OK;
        bmb_state_unlock();
        if (err != ESP_OK) ESP_LOGW(TAG, "clock init: %s", esp_err_to_name(err));
        goto start_radio;
fail_handlers:
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_link.ip_handler);
fail_wifi_handler:
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_link.wifi_handler);
fail_driver:
        esp_wifi_deinit();
fail_netif:
        esp_netif_destroy_default_wifi(s_link.netif);
        s_link.netif = NULL;
        return err;
    }
start_radio:
    if (!s_link.wifi_started) {
        bmb_state_lock();
        s_link.wifi_ready = false;
        bmb_state_unlock();
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_link.wifi_started = true;
    } else {
        esp_netif_ip_info_t info;
        if (esp_netif_is_netif_up(s_link.netif) &&
            esp_netif_get_ip_info(s_link.netif, &info) == ESP_OK && info.ip.addr) {
            bmb_state_lock();
            s_link.wifi_ready = true;
            bmb_state_unlock();
        } else {
            /* DISCONNECTED events normally reconnect; this also covers a
             * manual restart issued while the station is already down. */
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "wifi reconnect: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

static void ctrl_drain(esp_mqtt_client_handle_t client)
{
    for (;;) {
        unsigned part;
        uint32_t token;
        bmb_state_lock();
        tx_batch_t *batch = &s_link.outbox;
        if (!s_link.active || !s_link.wifi_ready || s_link.client != client ||
            !bmb_transport_policy_can_send(&s_link.policy) ||
            batch->cursor >= batch->parts) {
            bmb_state_unlock();
            return;
        }
        part = batch->cursor++;
        token = batch->token;
        if (!bmb_command_can_send_locked(s_link.model, token, part, bmb_now_ms())) {
            batch->parts = 0;
            bmb_state_unlock();
            return;
        }
        memcpy(s_link.tx_payload, batch->payload[part], sizeof(s_link.tx_payload));
        memcpy(s_link.tx_topic, batch->topic, sizeof(s_link.tx_topic));
        bmb_state_unlock();
        /* Only a complete parsed DATA callback calls this function. MQTT 1.1.0
         * already holds its recursive API mutex here, so no newer report can
         * cancel a token while publish waits for that mutex. QoS 0 writes are
         * immediate and never retained for delivery after reconnect. */
        int sent = esp_mqtt_client_publish(client, s_link.tx_topic,
                                           s_link.tx_payload, 0, 0, 0);
        bmb_state_lock();
        bmb_command_sent_locked(s_link.model, token, part, sent >= 0);
        bmb_state_unlock();
        if (sent < 0) {
            ESP_LOGW(TAG, "command publish failed token=%lu", (unsigned long)token);
            return;
        }
        ESP_LOGI(TAG, "command sent token=%lu part=%u", (unsigned long)token, part);
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = data;
    bmb_state_lock();
    bool current = s_link.active && s_link.client == event->client;
    bmb_state_unlock();
    if (!current) return;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        /* Connectivity is not telemetry freshness. Subscribe once per client;
         * a rejected/missing SUBACK leads to a bounded clean session retry. */
        bmb_state_lock();
        bmb_transport_policy_connected(&s_link.policy, bmb_now_ms());
        uint32_t connects = ++s_link.connects;
        bmb_state_unlock();
        ESP_LOGI(TAG, "mqtt connected (#%lu)", (unsigned long)connects);
        if (esp_mqtt_client_subscribe(event->client, "device/+/report", 0) < 0) {
            bmb_state_lock();
            bmb_transport_policy_failed(&s_link.policy, bmb_now_ms());
            bmb_state_unlock();
            ESP_LOGW(TAG, "report subscription failed");
        }
        wake_lifecycle();
        break;
    }
    case MQTT_EVENT_SUBSCRIBED: {
        /* MQTT 1.1.0 deliver_suback reports broker refusal on this event, not
         * MQTT_EVENT_ERROR. A successful subscribe write alone proves nothing. */
        bool accepted = event->data && event->data_len == 1 &&
                        (unsigned char)event->data[0] <= 2U &&
                        (!event->error_handle ||
                         event->error_handle->error_type == MQTT_ERROR_TYPE_NONE);
        bmb_state_lock();
        bmb_transport_policy_subscribed(&s_link.policy, accepted, bmb_now_ms());
        bmb_state_unlock();
        if (!accepted) ESP_LOGW(TAG, "report subscription rejected");
        wake_lifecycle();
        break;
    }
    case MQTT_EVENT_DISCONNECTED: {
        bmb_state_lock();
        uint32_t disconnects = ++s_link.disconnects, frames = s_link.frames;
        if (s_link.active && s_link.client == event->client) {
            s_link.policy.connected = false;
            bmb_transport_policy_failed(&s_link.policy, bmb_now_ms());
            s_link.policy.rx_busy = false;
            s_link.model->link = BMB_LINK_CONNECTING;
            bmb_command_cancel_locked(s_link.model, "连接已断开");
            s_link.outbox.parts = 0;
        }
        bmb_state_unlock();
        ESP_LOGW(TAG, "mqtt disconnected (#%lu) after %lu frames",
                 (unsigned long)disconnects, (unsigned long)frames);
        bmb_rx_reset(&s_link.rx);
        wake_lifecycle();
        break;
    }
    case MQTT_EVENT_DATA: {
        bmb_state_lock();
        s_link.policy.rx_busy = true;
        bmb_state_unlock();
        if (event->data_len < 0 || event->topic_len < 0 ||
            event->total_data_len < 0 || event->current_data_offset < 0) {
            bmb_rx_reset(&s_link.rx);
            bmb_state_lock();
            s_link.policy.rx_busy = false;
            bmb_state_unlock();
            wake_lifecycle();
            break;
        }
        bmb_rx_result_t result = bmb_rx_feed(&s_link.rx,
            event->topic, (size_t)event->topic_len, event->data,
            (size_t)event->data_len, (size_t)event->total_data_len,
            (size_t)event->current_data_offset);
        uint32_t now = bmb_now_ms();
        bool applied = false;
        if (result == BMB_RX_COMPLETE) {
            applied = bmb_report_apply(s_link.model, s_link.rx.topic, s_link.rx.topic_len,
                                       s_link.rx.data, s_link.rx.received, now);
        }
        bmb_state_lock();
        if (s_link.active && s_link.client == event->client) {
            if (result == BMB_RX_COMPLETE) ++s_link.frames;
            /* ACK-only traffic does not move last_report_ms. A report applied
             * in this millisecond establishes liveness; a same-ms ACK can only
             * repeat an already valid timestamp, never extend its freshness. */
            if (applied && s_link.model->state_received &&
                s_link.model->last_report_ms == now) {
                bmb_transport_policy_report(&s_link.policy, now);
            }
            s_link.policy.rx_busy = (size_t)event->current_data_offset +
                (size_t)event->data_len < (size_t)event->total_data_len;
        }
        bmb_state_unlock();
        /* Parse/cancel first, then validate/send on the SAME MQTT task while
         * it owns the SDK API mutex. Wait for the next complete accepted report
         * (~1.3 s on the printer); no DATA means the command expires after 8 s.
         * Neither malformed/partial RX nor a self-queued custom event may send. */
        if (result == BMB_RX_COMPLETE && applied) ctrl_drain(event->client);
        wake_lifecycle();
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error type=%d tls=%s sock_errno=%d",
                 event->error_handle ? (int)event->error_handle->error_type : -1,
                 event->error_handle ? esp_err_to_name(event->error_handle->esp_tls_last_esp_err) : "n/a",
                 event->error_handle ? event->error_handle->esp_transport_sock_errno : -1);
        break;
    default:
        break;
    }
}

static esp_err_t mqtt_start(void)
{
    bmb_pairing_t pairing;
    if (!bmb_pairing_load(&pairing) || !pairing.valid) return ESP_ERR_INVALID_STATE;
    s_link.rx_storage = heap_caps_malloc(RX_PAYLOAD_MAX + 1U,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_link.rx_storage) return ESP_ERR_NO_MEM;
    bmb_rx_init(&s_link.rx, s_link.rx_storage, RX_PAYLOAD_MAX + 1U);
    char uri[48];
    int len = snprintf(uri, sizeof(uri), "mqtts://%s:8883", pairing.ip);
    if (len < 0 || (size_t)len >= sizeof(uri)) {
        free(s_link.rx_storage); s_link.rx_storage = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = uri,
        .credentials.username = "bblp",
        .credentials.authentication.password = pairing.code,
        /* LAN traffic is encrypted but peer verification is currently skipped
         * for the printer's self-signed certificate. */
        /* esp-mqtt aborts a connection whenever a PINGREQ goes unanswered for
         * `keepalive`. The printer already streams a report every ~1.3 s, so
         * MQTT pings add no liveness information the app cannot measure itself;
         * an unanswered ping used to tear down a healthy session. Liveness is
         * handled by the telemetry watchdog and an explicit recovery restart. */
        .session.disable_keepalive = true,
        /* One retry owner: the lifecycle policy covers connect/SUBACK/telemetry
         * deadlines. SDK auto-reconnect would bypass its bounded backoff. */
        .network.disable_auto_reconnect = true,
        .network.timeout_ms = MQTT_IO_TIMEOUT_MS,
        .buffer.size = 16384,
        .buffer.out_size = 1024,
        .task.stack_size = 8192,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!client) goto fail_storage;
    err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, NULL);
    if (err != ESP_OK) goto fail_client;
    bmb_state_lock();
    if (!s_link.active) {
        bmb_state_unlock();
        err = ESP_ERR_INVALID_STATE;
        goto fail_client;
    }
    s_link.client = client;
    s_link.model->link_resetting = false;
    bmb_state_unlock();
    err = esp_mqtt_client_start(client);
    if (err == ESP_OK) return ESP_OK;
    bmb_state_lock();
    s_link.client = NULL;
    bmb_state_unlock();
fail_client:
    esp_mqtt_client_destroy(client);
fail_storage:
    free(s_link.rx_storage); s_link.rx_storage = NULL;
    return err;
}

static void mqtt_stop(void)
{
    bmb_state_lock();
    esp_mqtt_client_handle_t client = s_link.client;
    /* Only this task can send or destroy a client. Detach before joining MQTT
     * so late callbacks cannot admit data into the following session. */
    s_link.client = NULL;
    s_link.model->link_resetting = true;
    s_link.outbox.parts = 0;
    bmb_command_cancel_locked(s_link.model, "已取消");
    bmb_state_unlock();
    bool joined = true;
    if (client) {
        esp_err_t err = esp_mqtt_client_stop(client);
        /* esp_mqtt_client_start() creates the SDK task but returns before that
         * task executes its first statement (client->run = true). In that window
         * stop() sees run==false, skips the join, and destroy() would free the
         * client just as the task starts to use it. stop() returns ESP_OK only
         * after it has set run=false and joined the task, so retry while the
         * task has not started yet; never destroy an unjoined client. */
        for (unsigned i = 0; err != ESP_OK && i < MQTT_START_SETTLE_TICKS; ++i) {
            vTaskDelay(1);
            err = esp_mqtt_client_stop(client);
        }
        joined = err == ESP_OK;
        if (joined) {
            err = esp_mqtt_client_destroy(client);
            if (err != ESP_OK) ESP_LOGW(TAG, "mqtt destroy: %s", esp_err_to_name(err));
        } else {
            /* Task never reached run=true within the settle window: do not free
             * the client or its RX storage, since the SDK task may still start
             * and dereference both. Leaking this session is safer than a UAF and
             * is only reachable if the scheduler never ran a ready priority-5
             * task. s_link no longer references it, so no callback can use it
             * through our state, and the next start allocates fresh storage. */
            ESP_LOGE(TAG, "mqtt stop: %s (task not joined; session retained)",
                     esp_err_to_name(err));
        }
    }
    if (joined) {
        free(s_link.rx_storage); s_link.rx_storage = NULL;
        memset(&s_link.rx, 0, sizeof(s_link.rx));
    }
    /* A callback admitted just before cancellation may finish during stop.
     * Reassert the final state after its task has joined, before any restart. */
    bmb_state_lock();
    s_link.model->link = BMB_LINK_OFFLINE;
    s_link.policy.rx_busy = false;
    bmb_state_unlock();
}

static void start_failed(const char *service, esp_err_t err)
{
    ESP_LOGE(TAG, "%s start: %s", service, esp_err_to_name(err));
    bmb_state_lock();
    if (s_link.active) {
        bmb_transport_policy_failed(&s_link.policy, bmb_now_ms());
        s_link.model->link = BMB_LINK_OFFLINE;
        bmb_command_cancel_locked(s_link.model, "连接失败");
        s_link.outbox.parts = 0;
        snprintf(s_link.model->ctrl_status, sizeof(s_link.model->ctrl_status), "连接失败");
    }
    bmb_state_unlock();
}

static void lifecycle_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* UI/lifecycle requests notify this task for cancellation and recovery,
         * never publishing. MQTT's single-slot queue carries only SDK events:
         * a custom event between DATA fragments can block its producer forever.
         * Periodic ticks handle deadlines if notifications coalesce. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LIFECYCLE_TICK_MS));
        link_request_t request;
        if (xQueueReceive(s_link.requests, &request, 0) == pdTRUE) {
            link_request_t next;
            while (xQueueReceive(s_link.requests, &next, 0) == pdTRUE) request = next;
            mqtt_stop();
            bmb_state_lock();
            bool current = request.epoch == s_link.epoch;
            if (current) bmb_transport_policy_reset(&s_link.policy);
            if (current && request.action == LINK_RESTART) {
                uint32_t token = s_link.model->command.token;
                bool cancelled = strcmp(s_link.model->ctrl_status, "已取消") == 0;
                bmb_state_reset_session(s_link.model);
                s_link.model->command.token = token + 1U;
                if (cancelled) {
                    snprintf(s_link.model->ctrl_status, sizeof(s_link.model->ctrl_status),
                             "已取消");
                }
                s_link.model->link = BMB_LINK_CONNECTING;
                s_link.model->link_resetting = true;
                s_link.active = true;
            }
            bmb_state_unlock();
            if (!current) continue;
            if (request.action == LINK_STOP) {
                if (s_link.wifi_started) {
                    esp_err_t err = esp_wifi_stop();
                    if (err == ESP_OK) s_link.wifi_started = false;
                    else ESP_LOGW(TAG, "wifi stop: %s", esp_err_to_name(err));
                }
                continue;
            }
            esp_err_t err = wifi_start();
            if (err != ESP_OK) start_failed("wifi", err);
        }
        bmb_state_lock();
        const uint32_t now = bmb_now_ms();
        const uint32_t epoch = s_link.epoch;
        bmb_command_tick_locked(s_link.model, now);
        /* Initial Wi-Fi setup failures use the same bounded schedule. Once the
         * radio is started, automatic recovery never stops/reinitializes it. */
        bmb_transport_action_t action = bmb_transport_policy_step(&s_link.policy,
            now, s_link.active, s_link.wifi_ready || !s_link.wifi_started,
            s_link.client != NULL);
        if (action == BMB_TRANSPORT_RECREATE) {
            s_link.model->link_resetting = true;
            ++s_link.model->session_epoch;
            s_link.model->finish_seen = false;
            s_link.model->error_seen = false;
            bmb_command_cancel_locked(s_link.model, "连接已断开");
            s_link.outbox.parts = 0;
        }
        bool stats = (uint32_t)(now - s_link.stats_ms) >= 60000U;
        uint32_t frames = s_link.frames, connects = s_link.connects;
        uint32_t disconnects = s_link.disconnects;
        bmb_link_t link = s_link.model->link;
        if (stats) s_link.stats_ms = now;
        bmb_state_unlock();
        if (stats) {
            ESP_LOGI(TAG, "stats: frames=%lu connects=%lu disconnects=%lu link=%d",
                     (unsigned long)frames, (unsigned long)connects,
                     (unsigned long)disconnects, (int)link);
        }
        if (action == BMB_TRANSPORT_RECREATE) {
            ESP_LOGW(TAG, "recovering MQTT session (%lu frames)", (unsigned long)frames);
            mqtt_stop();
            bmb_state_lock();
            bool current = epoch == s_link.epoch && s_link.active;
            if (current) s_link.model->link = BMB_LINK_CONNECTING;
            bmb_state_unlock();
            if (!current) continue;
        }
        if (action != BMB_TRANSPORT_WAIT) {
            if (!s_link.wifi_started) {
                esp_err_t err = wifi_start();
                if (err != ESP_OK) { start_failed("wifi", err); continue; }
            }
            bmb_state_lock();
            bool start = s_link.active && s_link.wifi_ready && !s_link.client;
            if (start) bmb_transport_policy_attempt(&s_link.policy, bmb_now_ms());
            bmb_state_unlock();
            if (start) {
                esp_err_t err = mqtt_start();
                if (err != ESP_OK) start_failed("mqtt", err);
            }
        }
    }
}

static bool request_link(bmb_state_t *model, link_action_t action)
{
    bmb_state_lock();
    if (!model || model != s_link.model || !s_link.requests) {
        bmb_state_unlock(); return false;
    }
    link_request_t request = {action, s_link.epoch + 1U};
    if (xQueueSend(s_link.requests, &request, 0) != pdTRUE) {
        bmb_state_unlock(); return false;
    }
    s_link.epoch = request.epoch;
    s_link.active = false;
    s_link.outbox.parts = 0;
    bmb_command_cancel_locked(model, "已取消");
    model->link_resetting = true;
    model->finish_seen = false;
    model->error_seen = false;
    model->link = action == LINK_RESTART ? BMB_LINK_CONNECTING : BMB_LINK_OFFLINE;
    bmb_state_unlock();
    wake_lifecycle();
    return true;
}

bool bmb_link_restart(bmb_state_t *model)
{
    return request_link(model, LINK_RESTART);
}

void bmb_link_start(bmb_state_t *model)
{
    /* Called once from app_main. The singleton task/queue live until reboot;
     * subsequent UI changes use the nonblocking restart request. */
    if (!model) return;
    if (!s_link.requests) {
        s_link.model = model;
        s_link.sequence = esp_random();
        s_link.requests = xQueueCreate(LIFECYCLE_QUEUE_SLOTS, sizeof(link_request_t));
        if (!s_link.requests) { set_failure("连接失败"); return; }
        if (xTaskCreate(lifecycle_task, "bmb_lifecycle", LIFECYCLE_STACK_BYTES,
                        NULL, 4, &s_link.task) != pdPASS) {
            vQueueDelete(s_link.requests); s_link.requests = NULL;
            set_failure("连接失败"); return;
        }
    }
    if (!bmb_link_restart(model)) set_failure("连接失败");
}

void bmb_link_stop(void)
{
    if (s_link.model) request_link(s_link.model, LINK_STOP);
}

static int make_payload(char *dst, size_t capacity, bmb_command_kind_t kind,
                         unsigned part, int value, uint32_t sequence)
{
    if (kind == BMB_COMMAND_LIGHT) {
        return snprintf(dst, capacity,
            "{\"system\":{\"command\":\"ledctrl\",\"led_node\":\"%s\","
            "\"led_mode\":\"%s\",\"interval_time\":1000,\"led_on_time\":500,"
            "\"led_off_time\":500,\"loop_times\":1,\"sequence_id\":\"%lu\"}}",
            part ? "chamber_light2" : "chamber_light", value ? "on" : "off",
            (unsigned long)sequence);
    }
    if (kind == BMB_COMMAND_SPEED) {
        return snprintf(dst, capacity,
            "{\"print\":{\"command\":\"print_speed\",\"param\":\"%d\","
            "\"sequence_id\":\"%lu\"}}", value, (unsigned long)sequence);
    }
    static const char *const commands[] = {"pause", "resume", "stop"};
    return snprintf(dst, capacity,
        "{\"print\":{\"command\":\"%s\",\"sequence_id\":\"%lu\"}}",
        commands[kind], (unsigned long)sequence);
}

static bool ctrl_submit(bmb_state_t *model, bmb_command_kind_t kind, int value)
{
    /* One bounded batch fits the 12 KiB render stack. The next complete report
     * callback parses first, then revalidates/sends; lifecycle only ticks expiry. */
    tx_batch_t batch = {0};
    uint32_t sequences[BMB_COMMAND_PARTS];
    bmb_state_lock();
    if (model != s_link.model || !model || !s_link.active || !s_link.client ||
        !s_link.policy.connected || s_link.policy.retry_pending) {
        bmb_state_unlock(); return false;
    }
    batch.parts = kind == BMB_COMMAND_LIGHT ? 2U : 1U;
    for (unsigned i = 0; i < batch.parts; ++i) {
        if (++s_link.sequence == 0) ++s_link.sequence;
        sequences[i] = s_link.sequence;
        int n = make_payload(batch.payload[i], sizeof(batch.payload[i]),
                             kind, i, value, sequences[i]);
        if (n < 0 || (size_t)n >= sizeof(batch.payload[i])) {
            bmb_state_unlock(); return false;
        }
    }
    int n = snprintf(batch.topic, sizeof(batch.topic), "device/%s/request", model->dev_id);
    if (n < 0 || (size_t)n >= sizeof(batch.topic) ||
        !bmb_command_begin_locked(model, kind, sequences, batch.parts, bmb_now_ms())) {
        bmb_state_unlock(); return false;
    }
    batch.token = model->command.token;
    s_link.outbox = batch;
    bmb_state_unlock();
    /* No MQTT API, socket I/O or client pointer crosses into the UI task. */
    wake_lifecycle();
    return true;
}

bool bmb_ctrl_send(bmb_state_t *model, bmb_cmd_t cmd)
{
    if (cmd < BMB_CMD_PAUSE || cmd > BMB_CMD_STOP) return false;
    return ctrl_submit(model, (bmb_command_kind_t)cmd, 0);
}

bool bmb_ctrl_set_speed(bmb_state_t *model, int level)
{
    return level >= 1 && level <= BMB_SPEED_LEVELS &&
           ctrl_submit(model, BMB_COMMAND_SPEED, level);
}

bool bmb_ctrl_set_light(bmb_state_t *model, bool on)
{
    return ctrl_submit(model, BMB_COMMAND_LIGHT, on ? 1 : 0);
}
#endif
