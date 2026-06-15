#include "weather_task.h"
#include <time.h>
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include "event_bus.h"
#include <math.h>
#include "freertos/queue.h"
#include "esp_task_wdt.h"  // ✅ Déjà présent

// Seuil pour considérer un changement comme notable
#define WEATHER_TEMP_CHANGE_THRESHOLD  0.5f
#define WEATHER_HUM_CHANGE_THRESHOLD   2.0f

static const char *TAG = "WEATHER_TASK";

// Variables pour mémoriser les dernières valeurs publiées
static float s_last_published_temp = NAN;
static float s_last_published_hum = NAN;
static int s_last_published_code = -1;
static bool s_wifi_connected = false;

// Buffer pour les logs
static QueueHandle_t s_log_queue = NULL;

// Initialisation du buffer de logs
static void log_buffer_init(void) {
    s_log_queue = xQueueCreate(50, 128);  // ✅ 50 messages au lieu de 10
    if (s_log_queue == NULL) {
        ESP_LOGE(TAG, "Échec de la création de la file de logs !");
    }
}

// Ajoute un log au buffer
static void log_buffer_write(const char *tag, const char *format, ...) {
    if (s_log_queue == NULL) return;

    char *log_msg = malloc(128);
    if (log_msg == NULL) return;

    va_list args;
    va_start(args, format);
    vsnprintf(log_msg, 128, format, args);
    va_end(args);

    // ✅ Utilise xQueueSendToBack avec un timeout
    if (xQueueSendToBack(s_log_queue, &log_msg, pdMS_TO_TICKS(100)) != pdPASS) {
        free(log_msg);  // Libère si la file est pleine
    }
}

static void log_buffer_task(void *arg) {
    esp_task_wdt_add(NULL);  // ✅ Enregistre la tâche

    while (1) {
        char *log_msg = NULL;
        // ✅ Utilise un timeout de 1 seconde au lieu de portMAX_DELAY
        if (xQueueReceive(s_log_queue, &log_msg, pdMS_TO_TICKS(1000)) == pdPASS) {
            ESP_LOGI("WEATHER_TASK", "%s", log_msg);
            free(log_msg);
        }
        esp_task_wdt_reset();  // ✅ Réinitialise le WDT à chaque itération
    }
}

