#include "wifi_storage.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "wifi_storage";

/* ===================== INSTANCE GLOBALE ===================== */

wifi_config_storage_t g_wifi_cfg = {0};

/* ===================== CHARGEMENT NVS ===================== */

bool wifi_storage_load_all(wifi_config_storage_t *cfg)
{
    if (!cfg)
        return false;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Aucune config WiFi en NVS (%s)", esp_err_to_name(err));
        return false;
    }

    size_t len;

    /* --- STA SSID --- */
    len = sizeof(cfg->sta_ssid);
    if (nvs_get_str(nvs, "sta_ssid", cfg->sta_ssid, &len) != ESP_OK)
        cfg->sta_ssid[0] = '\0';

    /* --- STA PASS --- */
    len = sizeof(cfg->sta_pass);
    if (nvs_get_str(nvs, "sta_pass", cfg->sta_pass, &len) != ESP_OK)
        cfg->sta_pass[0] = '\0';

    /* --- AP SSID --- */
    len = sizeof(cfg->ap_ssid);
    if (nvs_get_str(nvs, "ap_ssid", cfg->ap_ssid, &len) != ESP_OK)
        strlcpy(cfg->ap_ssid, "esp32_ap", sizeof(cfg->ap_ssid)); // CORRECTION: strlcpy sécurisé

    /* --- AP PASS --- */
    len = sizeof(cfg->ap_pass);
    if (nvs_get_str(nvs, "ap_pass", cfg->ap_pass, &len) != ESP_OK)
        strlcpy(cfg->ap_pass, "12345678", sizeof(cfg->ap_pass)); // CORRECTION: strlcpy sécurisé

    /* --- AP CHANNEL --- */
    if (nvs_get_u8(nvs, "ap_channel", &cfg->ap_channel) != ESP_OK)
        cfg->ap_channel = 1;

    /* --- RETRY COUNT --- */
    if (nvs_get_u8(nvs, "retry_count", &cfg->retry_count) != ESP_OK)
        cfg->retry_count = 5;

    /* --- RETRY INTERVAL --- */
    if (nvs_get_u32(nvs, "retry_interval", &cfg->retry_interval_ms) != ESP_OK)
        cfg->retry_interval_ms = 3000;

    /* --- AUTH MODE --- */
    if (nvs_get_u8(nvs, "auth_mode", &cfg->auth_mode) != ESP_OK)
        cfg->auth_mode = 3; // WPA2_PSK

    nvs_close(nvs);

    ESP_LOGI(TAG, "Config WiFi chargée depuis NVS");
    ESP_LOGI(TAG, "STA SSID: %s", cfg->sta_ssid);
    ESP_LOGI(TAG, "AP SSID : %s", cfg->ap_ssid);

    return (cfg->sta_ssid[0] != '\0');
}

/* ===================== SAUVEGARDE NVS ===================== */

bool wifi_storage_save_all(const wifi_config_storage_t *cfg)
{
    if (!cfg)
        return false;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Impossible d'ouvrir NVS (%s)", esp_err_to_name(err));
        return false;
    }

    /* --- Écriture des chaînes --- */
    nvs_set_str(nvs, "sta_ssid", cfg->sta_ssid);
    nvs_set_str(nvs, "sta_pass", cfg->sta_pass);
    nvs_set_str(nvs, "ap_ssid", cfg->ap_ssid);
    nvs_set_str(nvs, "ap_pass", cfg->ap_pass);

    /* --- Écriture des entiers --- */
    nvs_set_u8(nvs, "ap_channel", cfg->ap_channel);
    nvs_set_u8(nvs, "retry_count", cfg->retry_count);
    nvs_set_u32(nvs, "retry_interval", cfg->retry_interval_ms);
    nvs_set_u8(nvs, "auth_mode", cfg->auth_mode);

    /* --- Commit --- */
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Config WiFi sauvegardée en NVS");
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "Erreur commit NVS (%s)", esp_err_to_name(err));
        return false;
    }
}
