#include "weather_task.h"
#include <time.h>
#include "esp_log.h"
#include "freertos/task.h"
#include "esp_heap_caps.h" 
#include "esp_task_wdt.h"

static const char *TAG = "WEATHER_TASK";

void weather_update_task(void *pvParameters)
{
    weather_task_config_t *config = (weather_task_config_t *)pvParameters;

    if (config == NULL)
    {
        ESP_LOGE(TAG, "Configuration de la tache manquante ! Abandon.");
        vTaskDelete(NULL);
        return; // Évite de continuer si le pointeur est NULL
    }

    // La tâche Meteo est surveillée par le WDT
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();

    while (1)
    {
        // --- 1) Vérification de l'heure ---
        time_t now;
        time(&now);

        if (now < 1609459200)
        {
            ESP_LOGW(TAG, "Heure non synchronisee. Attente...");
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // --- 2) Attente de l'événement de déclenchement (Sécurisée pour le WDT) ---
        // Remplacement de portMAX_DELAY par des blocs de 1 seconde pour nourrir le WDT
        EventBits_t bits;
        do {
            esp_task_wdt_reset();
            bits = xEventGroupWaitBits(config->event_group, 
                                       config->event_bit, 
                                       pdFALSE, 
                                       pdTRUE, 
                                       pdMS_TO_TICKS(1000)); // Attente de 1s max à chaque itération
        } while ((bits & config->event_bit) == 0); // Boucle tant que le bit attendu n'est pas levé

        // --- 3) Vérification WiFi ---
        if (config->is_wifi_connected && !config->is_wifi_connected())
        {
            ESP_LOGW(TAG, "Weather: WiFi non connecte, attente...");
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Demarrage du cycle de mise a jour meteo globale...");
        esp_task_wdt_reset();

        // --- 4) Allocation mémoire ---
        weather_data_t *tmp_data =
            heap_caps_malloc(sizeof(weather_data_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (tmp_data == NULL)
            tmp_data = malloc(sizeof(weather_data_t));

        if (tmp_data == NULL)
        {
            ESP_LOGE(TAG, "Erreur critique : Allocation memoire impossible !");
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        memset(tmp_data, 0, sizeof(weather_data_t));

        // --- 5) Mise à jour Météo (Gère Open-Meteo ET le secours Norvège en interne) ---
        esp_task_wdt_reset();
        temperature_set_valid(false);
        
        // weather_update renvoie ESP_OK si la météo principale OU le secours fonctionne
        esp_err_t ret = weather_update(tmp_data);

        if (ret == ESP_OK)
        {
            // Correction du log pour refléter la réalité du fallback transparent
            ESP_LOGI(TAG, "Meteo mise a jour (Open-Meteo ou Secours OK). Enchainement Jeedom...");
            temperature_set_valid(true);

            // --- 6) Mise à jour Jeedom ---
            esp_task_wdt_reset();
            // esp_err_t ret_jee = jeedom_temp_update(tmp_data);

            // if (ret_jee != ESP_OK)
            //     ESP_LOGW(TAG, "Echec Jeedom, mais Météo OK.");

            // --- 7) Stockage global ---
            if (config->store_set_all)
                config->store_set_all(tmp_data);

            ESP_LOGI(TAG, "Cycle complet de mise a jour reussi.");
        }
        else
        {
            // Ce log ne s'exécute désormais que si TOUT a échoué (Open-Meteo ET la Norvège)
            ESP_LOGE(TAG, "Echec total de la mise a jour Meteo (Principal + Secours en échec) (%s).",
                     esp_err_to_name(ret));
        }

        // --- 8) Nettoyage mémoire ---
        free(tmp_data);
        tmp_data = NULL;

        // --- 9) Délai entre deux cycles (avec WDT nourri) ---
        uint32_t delay_duration_ms = 60000;

        if (config->delay_ms && *config->delay_ms > 0)
            delay_duration_ms = *config->delay_ms;
        else
            ESP_LOGW(TAG, "Delai invalide. Utilisation de 60s.");

        uint32_t remaining = delay_duration_ms;
        const uint32_t step = 1000; // 1 seconde

        while (remaining > 0)
        {
            uint32_t chunk = (remaining > step) ? step : remaining;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            esp_task_wdt_reset();
            remaining -= chunk;
        }
    }
}
