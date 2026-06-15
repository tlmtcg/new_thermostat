/**
 * @file thermostat.c
 * @brief Régulation par hystérésis avec arbitrage des capteurs et programme de chauffage dynamique.
 */

#include "event_bus.h"
#include "heating_program.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <time.h>
#include "utils.h"
#include "weather.h"
#include "esp_task_wdt.h"

/* =========================================================================
 * CONSTANTES ET VARIABLES GLOBALES
 * ========================================================================= */

static const char *TAG = "THERMOSTAT";

// --- Données des capteurs ---
static float dht_temperature = NAN;
static float sht31_temperature = NAN;
static bool dht_valid = false;
static bool sht31_valid = false;

// --- État du relais ---
static bool last_relay_state = false;

// --- Consigne calculée ---
static float current_calculated_target = 19.0f;

// --- Données météo ---
static float last_known_exterior_temp = 10.0f;

// --- Queue d'événements ---
static QueueHandle_t s_thermostat_event_queue = NULL;

// --- Données météo horaires ---
typedef struct {
    float temperature;
    float humidity;
    int weather_code;
    time_t timestamp;
} thermostat_weather_hourly_t;

static thermostat_weather_hourly_t s_hourly_weather = {0};
static SemaphoreHandle_t s_weather_mutex = NULL;

/* =========================================================================
 * DÉCLARATIONS DE FONCTIONS
 * ========================================================================= */
void thermostat_update_hourly_weather(float temperature, float humidity, int weather_code);
esp_err_t thermostat_init(void);
void thermostat_task(void *arg);
void thermostat_get_mode_status_str(char *dest, size_t max_len);

/* =========================================================================
 * 1. SOUS-COMPOSANT : ARBITRAGE DES CAPTEURS
 * ========================================================================= */

static float sensor_arbitrate_current_temperature(const char **out_source) {
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

static void perform_thermal_regulation(float current_temp, const char *source) {
    const float consigne = heating_calculate_target_temperature(last_known_exterior_temp);
    const float hysteresis = 0.2f;

    const float low_threshold = consigne - hysteresis;
    const float high_threshold = consigne + hysteresis;

    bool target_state = last_relay_state;

    if (fabsf(consigne - current_calculated_target) > 0.01f) {
        current_calculated_target = consigne;
        event_t tx_consigne = {
            .type = EVENT_THERMOSTAT_SET,
            .sensor.temperature = consigne
        };
        event_bus_publish(&tx_consigne);
    }

    if (current_temp < low_threshold) {
        target_state = true;
    } else if (current_temp > high_threshold) {
        target_state = false;
    }

    const float delta = consigne - current_temp;

    ESP_LOGI(TAG,
             "[THERMO] src=%s mode=%s "
             "ext=%.1f°C temp=%.2f°C consigne=%.2f°C "
             "delta=%+.2f°C hyst=±%.2f°C "
             "seuils=[%.2f / %.2f] relais=%s -> cible=%s",
             source ? source : "N/A",
             heating_mode_to_string(heating_get_mode()),
             last_known_exterior_temp,
             current_temp,
             consigne,
             delta,
             hysteresis,
             low_threshold,
             high_threshold,
             last_relay_state ? "ON" : "OFF",
             target_state ? "ON" : "OFF");

    if (target_state != last_relay_state) {
        ESP_LOGI(TAG,
                 "[THERMO] CHANGEMENT ETAT "
                 "src=%s mode=%s "
                 "temp=%.2f°C consigne=%.2f°C delta=%+.2f°C "
                 "relais %s -> %s",
                 source ? source : "N/A",
                 heating_mode_to_string(heating_get_mode()),
                 current_temp,
                 consigne,
                 delta,
                 last_relay_state ? "ON" : "OFF",
                 target_state ? "ON" : "OFF");

        event_t relay_evt = {
            .type = EVENT_RELAY_SET,
            .priority = EVENT_PRIO_NORMAL,
            .net.bool_value = target_state
        };
        event_bus_publish(&relay_evt);
        last_relay_state = target_state;
    }
}

/* =========================================================================
 * 3. SOUS-COMPOSANT : SÉCURITÉ CRITIQUE
 * ========================================================================= */

static void handle_safety_fallback(void) {
    if (last_relay_state == true) {
        ESP_LOGE(TAG, "URGENCE : Rupture totale de capteurs valides ! Arrêt du relais.");
        event_t relay_evt = {
            .type = EVENT_RELAY_SET,
            .priority = EVENT_PRIO_HIGH,
            .net.bool_value = false
        };
        event_bus_publish(&relay_evt);
        last_relay_state = false;
    }
}

/* =========================================================================
 * 4. SOUS-COMPOSANT : GESTION DES DONNÉES MÉTÉO
 * ========================================================================= */

static void thermostat_weather_init(void) {
    s_weather_mutex = xSemaphoreCreateMutex();
    if (!s_weather_mutex) {
        ESP_LOGE(TAG, "Échec de la création du mutex pour les données météo");
    }
    s_hourly_weather.timestamp = 0;
}

void thermostat_update_hourly_weather(float temperature, float humidity, int weather_code) {
    if (temperature < -50.0f || temperature > 60.0f) {
        ESP_LOGW(TAG, "Température invalide : %.1f°C", temperature);
        return;
    }
    if (humidity < 0.0f || humidity > 100.0f) {
        ESP_LOGW(TAG, "Humidité invalide : %.1f%%", humidity);
        return;
    }
    if (weather_code < 0 || weather_code > 99) {
        ESP_LOGW(TAG, "Code météo invalide : %d", weather_code);
        return;
    }

    if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_hourly_weather.temperature = temperature;
        s_hourly_weather.humidity = humidity;
        s_hourly_weather.weather_code = weather_code;
        s_hourly_weather.timestamp = time(NULL);

        ESP_LOGI(TAG, "Données météo horaires mises à jour : temp=%.1f°C, hum=%.1f%%, code=%d",
                 temperature, humidity, weather_code);

        xSemaphoreGive(s_weather_mutex);
    } else {
        ESP_LOGE(TAG, "Impossible d'acquérir le mutex pour mettre à jour les données météo");
    }
}

