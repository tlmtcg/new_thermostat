#include "wifi_manager.h"
#include "wifi_storage.h"
#include "wifi_mode.h"
#include "wifi_state_machine.h"
#include "wifi_events.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>
#include "sdkconfig.h"

static const char *TAG = "WIFI_MGR";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static TimerHandle_t s_wifi_retry_timer;
static int s_retry_num = 0;
static bool s_sta_connected = false;
static int s_ap_clients = 0;
static bool s_test_mode = false;

static void wifi_retry_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Retry STA...");
    esp_wifi_connect();
}

// ===================== API PUBLIQUE =====================

void wifi_manager_init(const wifi_callbacks_t *callbacks)
{
    ESP_LOGI(TAG, "Init WiFi Manager");

    // 1. Event groups + retry timer
    s_wifi_event_group = xEventGroupCreate();
    s_wifi_retry_timer = xTimerCreate("wifi_retry",
                                      pdMS_TO_TICKS(CONFIG_ESP_WIFI_RETRY_INTERVAL_MS),
                                      pdFALSE,
                                      NULL,
                                      wifi_retry_callback);

    // 2. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 3. esp-netif + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3B. CREATION MANUELLE DE L'INTERFACE STA SANS IPv6
    esp_netif_inherent_config_t sta_base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    sta_base.ip_info = NULL;   // Pas d'IPv6 auto, pas de DHCPv6

    esp_netif_config_t sta_cfg = {
        .base = &sta_base,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
    };

    esp_netif_t *sta_netif = esp_netif_new(&sta_cfg);
    esp_netif_attach_wifi_station(sta_netif);
    esp_wifi_set_default_wifi_sta_handlers();

    // 3C. CREATION DE L'INTERFACE AP
    esp_netif_create_default_wifi_ap();

    // 4. Initialisation du driver WiFi
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    // Stockage RAM forcée pour éviter les résidus NVS conflictuels
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Liaison des gestionnaires d'événements système (wifi_events.c)
    wifi_events_init(callbacks);

    // 5. Chargement de la configuration stockée
    if (!wifi_storage_load_all(&g_wifi_cfg))
    {
        ESP_LOGW(TAG, "Aucune config WiFi en NVS, utilisation des valeurs Kconfig");

        strlcpy(g_wifi_cfg.sta_ssid, CONFIG_ESP_WIFI_STA_SSID, sizeof(g_wifi_cfg.sta_ssid));
        strlcpy(g_wifi_cfg.sta_pass, CONFIG_ESP_WIFI_STA_PASSWORD, sizeof(g_wifi_cfg.sta_pass));

        strlcpy(g_wifi_cfg.ap_ssid, CONFIG_ESP_WIFI_AP_SSID, sizeof(g_wifi_cfg.ap_ssid));
        strlcpy(g_wifi_cfg.ap_pass, CONFIG_ESP_WIFI_AP_PASSWORD, sizeof(g_wifi_cfg.ap_pass));

        g_wifi_cfg.ap_channel = CONFIG_ESP_WIFI_AP_CHANNEL;
        g_wifi_cfg.retry_count = CONFIG_ESP_MAXIMUM_STA_RETRY;
        g_wifi_cfg.retry_interval_ms = CONFIG_ESP_WIFI_RETRY_INTERVAL_MS;
        g_wifi_cfg.auth_mode = WIFI_AUTH_WPA2_PSK;

        wifi_storage_save_all(&g_wifi_cfg);
    }

    ESP_LOGI(TAG, "SSID STA : %s", g_wifi_cfg.sta_ssid);

    // 6. Configuration du profil Station
    wifi_config_t sta_wifi_cfg = {0};
    strlcpy((char *)sta_wifi_cfg.sta.ssid, g_wifi_cfg.sta_ssid, sizeof(sta_wifi_cfg.sta.ssid));
    strlcpy((char *)sta_wifi_cfg.sta.password, g_wifi_cfg.sta_pass, sizeof(sta_wifi_cfg.sta.password));

    sta_wifi_cfg.sta.threshold.authmode = g_wifi_cfg.auth_mode;
    sta_wifi_cfg.sta.pmf_cfg.capable = true;
    sta_wifi_cfg.sta.pmf_cfg.required = false;

    // 7. Configuration du profil Access Point
    wifi_config_t ap_wifi_cfg = {0};
    strlcpy((char *)ap_wifi_cfg.ap.ssid, g_wifi_cfg.ap_ssid, sizeof(ap_wifi_cfg.ap.ssid));
    strlcpy((char *)ap_wifi_cfg.ap.password, g_wifi_cfg.ap_pass, sizeof(ap_wifi_cfg.ap.password));

    ap_wifi_cfg.ap.ssid_len = strlen(g_wifi_cfg.ap_ssid);
    ap_wifi_cfg.ap.channel = g_wifi_cfg.ap_channel;
    ap_wifi_cfg.ap.max_connection = CONFIG_ESP_MAX_STA_CONN_AP;
    ap_wifi_cfg.ap.authmode = (strlen(g_wifi_cfg.ap_pass) >= 8)
                                  ? WIFI_AUTH_WPA2_PSK
                                  : WIFI_AUTH_OPEN;

    // 8. Application + démarrage
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_wifi_cfg));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Lancement de la tentative de connexion à la Box...");
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi démarré (AP+STA, IPv4 ONLY)");
}


