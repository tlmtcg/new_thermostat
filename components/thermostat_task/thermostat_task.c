/**
 * @file thermostat.c
 * @brief Régulation par hystérésis avec arbitrage des capteurs et programme de chauffage dynamique.
 */

#include "event_bus.h"
#include "heating_program.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "THERMOSTAT";

static float current_calculated_target = 19.0f;
static QueueHandle_t thermostat_queue = NULL;

static float dht_temperature = NAN;
static float sht31_temperature = NAN;
static bool dht_valid = false;
static bool sht31_valid = false;
static float last_known_exterior_temp = 10.0f; 
static bool last_relay_state = false;

/* =========================================================================
 * 1. SOUS-COMPOSANT : ARBITRAGE DES CAPTEURS
 * ========================================================================= */
static float sensor_arbitrate_current_temperature(const char **out_source)
{
    if (sht31_valid && !isnan(sht31_temperature)) {
        *out_source = "SHT31 (Maître)";
        return sht31_temperature;
    }
    if (dht_valid && !isnan(dht_temperature)) {
        *out_source = "DHT (Sauvegarde)";
        return dht_temperature;
    }
    *out_source = "AUCUN";
    return NAN;
}

/* =========================================================================
 * 2. SOUS-COMPOSANT : ALGORITHME DE RÉGULATION
 * ========================================================================= */
static void perform_thermal_regulation(float current_temp, const char *source)
{
    const float consigne = heating_calculate_target_temperature(last_known_exterior_temp);
    const float hysteresis = 0.2f;
    bool target_state = last_relay_state;

    if (consigne != current_calculated_target) {
        current_calculated_target = consigne;
        event_t tx_consigne = { .type = EVENT_THERMOSTAT_SET, .sensor.temperature = consigne };
        event_bus_publish(&tx_consigne);
    }

    if (current_temp < (consigne - hysteresis)) {
        target_state = true;
    } else if (current_temp > (consigne + hysteresis)) {
        target_state = false;
    }

    if (target_state != last_relay_state) {
        ESP_LOGI(TAG, "CHANGEMENT D'ÉTAT via %s => Température %.1f Consigne %.1f -> Relais %s",
                 source, current_temp, consigne, target_state ? "ON" : "OFF");

        event_t response_evt = {
            .type = EVENT_RELAY_SET,
            .priority = EVENT_PRIO_NORMAL,
            .net.bool_value = target_state
        };
        event_bus_publish(&response_evt);
        last_relay_state = target_state;
    }
}

/* =========================================================================
 * 3. SOUS-COMPOSANT : SÉCURITÉ CRITIQUE
 * ========================================================================= */
static void handle_safety_fallback(void)
{
    if (last_relay_state == true) {
        ESP_LOGE(TAG, "URGENCE : Rupture totale de capteurs valides ! Arrêt du relais.");
        event_t response_evt = { .type = EVENT_RELAY_SET, .priority = EVENT_PRIO_HIGH, .net.bool_value = false };
        event_bus_publish(&response_evt);
        last_relay_state = false;
    }
}

/* =========================================================================
 * TÂCHE PRINCIPALE
 * ========================================================================= */
void thermostat_task(void *arg)
{
    // Réintégration du 7ème filtre pour intercepter l'erreur du DHT
    static const event_type_t filter[] = {
        EVENT_SENSOR_DHT,
        EVENT_SENSOR_SHT31,
        EVENT_SENSOR_ERROR_DHT, // Le 7ème filtre est de retour
        EVENT_SENSOR_ERROR_SHT31,
        EVENT_MODE_CHANGE_REQUEST,
        EVENT_MANUAL_SETPOINT_REQUEST,
        EVENT_WEATHER_UPDATE
    };

    // Ajustement du paramètre filter_count à 7
    thermostat_queue = event_bus_subscribe("thermostat", filter, 7);
    if (!thermostat_queue) {
        ESP_LOGE(TAG, "Subscribe failed");
        vTaskDelete(NULL);
        return;
    }

    event_t evt;
    ESP_LOGI(TAG, "Tâche Thermostat SOLID démarrée avec 7 filtres.");

    while (1)
    {
        if (event_bus_receive(thermostat_queue, &evt, pdMS_TO_TICKS(200)))
        {
            bool run_pipeline = false;

            switch (evt.type)
            {
                case EVENT_SENSOR_DHT:
                    ESP_LOGI(TAG, "DHT OK: Temp=%.1f Hum=%.1f", evt.sensor.temperature, evt.sensor.humidity);
                    dht_temperature = evt.sensor.temperature;
                    dht_valid = evt.net.bool_value;
                    run_pipeline = true;
                    break;

                case EVENT_SENSOR_SHT31:
                    ESP_LOGI(TAG, "SHT31 OK: Temp=%.1f Hum=%.1f", evt.sensor.temperature, evt.sensor.humidity);
                    sht31_temperature = evt.sensor.temperature;
                    sht31_valid = evt.net.bool_value;
                    run_pipeline = true;
                    break;

                case EVENT_SENSOR_ERROR_DHT:
                    ESP_LOGW(TAG, "Panne DHT reçue ! Code: %s", esp_err_to_name((esp_err_t)evt.net.error_code));
                    dht_valid = false; 
                    run_pipeline = true; 
                    break;

                case EVENT_SENSOR_ERROR_SHT31:
                    ESP_LOGE(TAG, "Panne SHT31 reçue ! Code: %s", esp_err_to_name((esp_err_t)evt.net.error_code));
                    sht31_valid = false;
                    run_pipeline = true; 
                    break;

                case EVENT_MODE_CHANGE_REQUEST:
                    heating_set_mode((heating_mode_t)(int)evt.sensor.temperature);
                    heating_save();
                    run_pipeline = true;
                    break;

                case EVENT_MANUAL_SETPOINT_REQUEST:
                    heating_set_manual_target(evt.sensor.temperature);
                    heating_save();
                    run_pipeline = true;
                    break;

                case EVENT_WEATHER_UPDATE:
                    last_known_exterior_temp = evt.sensor.temperature;
                    run_pipeline = true;
                    break;

                default:
                    break;
            }

            if (run_pipeline) 
            {
                const char* source = "AUCUN";
                float current_room_temp = sensor_arbitrate_current_temperature(&source);

                if (!isnan(current_room_temp)) {
                    perform_thermal_regulation(current_room_temp, source);
                } else {
                    handle_safety_fallback();
                }
            }
        }
    }
}

esp_err_t thermostat_init(void)
{
    esp_err_t err = heating_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Échec init heating_program");
        return err;
    }

    BaseType_t ret = xTaskCreate(thermostat_task, "thermostat_task", 4096, NULL, 4, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void thermostat_get_mode_status_str(char *dest, size_t max_len)
{
    if (!dest) return;
    heating_mode_t mode = heating_get_mode();
    switch (mode) {
        case HEATING_MODE_AUTO:      snprintf(dest, max_len, "AUTO"); break;
        case HEATING_MODE_MANUAL:    snprintf(dest, max_len, "MANU (%.1f)", heating_get_manual_target()); break;
        case HEATING_MODE_ABSENT:    snprintf(dest, max_len, "ABSENT"); break;
        case HEATING_MODE_HORS_GEL:  snprintf(dest, max_len, "H-GEL"); break;
        default:                     snprintf(dest, max_len, "UNKNOWN"); break;
    }
}
