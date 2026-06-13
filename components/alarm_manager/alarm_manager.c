#include "alarm_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


typedef enum {
    ALARM_STATE_IDLE,
    ALARM_STATE_ACTIVE,
    ALARM_STATE_ACKNOWLEDGED
} alarm_state_t;

typedef struct {
    alarm_state_t dht_error;
    alarm_state_t wifi_error;
} alarm_status_t;

static alarm_status_t system_alarms = {0};

static const char *TAG = "ALARM_MGR";

static void alarm_manager_task(void *pvParameters)
{
    // 1. Filtrer uniquement les événements de type erreur/panne
    static const event_type_t alarm_filter[] = {
        EVENT_SENSOR_ERROR_DHT,
        EVENT_SENSOR_ERROR_SHT31,
        EVENT_WIFI_STATUS // On écoute aussi le Wi-Fi pour détecter une coupure critique
    };

    QueueHandle_t alarm_queue = event_bus_subscribe("alarm_mgr", alarm_filter, 3);
    event_t evt;

    ESP_LOGI(TAG, "Alarm Manager démarré.");

    while (1)
    {
        while (1)
    {
        if (event_bus_receive(alarm_queue, &evt, portMAX_DELAY))
        {
            // --- GESTION ERREUR WIFI ---
            if (evt.type == EVENT_WIFI_STATUS)
            {
                if (evt.net.bool_value == false) {
                    if (system_alarms.wifi_error == ALARM_STATE_IDLE) {
                        ESP_LOGE(TAG, "ALERTE : Connexion perdue. Mode sécurité activé.");
                        system_alarms.wifi_error = ALARM_STATE_ACTIVE;
                    }
                } else {
                    // Si on reçoit un statut 'true', on résout automatiquement
                    if (system_alarms.wifi_error != ALARM_STATE_IDLE) {
                        ESP_LOGI(TAG, "ALERTE RÉSOULUE : Connexion rétablie.");
                        system_alarms.wifi_error = ALARM_STATE_IDLE;
                    }
                }
            }
            
            // --- GESTION ACQUITTEMENT MANUEL (Ex: via un bouton) ---
            // Si on reçoit un événement UI_ACK, on passe l'état à ACKNOWLEDGED
            if (evt.type == EVENT_UI_ACK) {
                system_alarms.dht_error = ALARM_STATE_ACKNOWLEDGED;
                ESP_LOGI(TAG, "Alarme capteur acquittée par l'utilisateur.");
            }
        }
    }
}

void alarm_manager_start(void)
{
    xTaskCreate(alarm_manager_task, "alarm_mgr", 2048, NULL, 5, NULL);
}