wifi_state_t wifi_get_state(void)
{
    return wifi_state_get();
}

// Cette fonction doit être appelée par wifi_events.c
void wifi_manager_update_client_count(int count)
{
    s_ap_clients = count;
    // On profite de la mise à jour pour réévaluer le mode (STA pur ou AP+STA)
    // On récupère l'état de connexion actuel pour ne pas faire d'erreur
    bool connected = (wifi_state_get() == WIFI_STATE_STA_CONNECTED);
    wifi_mode_update(connected, s_ap_clients, false);
}

int wifi_get_ap_client_count(void)
{
    return s_ap_clients;
}

void wifi_manager_try_connect(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Test de connexion vers %s", ssid);

    s_test_mode = true;
    s_retry_num = 0;
    xTimerStop(s_wifi_retry_timer, 0);

    /* 1) Préparer la config STA temporaire */
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));

    if (pass)
        strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    else
        sta_cfg.sta.password[0] = '\0';

    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    /* 2) Reset des flags de connexion */
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* 3) Appliquer la config temporaire */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    esp_wifi_connect();

    /* 4) Attendre le résultat */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connexion OK → mise à jour de la config NVS");

        /* Mise à jour de g_wifi_cfg */
        strlcpy(g_wifi_cfg.sta_ssid, ssid, sizeof(g_wifi_cfg.sta_ssid));
        strlcpy(g_wifi_cfg.sta_pass, pass ? pass : "", sizeof(g_wifi_cfg.sta_pass));

        /* Sauvegarde NVS */
        wifi_storage_save_all(&g_wifi_cfg);
    }
    else
    {
        ESP_LOGE(TAG, "Échec connexion");
    }

    /* 5) Fin du mode test */
    s_test_mode = false;

    /* Mise à jour du mode AP/STA */
    wifi_mode_update(s_sta_connected, s_ap_clients, s_test_mode);
}


void wifi_manager_force_disconnect(void)
{
    ESP_LOGW(TAG, "Simulation de panne : Déconnexion forcée...");
    // On arrête le timer de retry pour éviter qu'il ne se reconnecte tout seul
    xTimerStop(s_wifi_retry_timer, 0);
    esp_wifi_disconnect();
}

void wifi_manager_reload_config(void)
{
    ESP_LOGW(TAG, "Rechargement complet de la configuration WiFi...");

    /* 1) Stopper proprement le WiFi */
    xTimerStop(s_wifi_retry_timer, 0);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_wifi_stop());

    /* 2) Reconfigurer STA */
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, g_wifi_cfg.sta_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, g_wifi_cfg.sta_pass, sizeof(sta_cfg.sta.password));

    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.threshold.authmode = g_wifi_cfg.auth_mode;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    /* 3) Reconfigurer AP */
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, g_wifi_cfg.ap_ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, g_wifi_cfg.ap_pass, sizeof(ap_cfg.ap.password));

    ap_cfg.ap.ssid_len = strlen(g_wifi_cfg.ap_ssid);
    ap_cfg.ap.channel = g_wifi_cfg.ap_channel;
    ap_cfg.ap.max_connection = CONFIG_ESP_MAX_STA_CONN_AP;

    if (strlen(g_wifi_cfg.ap_pass) >= 8)
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    else
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    /* 4) Appliquer les nouvelles configs */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* 5) Redémarrer le WiFi */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 6) Relancer la connexion STA */
    ESP_LOGI(TAG, "Connexion STA vers %s...", g_wifi_cfg.sta_ssid);
    esp_wifi_connect();

    /* 7) Réinitialiser l’état interne */
    s_retry_num = 0;
    s_test_mode = false;

    ESP_LOGI(TAG, "Configuration WiFi rechargée.");
}
