// SPDX-License-Identifier: Apache-2.0

#include "bmb_config.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "nvs_flash.h"

#define BMB_CFG_NAMESPACE "bambu"
#define BMB_CFG_IP_KEY "ip"
#define BMB_CFG_CODE_KEY "code"

/* Build-time pairing defaults for this home build. The settings page can
 * still change them at runtime; NVS wins once the user saves. */
#define BMB_DEFAULT_IP BMB_DEFAULT_IP_STR
#define BMB_DEFAULT_CODE BMB_DEFAULT_CODE_STR

/* The Vibe Mode network module stores its credentials here; keep the literal
 * in sync with esp-mosaico-vibe-mode's factory_network.c. */
#define BMB_WIFI_PARTITION "sysmeta"
#define BMB_WIFI_NAMESPACE "wifi"

bool bmb_pairing_load(bmb_pairing_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    nvs_handle_t handle;
    bool ok = false;
    if (nvs_open(BMB_CFG_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(cfg->ip);
        ok = nvs_get_str(handle, BMB_CFG_IP_KEY, cfg->ip, &len) == ESP_OK;
        if (ok) {
            len = sizeof(cfg->code);
            ok = nvs_get_str(handle, BMB_CFG_CODE_KEY, cfg->code, &len) == ESP_OK;
        }
        nvs_close(handle);
    }
    if (!ok) {
        /* Fall back to the build-time defaults so a fresh flash connects
         * without any on-screen pairing. */
        strlcpy(cfg->ip, BMB_DEFAULT_IP, sizeof(cfg->ip));
        strlcpy(cfg->code, BMB_DEFAULT_CODE, sizeof(cfg->code));
    }
    cfg->valid = cfg->ip[0] != '\0' && cfg->code[0] != '\0';
    return cfg->valid;
}

bool bmb_pairing_save(const bmb_pairing_t *cfg)
{
    if (!cfg->valid) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(BMB_CFG_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(handle, BMB_CFG_IP_KEY, cfg->ip) == ESP_OK &&
              nvs_set_str(handle, BMB_CFG_CODE_KEY, cfg->code) == ESP_OK;
    if (ok) {
        ok = nvs_commit(handle) == ESP_OK;
    }
    nvs_close(handle);
    return ok;
}

bool bmb_wifi_creds(char *ssid, size_t ssid_cap, char *password, size_t pass_cap)
{
    nvs_handle_t handle;
    if (nvs_flash_init_partition(BMB_WIFI_PARTITION) != ESP_OK) {
        return false;
    }
    if (nvs_open_from_partition(BMB_WIFI_PARTITION, BMB_WIFI_NAMESPACE,
                                NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = ssid_cap;
    bool ok = nvs_get_str(handle, "ssid", ssid, &len) == ESP_OK;
    if (ok) {
        len = pass_cap;
        ok = nvs_get_str(handle, "password", password, &len) == ESP_OK;
    }
    nvs_close(handle);
    return ok;
}
#else
/* Host replay build: deterministic demo pairing, in-memory only. */
bool bmb_pairing_load(bmb_pairing_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->ip, sizeof(cfg->ip), "%s", "192.168.31.100");
    snprintf(cfg->code, sizeof(cfg->code), "%s", "18charcode");
    cfg->valid = true;
    return true;
}

bool bmb_pairing_save(const bmb_pairing_t *cfg)
{
    (void)cfg;
    return true;
}

bool bmb_wifi_creds(char *ssid, size_t ssid_cap, char *password, size_t pass_cap)
{
    snprintf(ssid, ssid_cap, "%s", "host-sim");
    snprintf(password, pass_cap, "%s", "");
    return true;
}
#endif
