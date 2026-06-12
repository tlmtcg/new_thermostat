/**
 * @file sht31_task.c
 * @brief Tâche FreeRTOS pour la lecture périodique du capteur SHT31 (température/humidité).
 * Gère les erreurs, les logs sur carte SD, et notifie le thermostat.
 */

#include "sht31_task.h"
#include <stdio.h>
#include <math.h>
#include "utils.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sht31.h"
#include "event_bus.h"

static const char *TAG = "SHT31_TASK";

#define SHT31_TEMP_DELTA ((float)atof(CONFIG_SHT31_TEMP_DELTA))
#define SHT31_HUM_DELTA ((float)atof(CONFIG_SHT31_HUM_DELTA))

int64_t last_log_time = 0;

static float last_published_temp = NAN;
static float last_published_hum = NAN;
static uint32_t last_publish_ms = 0;

void sht31_task(void *pvParameters)
{
    if (pvParameters == NULL)
    {
        ESP_LOGE(TAG, "Argument pvParameters obligatoire manquant ! Suppression de la tâche.");
        vTaskDelete(NULL);
        return;
    }

    sht31_task_config_t *task_config = (sht31_task_config_t *)pvParameters;
    bool was_active = false;
    ESP_LOGI(TAG, "OK");

    while (1)
    {
        // ---------------------------------------------------------
        // 1) Attente du signal de démarrage avec un timeout adaptatif
        // ---------------------------------------------------------
        EventBits_t bits = xEventGroupWaitBits(
            task_config->event_group,
            task_config->event_bit,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(100));

        if ((bits & task_config->event_bit) == 0)
        {
            if (was_active)
            {
                sht31_set_running(false);
                was_active = false;
                ESP_LOGW(TAG, "Désactivation détectée : mise en veille du SHT31");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!was_active)
        {
            sht31_set_running(true);
            was_active = true;
            ESP_LOGI(TAG, "Tâche SHT31 activée par EventGroup");
        }

        // ---------------------------------------------------------
        // 2) Lecture du capteur
        // ---------------------------------------------------------
        float temperature = 0.0f;
        float humidity = 0.0f;
        esp_err_t ret = sht31_read(&temperature, &humidity);

        // =========================================================
        // CAS 1 : LECTURE OK
        // ========================================================= 
        if (ret == ESP_OK)
        {
            sht31_reset_error_counter();

            // Vérification du Delta (Changement significatif)
            bool changed =
                isnan(last_published_temp) ||
                fabsf(temperature - last_published_temp) >= SHT31_TEMP_DELTA ||
                fabsf(humidity - last_published_hum) >= SHT31_HUM_DELTA;

            // Vérification du Heartbeat (Sécurité temps écoulé)
            bool heartbeat =
                (get_ms() - last_publish_ms) >= CONFIG_SHT31_HEARTBEAT_MS;

            if (changed || heartbeat)
            {
                // CORRECTION : On initialise UNIQUEMENT le bloc .sensor pour éviter d'écraser la mémoire
                event_t evt = {
                    .type = EVENT_SENSOR_SHT31,
                    .priority = EVENT_PRIO_NORMAL,
                    .timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
                    .sensor = {
                        .temperature = temperature,
                        .humidity = humidity
                    }
                };

                event_bus_publish(&evt);

                last_published_temp = temperature;
                last_published_hum = humidity;
                last_publish_ms = get_ms();
            }
        }
        // =========================================================
        // CAS 2 : ERREUR DE LECTURE
        // =========================================================
        else
        {
            const sht31_runtime_t *runtime = sht31_get_runtime();
            if (!runtime)
            {
                ESP_LOGE(TAG, "sht31_get_runtime() a retourné NULL");
                goto manage_delay;
            }

            event_t evt = {
                .type = EVENT_SENSOR_ERROR_SHT31,
                .priority = EVENT_PRIO_HIGH,
                .timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
                .net = {
                    .bool_value = false,
                    .retry_count = runtime->consecutive_error_count,
                    .error_code = ret
                }
            };

            event_bus_publish(&evt);

            // Log filtré pour la console
            if (runtime->consecutive_error_count <= CONFIG_SHT31_ERROR_LOG_FIRST_COUNT ||
                (runtime->consecutive_error_count % CONFIG_SHT31_ERROR_LOG_EVERY_COUNT) == 0)
            {
                ESP_LOGW(TAG,
                         "Erreur SHT31: %s (consecutives=%lu)",
                         esp_err_to_name(ret),
                         (unsigned long)runtime->consecutive_error_count);
            }

            // Récupération matérielle si le capteur est planté
            uint32_t err_count = runtime->consecutive_error_count;
            if (err_count >= CONFIG_SHT31_RECOVER_AFTER_CONSECUTIVE_ERRORS &&
                ((err_count - CONFIG_SHT31_RECOVER_AFTER_CONSECUTIVE_ERRORS) % CONFIG_SHT31_RECOVER_INTERVAL_CYCLES) == 0)
            {
                esp_err_t recover_ret = sht31_recover();
                if (recover_ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "Récupération SHT31 échouée: %s", esp_err_to_name(recover_ret));
                }
                else
                {
                    ESP_LOGI(TAG, "Récupération matérielle SHT31 réussie");
                }
            }
        }

        // ---------------------------------------------------------
        // 3) Délai adaptatif entre les lectures
        // ---------------------------------------------------------
    manage_delay:
    {
        uint32_t delay_ms = CONFIG_SHT31_DEFAULT_READ_INTERVAL_MS;

        if (task_config->delay_ms != NULL)
        {
            delay_ms = *task_config->delay_ms;
        }

        sht31_config_t config;
        if (sht31_get_config(&config) == ESP_OK && config.read_interval_ms > 0)
        {
            delay_ms = config.read_interval_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    }
}

// /**
//  * @file sht31_task.c
//  * @brief Tâche FreeRTOS pour la lecture périodique du capteur SHT31 (température/humidité).
//  * Gère les erreurs, les logs sur carte SD, et notifie le thermostat.
//  */

// #include "sht31_task.h"
// #include <stdio.h>
// #include <math.h>
// #include "utils.h"
// #include "sdkconfig.h"
// #include "esp_log.h"
// #include "freertos/task.h"
// #include "sht31.h"
// #include "event_bus.h"

// // =========================================================================
// // CONFIGURATION
// // =========================================================================

// static const char *TAG = "SHT31_TASK";

// #define SHT31_TEMP_DELTA ((float)atof(CONFIG_SHT31_TEMP_DELTA))
// #define SHT31_HUM_DELTA ((float)atof(CONFIG_SHT31_HUM_DELTA))

// int64_t last_log_time = 0;

// static float last_published_temp = NAN;
// static float last_published_hum = NAN;
// static uint32_t last_publish_ms = 0;

// // =========================================================================
// // FONCTION PRINCIPALE DE LA TÂCHE
// // =========================================================================

// void sht31_task(void *pvParameters)
// {
//     if (pvParameters == NULL)
//     {
//         ESP_LOGE(TAG, "Argument pvParameters obligatoire manquant ! Suppression de la tâche.");
//         vTaskDelete(NULL);
//         return;
//     }

//     sht31_task_config_t *task_config = (sht31_task_config_t *)pvParameters;
//     bool was_active = false;
//     ESP_LOGI(TAG, "OK");

//     while (1)
//     {
//         // ---------------------------------------------------------
//         // 1) Attente du signal de démarrage avec un timeout adaptatif
//         // ---------------------------------------------------------
//         EventBits_t bits = xEventGroupWaitBits(
//             task_config->event_group,
//             task_config->event_bit,
//             pdFALSE,
//             pdTRUE,
//             pdMS_TO_TICKS(100));

//         // Si le bit n'est pas levé, on traite l'arrêt potentiel directement
//         if ((bits & task_config->event_bit) == 0)
//         {
//             if (was_active)
//             {
//                 sht31_set_running(false);
//                 was_active = false;
//                 ESP_LOGW(TAG, "Désactivation détectée : mise en veille du SHT31");
//             }
//             vTaskDelay(pdMS_TO_TICKS(1000)); // Attente passive en mode veille
//             continue;
//         }

//         if (!was_active)
//         {
//             sht31_set_running(true);
//             was_active = true;
//             ESP_LOGI(TAG, "Tâche SHT31 activée par EventGroup");
//         }

//         // ---------------------------------------------------------
//         // 2) Lecture du capteur
//         // ---------------------------------------------------------
//         float temperature = 0.0f;
//         float humidity = 0.0f;
//         esp_err_t ret = sht31_read(&temperature, &humidity);

//         // =========================================================
//         // CAS 1 : LECTURE OK
//         // =========================================================
//         if (ret == ESP_OK)
//         {
//             sht31_reset_error_counter();

// #ifdef SHT31_DEBUG
//             ESP_LOGI(TAG, "SHT31: %.2f C, %.2f%%", temperature, humidity);
// #endif

//             // Vérification du Delta (Changement significatif)
//             bool changed =
//                 isnan(last_published_temp) ||
//                 fabsf(temperature - last_published_temp) >= SHT31_TEMP_DELTA ||
//                 fabsf(humidity - last_published_hum) >= SHT31_HUM_DELTA;

//             // Vérification du Heartbeat (Sécurité temps écoulé)
//             bool heartbeat =
//                 (get_ms() - last_publish_ms) >= CONFIG_SHT31_HEARTBEAT_MS;

//             // Envoi de l'événement si l'une des deux conditions est vraie
//             if (changed || heartbeat)
//             {
//                 event_t evt = {
//                     .type = EVENT_SENSOR_SHT31,
//                     .priority = EVENT_PRIO_NORMAL,
//                     .sensor.temperature = temperature,
//                     .sensor.humidity = humidity,
//                     .net.bool_value = true,
//                     .net.retry_count = 0,
//                     .net.error_code = ESP_OK};

//                 event_bus_publish(&evt);

//                 // Sauvegarde des états de publication
//                 last_published_temp = temperature;
//                 last_published_hum = humidity;
//                 last_publish_ms = get_ms();
//             }
//         }
//         // =========================================================
//         // CAS 2 : ERREUR DE LECTURE
//         // =========================================================
//         else
//         {
//             const sht31_runtime_t *runtime = sht31_get_runtime();
//             if (!runtime)
//             {
//                 ESP_LOGE(TAG, "sht31_get_runtime() a retourné NULL");
//                 goto manage_delay;
//             }

//             // Publication immédiate de l'erreur sur le bus (Priorité Haute)
//             event_t evt = {
//                 .type = EVENT_SENSOR_ERROR_SHT31,
//                 .priority = EVENT_PRIO_HIGH,
//                 .sensor.temperature = NAN,
//                 .sensor.humidity = NAN,
//                 .net.bool_value = false,
//                 .net.retry_count = runtime->consecutive_error_count,
//                 .net.error_code = ret};

//             event_bus_publish(&evt);

//             // Log filtré pour éviter d'inonder la console
//             if (runtime->consecutive_error_count <= CONFIG_SHT31_ERROR_LOG_FIRST_COUNT ||
//                 (runtime->consecutive_error_count % CONFIG_SHT31_ERROR_LOG_EVERY_COUNT) == 0)
//             {
//                 ESP_LOGW(TAG,
//                          "Erreur SHT31: %s (consecutives=%lu)",
//                          esp_err_to_name(ret),
//                          (unsigned long)runtime->consecutive_error_count);
//             }

//             // Filtrage du recover matériel pour ne pas saturer l'I2C à chaque tick
//             uint32_t err_count = runtime->consecutive_error_count;
//             if (err_count >= CONFIG_SHT31_RECOVER_AFTER_CONSECUTIVE_ERRORS &&
//                 ((err_count - CONFIG_SHT31_RECOVER_AFTER_CONSECUTIVE_ERRORS) % CONFIG_SHT31_RECOVER_INTERVAL_CYCLES) == 0)
//             {
//                 esp_err_t recover_ret = sht31_recover();
//                 if (recover_ret != ESP_OK)
//                 {
//                     ESP_LOGW(TAG, "Récupération SHT31 échouée: %s", esp_err_to_name(recover_ret));
//                 }
//                 else
//                 {
//                     ESP_LOGI(TAG, "Récupération matérielle SHT31 réussie");
//                 }
//             }
//         }

//         // ---------------------------------------------------------
//         // 3) Délai adaptatif entre les lectures
//         // ---------------------------------------------------------
//     manage_delay:
//     {
//         // CORRECTION : Valeur par défaut sécurisée basée sur le Kconfig
//         uint32_t delay_ms = CONFIG_SHT31_DEFAULT_READ_INTERVAL_MS;

//         if (task_config->delay_ms != NULL)
//         {
//             delay_ms = *task_config->delay_ms;
//         }

//         sht31_config_t config;
//         // On récupère proprement la configuration active via l'API du driver
//         if (sht31_get_config(&config) == ESP_OK && config.read_interval_ms > 0)
//         {
//             delay_ms = config.read_interval_ms;
//         }

//         vTaskDelay(pdMS_TO_TICKS(delay_ms));
//     }
//     }
// }
