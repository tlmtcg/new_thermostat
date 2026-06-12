#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* IMPORTANT : ne pas utiliser wifi_storage_t (réservé par ESP-IDF) */
typedef struct {
    char sta_ssid[32];
    char sta_pass[64];

    char ap_ssid[32];
    char ap_pass[64];

    uint8_t ap_channel;
    uint8_t retry_count;
    uint32_t retry_interval_ms;
    uint8_t auth_mode;
} wifi_config_storage_t;

/* Instance globale */
extern wifi_config_storage_t g_wifi_cfg;

/* Fonctions */
bool wifi_storage_load_all(wifi_config_storage_t *cfg);
bool wifi_storage_save_all(const wifi_config_storage_t *cfg);