void weather_update_task(void *pvParameters) {
    // ✅ Ajoute la tâche au WDT dès le début
    esp_task_wdt_add(NULL);

    weather_task_config_t *config = (weather_task_config_t *)pvParameters;

    log_buffer_init();  // Initialise le buffer de logs

    // ✅ Vérifie que la file de logs a été créée
    if (s_log_queue == NULL) {
        ESP_LOGE(TAG, "Impossible de continuer sans file de logs. Abandon.");
        vTaskDelete(NULL);
        return;
    }

    // Crée la tâche pour afficher les logs
    xTaskCreate(log_buffer_task, "log_buffer_task", 2048, NULL, 1, NULL);

    if (config == NULL) {
        log_buffer_write(TAG, "Configuration de la tâche manquante ! Abandon.");
        vTaskDelete(NULL);
        return;
    }

    // Initialise les dernières valeurs publiées avec des valeurs invalides
    s_last_published_temp = NAN;
    s_last_published_hum = NAN;
    s_last_published_code = -1;

    // Abonne-toi à EVENT_WIFI_STATUS pour mettre à jour s_wifi_connected
    QueueHandle_t wifi_event_queue = event_bus_subscribe("weather_task_wifi", (event_type_t[]){EVENT_WIFI_STATUS}, 1);
    if (!wifi_event_queue) {
        log_buffer_write(TAG, "Échec de l'abonnement à EVENT_WIFI_STATUS");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        esp_task_wdt_reset();

        // Vérifie les événements WiFi
        event_t wifi_evt;
        if (event_bus_receive(wifi_event_queue, &wifi_evt, 0)) {
            if (wifi_evt.type == EVENT_WIFI_STATUS) {
                s_wifi_connected = wifi_evt.net.bool_value;
                if (s_wifi_connected) {
                    log_buffer_write(TAG, "WiFi connecté, déclenchement de la mise à jour météo.");
                    xEventGroupSetBits(config->event_group, config->event_bit);
                } else {
                    log_buffer_write(TAG, "WiFi déconnecté, attente de la reconnexion.");
                }
            }
        }

        // Vérification de l'heure
        time_t now;
        time(&now);

        if (now < 1609459200) {  // 1er janvier 2021
            log_buffer_write(TAG, "Heure non synchronisée. Attente...");
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Attente de l'événement de déclenchement
        EventBits_t bits;
        do {
            esp_task_wdt_reset();
            bits = xEventGroupWaitBits(
                config->event_group,
                config->event_bit,
                pdFALSE,
                pdTRUE,
                pdMS_TO_TICKS(1000)
            );
        } while ((bits & config->event_bit) == 0);

        // Vérifie que le WiFi est connecté
        if (!s_wifi_connected) {
            log_buffer_write(TAG, "WiFi non connecté, attente...");
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        log_buffer_write(TAG, "Démarrage du cycle de mise à jour météo globale...");
        esp_task_wdt_reset();

        // Allocation mémoire
        weather_data_t *tmp_data = heap_caps_malloc(
            sizeof(weather_data_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

        if (tmp_data == NULL) {
            tmp_data = malloc(sizeof(weather_data_t));
        }

        if (tmp_data == NULL) {
            log_buffer_write(TAG, "Erreur critique : Allocation mémoire impossible !");
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        memset(tmp_data, 0, sizeof(weather_data_t));

        // Mise à jour Météo
        esp_task_wdt_reset();
        temperature_set_valid(false);

        // ✅ Ajout de esp_task_wdt_reset() avant/après weather_update
        esp_task_wdt_reset();
        esp_err_t ret = weather_update(tmp_data);
        esp_task_wdt_reset();

        if (ret == ESP_OK) {
            log_buffer_write(TAG, "Météo mise à jour (Open-Meteo ou secours OK).");
            temperature_set_valid(true);

            // Publie EVENT_WEATHER_HOURLY uniquement si changement notable
            bool temp_changed = false;
            bool hum_changed = false;
            bool code_changed = false;

            if (!isnan(tmp_data->forecast_48h_temp[0]) &&
                !isnan(tmp_data->forecast_48h_hum[0]) &&
                tmp_data->forecast_48h_code[0] >= 0) {

                if (isnan(s_last_published_temp)) {
                    temp_changed = true;
                    hum_changed = true;
                    code_changed = true;
                } else {
                    temp_changed = fabsf(tmp_data->forecast_48h_temp[0] - s_last_published_temp) >= WEATHER_TEMP_CHANGE_THRESHOLD;
                    hum_changed = fabsf(tmp_data->forecast_48h_hum[0] - s_last_published_hum) >= WEATHER_HUM_CHANGE_THRESHOLD;
                    code_changed = (tmp_data->forecast_48h_code[0] != s_last_published_code);
                }

                if (temp_changed || hum_changed || code_changed) {
                    esp_task_wdt_reset();
                    if (!weather_publish_hourly(
                            tmp_data->forecast_48h_temp[0],
                            tmp_data->forecast_48h_hum[0],
                            tmp_data->forecast_48h_code[0]
                        )) {
                        log_buffer_write(TAG, "Échec de la publication de EVENT_WEATHER_HOURLY");
                    } else {
                        s_last_published_temp = tmp_data->forecast_48h_temp[0];
                        s_last_published_hum = tmp_data->forecast_48h_hum[0];
                        s_last_published_code = tmp_data->forecast_48h_code[0];
                        log_buffer_write(TAG, "Nouveaux données météo publiées (temp: %.1f°C, hum: %.1f%%, code: %d)",
                                         s_last_published_temp, s_last_published_hum, s_last_published_code);
                    }
                } else {
                    log_buffer_write(TAG, "Aucun changement notable dans les données météo.");
                }
            } else {
                log_buffer_write(TAG, "Données horaires invalides, EVENT_WEATHER_HOURLY non publié");
            }

            // Stockage global
            esp_task_wdt_reset();
            if (config->store_set_all) {
                config->store_set_all(tmp_data);
            }

            log_buffer_write(TAG, "Cycle complet de mise à jour réussi.");
        } else {
            log_buffer_write(TAG, "Échec total de la mise à jour Météo (%s).", esp_err_to_name(ret));
        }

        // Nettoyage mémoire
        free(tmp_data);
        tmp_data = NULL;

        // Délai entre deux cycles
        uint32_t delay_duration_ms = 3600000;  // 1 heure par défaut

        if (config->delay_ms && *config->delay_ms > 0) {
            delay_duration_ms = *config->delay_ms;
        } else {
            log_buffer_write(TAG, "Délai invalide. Utilisation de 1h.");
        }

        // Attend en nourrissant le WDT
        uint32_t remaining = delay_duration_ms;
        const uint32_t step = 1000;  // 1 seconde
        while (remaining > 0) {
            esp_task_wdt_reset();
            uint32_t chunk = (remaining > step) ? step : remaining;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            remaining -= chunk;
        }
    }
}