/* =========================================================================
 * 5. FONCTIONS PUBLIQUES
 * ========================================================================= */

void thermostat_get_mode_status_str(char *dest, size_t max_len) {
    if (!dest || max_len == 0) {
        return;
    }

    heating_mode_t mode = heating_get_mode();
    switch (mode) {
        case HEATING_MODE_AUTO:
            snprintf(dest, max_len, "AUTO");
            break;
        case HEATING_MODE_MANUAL:
            snprintf(dest, max_len, "MANU (%.1f)", heating_get_manual_target());
            break;
        case HEATING_MODE_ABSENT:
            snprintf(dest, max_len, "ABSENT");
            break;
        case HEATING_MODE_HORS_GEL:
            snprintf(dest, max_len, "H-GEL");
            break;
        default:
            snprintf(dest, max_len, "UNKNOWN");
            break;
    }
}

/* =========================================================================
 * 6. INITIALISATION ET TÂCHE PRINCIPALE
 * ========================================================================= */

esp_err_t thermostat_init(void) {
    esp_err_t err = heating_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Échec de l'initialisation de heating_program");
        return err;
    }

    thermostat_weather_init();

    // Filtre pour les événements
    event_type_t filter[] = {
        EVENT_SENSOR_DHT,
        EVENT_SENSOR_SHT31,
        EVENT_SENSOR_ERROR_DHT,
        EVENT_SENSOR_ERROR_SHT31,
        EVENT_MODE_CHANGE_REQUEST,
        EVENT_MANUAL_SETPOINT_REQUEST,
        EVENT_WEATHER_UPDATE,
        EVENT_WEATHER_HOURLY,
        EVENT_NET_TIME_SYNCED,
    };

    // Abonne-toi aux événements
    s_thermostat_event_queue = event_bus_subscribe("thermostat", filter, sizeof(filter)/sizeof(filter[0]));
    if (!s_thermostat_event_queue) {
        ESP_LOGE(TAG, "Échec de l'abonnement aux événements");
        return ESP_FAIL;
    }

    // Log le nombre de filtres ici (où filter est défini)
    ESP_LOGI(TAG, "Thermostat initialisé avec %d filtres.", sizeof(filter)/sizeof(filter[0]));

    // Crée la tâche thermostat
    BaseType_t ret = xTaskCreate(
        thermostat_task,
        "thermostat_task",
        8192,
        NULL,
        4,
        NULL
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Échec de la création de la tâche thermostat");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Thermostat initialisé avec succès");
    return ESP_OK;
}

