// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Printer pairing data plus the Wi-Fi credentials Vibe Mode provisioned. */

typedef struct {
    char ip[16];    /* printer IPv4, e.g. "192.168.31.100" */
    char code[16];  /* LAN access code (bblp password) */
    bool valid;     /* both fields are non-empty */
} bmb_pairing_t;

/* Build-time pairing defaults are intentionally empty in the published
 * project: enter the printer's LAN IP and access code on the Settings page
 * after the first boot. Builders may override via CFLAGS or sdkconfig. */
#ifndef BMB_DEFAULT_IP_STR
#define BMB_DEFAULT_IP_STR ""
#endif
#ifndef BMB_DEFAULT_CODE_STR
#define BMB_DEFAULT_CODE_STR ""
#endif

/* Load the saved pairing. On the device this reads NVS namespace "bambu";
 * the host replay build returns the built-in demo values. */
bool bmb_pairing_load(bmb_pairing_t *cfg);

/* Persist the pairing. Returns false when the platform cannot store it (the
 * caller keeps the in-memory copy either way). */
bool bmb_pairing_save(const bmb_pairing_t *cfg);

/* Wi-Fi credentials provisioned by Vibe Mode (sysmeta partition, namespace
 * "wifi"). Returns false when not provisioned yet. */
bool bmb_wifi_creds(char *ssid, size_t ssid_cap, char *password, size_t pass_cap);
