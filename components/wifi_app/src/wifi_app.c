// #include "wifi_app.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "event_bus.h" // Intégration de l'Event Bus

static const char *TAG = "WIFI_APP";

// --- TIMER GLOBAL ---
static TimerHandle_t xReconnectTimer = NULL;

// --- CALLBACKS ---
static void vTimerReconnectCallback(TimerHandle_t xTimer)
{
    wifi_sta_list_t clients;
    // Récupère la liste complète des stations connectées à ton AP
    if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK)
    {
        if (clients.num > 0)
        {
            ESP_LOGW(TAG, "%d client(s) actifs sur l'AP. Report du scan STA...", clients.num);
            xTimerStart(xReconnectTimer, 0);
            return;
        }
    }

    // Si on arrive ici, l'AP est seul : on tente la reconnexion
    ESP_LOGI(TAG, "Lancement de la reconnexion STA...");
    esp_wifi_connect();
}

static void on_sta_connected(const esp_ip4_addr_t *ip)
{
    ESP_LOGI(TAG, "STA connectee ! IP : " IPSTR, IP2STR(ip));
    
    // CORRECTION SECURIATION MEMOIRE : Rendu 'static' pour que la chaîne 
    // persiste en mémoire après la fin de cette fonction, le temps que l'OLED la lise.
    static char ip_str[16]; 
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(ip));

    // CORRECTION UNION : On initialise uniquement la partie diagnostic d'abord
    event_t evt = {
        .type = EVENT_WIFI_STATUS,
        .priority = EVENT_PRIO_NORMAL,
        .timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
        .net = {
            .bool_value = true, // WiFi disponible
            .error_code = 0
        }
    };
    
    // CORRECTION UNION : On affecte le pointeur juste après pour ne pas casser l'initialiseur
    evt.payload.payload_ptr = ip_str; 

    event_bus_publish(&evt);

    if (xReconnectTimer)
        xTimerStop(xReconnectTimer, 0);
}

static void on_sta_failed(int reason)
{
    ESP_LOGW(TAG, "Echec de la connexion WiFi, raison: %d", reason);

    // CORRECTION: Initialisation propre et exclusive du bloc .net
    event_t evt = {
        .type = EVENT_WIFI_STATUS,
        .priority = EVENT_PRIO_HIGH,
        .timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
        .net = {
            .bool_value = false, // WiFi KO
            .error_code = reason  // On transmet la cause exacte (Ex: WIFI_REASON_NO_AP_FOUND)
        }
    };
    
    event_bus_publish(&evt);

    if (xReconnectTimer)
        xTimerStart(xReconnectTimer, 0);
}

// --- API PUBLIQUE ---

void wifi_app_start(void)
{
    ESP_LOGI(TAG, "Initialisation du module WiFi...");

    // 1. Timer de reconnexion
    xReconnectTimer = xTimerCreate("WiFi_Retrier",
                                   pdMS_TO_TICKS(5000),
                                   pdFALSE,
                                   NULL,
                                   vTimerReconnectCallback);

    // 2. Callbacks WiFi Manager
    static wifi_callbacks_t cb = {0};
    cb.on_sta_connected = on_sta_connected;
    cb.on_sta_failed = on_sta_failed;
    cb.on_ap_started = NULL;

    // 3. Lancement du WiFi Manager
    wifi_manager_init(&cb);

    // Publication de l'état initial déconnecté au démarrage du gestionnaire
    event_t init_evt = {
        .type = EVENT_WIFI_STATUS,
        .priority = EVENT_PRIO_NORMAL,
        .timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
        .net = {
            .bool_value = false,
            .error_code = 0
        }
    };
    event_bus_publish(&init_evt);

    ESP_LOGI(TAG, "wifi_app_start termine.");
}