void thermostat_task(void *arg) {
    (void)arg;
    event_t evt;

    ESP_LOGI(TAG, "Tâche Thermostat démarrée.");  // Log simplifié (sans référence à filter)

     esp_task_wdt_add(NULL);  // ✅ Ajoute la tâche au WDT

    while (1) {
        esp_task_wdt_reset();  // ✅ Réinitialise le WDT
        if (event_bus_receive(s_thermostat_event_queue, &evt, pdMS_TO_TICKS(200))) {
            bool run_pipeline = false;

            switch (evt.type) {
                case EVENT_SENSOR_DHT: {
                    ESP_LOGI(TAG, "DHT OK: Temp=%.1f°C Hum=%.1f%%",
                             evt.sensor.temperature, evt.sensor.humidity);
                    dht_temperature = evt.sensor.temperature;
                    dht_valid = true;
                    run_pipeline = true;
                    break;
                }

                case EVENT_SENSOR_SHT31: {
                    ESP_LOGI(TAG, "SHT31 OK: Temp=%.1f°C Hum=%.1f%%",
                             evt.sensor.temperature, evt.sensor.humidity);
                    sht31_temperature = evt.sensor.temperature;
                    sht31_valid = true;
                    run_pipeline = true;
                    break;
                }

                case EVENT_SENSOR_ERROR_DHT: {
                    ESP_LOGW(TAG, "Panne DHT reçue ! Code: %s",
                             esp_err_to_name((esp_err_t)evt.net.error_code));
                    dht_valid = false;
                    run_pipeline = true;
                    break;
                }

                case EVENT_SENSOR_ERROR_SHT31: {
                    ESP_LOGE(TAG, "Panne SHT31 reçue ! Code: %s",
                             esp_err_to_name((esp_err_t)evt.net.error_code));
                    sht31_valid = false;
                    run_pipeline = true;
                    break;
                }

                case EVENT_MODE_CHANGE_REQUEST: {
                    heating_set_mode((heating_mode_t)(int)evt.sensor.temperature);
                    heating_save();
                    run_pipeline = true;
                    break;
                }

                case EVENT_MANUAL_SETPOINT_REQUEST: {
                    heating_set_manual_target(evt.sensor.temperature);
                    heating_save();
                    run_pipeline = true;
                    break;
                }

                case EVENT_WEATHER_UPDATE: {
                    weather_data_t *weather_data = (weather_data_t *)evt.payload.payload_ptr;
                    if (weather_data) {
                        last_known_exterior_temp = weather_data->current.temperature;
                        ESP_LOGI(TAG, "Nouvelle température extérieure: %.1f°C",
                                 last_known_exterior_temp);
                    } else {
                        ESP_LOGW(TAG, "EVENT_WEATHER_UPDATE reçu sans payload valide");
                    }
                    run_pipeline = true;
                    break;
                }

                case EVENT_WEATHER_HOURLY: {
                    ESP_LOGI(TAG, "Données météo horaires reçues: temp=%.1f°C, hum=%.1f%%, code=%d",
                             evt.weather_hourly.temperature,
                             evt.weather_hourly.humidity,
                             evt.weather_hourly.weather_code);
                    thermostat_update_hourly_weather(
                        evt.weather_hourly.temperature,
                        evt.weather_hourly.humidity,
                        evt.weather_hourly.weather_code
                    );
                    break;
                }

                case EVENT_NET_TIME_SYNCED: {
                    const char *source = "SYNC";
                    float temp = sensor_arbitrate_current_temperature(&source);
                    if (!isnan(temp)) {
                        perform_thermal_regulation(temp, source);
                    }
                    break;
                }

                default: {
                    ESP_LOGW(TAG, "Événement non traité : %d", evt.type);
                    break;
                }
            }

            if (run_pipeline) {
                const char *source = "AUCUN";
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
