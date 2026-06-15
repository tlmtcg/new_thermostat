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
#include "cJSON.h"           // Librairie cJSON pour générer du JSON
#include "heating_program.h" // Pour heating_get_mode(),
#include "thermostat_task.h"
#include "heating_program.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "relay.h"

/* =========================================================================
 * CONSTANTES ET VARIABLES GLOBALES
 * ========================================================================= */

static const char *TAG = "THERMOSTAT";

// --- Données des capteurs ---
static float dht_temperature = NAN;
static float sht31_temperature = NAN;
static bool dht_valid = false;
static bool sht31_valid = false;

// Source de température actuelle
static temperature_source_t current_source = TEMP_SOURCE_NONE;

// --- État du relais ---
static bool last_relay_state = false;

// --- Consigne calculée ---
static float current_calculated_target = 19.0f;

// --- Données météo ---
static float last_known_exterior_temp = 10.0f;

// --- Queue d'événements ---
static QueueHandle_t s_thermostat_event_queue = NULL;

// --- Données météo horaires ---
typedef struct
{
    float temperature;
    float humidity;
    int weather_code;
    time_t timestamp;
} thermostat_weather_hourly_t;

static thermostat_weather_hourly_t s_hourly_weather = {0};
static SemaphoreHandle_t s_weather_mutex = NULL;

static float s_hysteresis = 0.2;    // Hystérésis par défaut

/* =========================================================================
 * DÉCLARATIONS DE FONCTIONS
 * ========================================================================= */
void thermostat_update_hourly_weather(float temperature, float humidity, int weather_code);
esp_err_t thermostat_init(void);
void thermostat_task(void *arg);


float heating_get_hysteresis(void) {
    return s_hysteresis;
}

void heating_set_hysteresis(float hysteresis) {
    s_hysteresis = hysteresis;
}

/* =========================================================================
 * 1. SOUS-COMPOSANT : ARBITRAGE DES CAPTEURS
 * ========================================================================= */

// Définit la source de température
void temperature_set_source(temperature_source_t source)
{
    current_source = source;
}

// Récupère la source de température actuelle
temperature_source_t temperature_get_source(void)
{
    return current_source;
}

// Fonction d'arbitrage des capteurs
static float sensor_arbitrate_current_temperature(const char **out_source)
{
    if (sht31_valid && !isnan(sht31_temperature))
    {
        *out_source = "SHT31 (Maître)";
        return sht31_temperature;
    }
    if (dht_valid && !isnan(dht_temperature))
    {
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

    const float low_threshold = consigne - s_hysteresis;
    const float high_threshold = consigne + s_hysteresis;

    bool target_state = last_relay_state;

    if (fabsf(consigne - current_calculated_target) > 0.01f)
    {
        current_calculated_target = consigne;
        event_t tx_consigne = {
            .type = EVENT_THERMOSTAT_SET,
            .sensor.temperature = consigne};
        event_bus_publish(&tx_consigne);
    }

    if (current_temp < low_threshold)
    {
        target_state = true;
    }
    else if (current_temp > high_threshold)
    {
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
             s_hysteresis,
             low_threshold,
             high_threshold,
             last_relay_state ? "ON" : "OFF",
             target_state ? "ON" : "OFF");

    if (target_state != last_relay_state)
    {
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
            .net.bool_value = target_state};
        event_bus_publish(&relay_evt);
        last_relay_state = target_state;
    }
}

/* =========================================================================
 * 3. SOUS-COMPOSANT : SÉCURITÉ CRITIQUE
 * ========================================================================= */

static void handle_safety_fallback(void)
{
    if (last_relay_state == true)
    {
        ESP_LOGE(TAG, "URGENCE : Rupture totale de capteurs valides ! Arrêt du relais.");
        event_t relay_evt = {
            .type = EVENT_RELAY_SET,
            .priority = EVENT_PRIO_HIGH,
            .net.bool_value = false};
        event_bus_publish(&relay_evt);
        last_relay_state = false;
    }
}

/* =========================================================================
 * 4. SOUS-COMPOSANT : GESTION DES DONNÉES MÉTÉO
 * ========================================================================= */

static void thermostat_weather_init(void)
{
    s_weather_mutex = xSemaphoreCreateMutex();
    if (!s_weather_mutex)
    {
        ESP_LOGE(TAG, "Échec de la création du mutex pour les données météo");
    }
    s_hourly_weather.timestamp = 0;
}

void thermostat_update_hourly_weather(float temperature, float humidity, int weather_code)
{
    if (temperature < -50.0f || temperature > 60.0f)
    {
        ESP_LOGW(TAG, "Température invalide : %.1f°C", temperature);
        return;
    }
    if (humidity < 0.0f || humidity > 100.0f)
    {
        ESP_LOGW(TAG, "Humidité invalide : %.1f%%", humidity);
        return;
    }
    if (weather_code < 0 || weather_code > 99)
    {
        ESP_LOGW(TAG, "Code météo invalide : %d", weather_code);
        return;
    }

    if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_hourly_weather.temperature = temperature;
        s_hourly_weather.humidity = humidity;
        s_hourly_weather.weather_code = weather_code;
        s_hourly_weather.timestamp = time(NULL);

        ESP_LOGI(TAG, "Données météo horaires mises à jour : temp=%.1f°C, hum=%.1f%%, code=%d",
                 temperature, humidity, weather_code);

        xSemaphoreGive(s_weather_mutex);
    }
    else
    {
        ESP_LOGE(TAG, "Impossible d'acquérir le mutex pour mettre à jour les données météo");
    }
}

/* =========================================================================
 * 5. FONCTIONS PUBLIQUES
 * ========================================================================= */

/**
 * @brief Génère un JSON avec l'état actuel du thermostat.
 * @return Une chaîne JSON allouée dynamiquement (à libérer avec free()).
 */
char *thermostat_get_json_status(void)
{
    // Alloue un buffer pour le JSON
    char *json_str = NULL;

    // Crée un objet JSON racine
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE("THERMOSTAT", "Échec de l'allocation pour le JSON");
        return NULL;
    }

    // --- 1. Mode de chauffage ---
    char mode_str[32];
    thermostat_get_mode_status_str(mode_str, sizeof(mode_str));
    cJSON_AddStringToObject(root, "mode", mode_str);

    // --- 2. Température actuelle et source ---
    cJSON *temp_source = cJSON_GetObjectItem(root, "temp_source");
    if (cJSON_IsString(temp_source))
    {
        if (strcmp(temp_source->valuestring, "SHT31") == 0)
        {
            temperature_set_source(TEMP_SOURCE_SHT31); // ✅ Utilise l'énumération
        }
        else if (strcmp(temp_source->valuestring, "DHT") == 0)
        {
            temperature_set_source(TEMP_SOURCE_DHT); // ✅ Utilise l'énumération
        }
    }

    // --- 3. Consigne actuelle ---
    float target_temp = heating_get_manual_target();
    cJSON_AddNumberToObject(root, "target_temp", target_temp);

    // --- 4. Température extérieure ---
    float exterior_temp = temperature_get_outdoor();
    cJSON_AddNumberToObject(root, "exterior_temp", exterior_temp);

    // --- 5. État du relais ---
    bool relay_state = get_relay_state();
    cJSON_AddBoolToObject(root, "relay_on", relay_state);

    // --- 6. Hystérésis ---
    float hysteresis = heating_get_hysteresis();
    cJSON_AddNumberToObject(root, "hysteresis", hysteresis);

    // --- 8. Heure de la dernière mise à jour ---
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    cJSON_AddStringToObject(root, "last_update", time_str);

    // --- 9. État des capteurs ---
    cJSON *sensors = cJSON_AddObjectToObject(root, "sensors");
    cJSON_AddBoolToObject(sensors, "sht31_valid", sht31_valid);
    cJSON_AddBoolToObject(sensors, "dht_valid", dht_valid);
    cJSON_AddNumberToObject(sensors, "sht31_temp", sht31_temperature);
    cJSON_AddNumberToObject(sensors, "dht_temp", dht_temperature);

    // --- 10. Convertit l'objet JSON en chaîne ---
    json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root); // Libère la mémoire de l'objet JSON

    return json_str;
}

/* =========================================================================
 * 6. INITIALISATION ET TÂCHE PRINCIPALE
 * ========================================================================= */

esp_err_t thermostat_init(void)
{
    esp_err_t err = heating_init();
    if (err != ESP_OK)
    {
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
    s_thermostat_event_queue = event_bus_subscribe("thermostat", filter, sizeof(filter) / sizeof(filter[0]));
    if (!s_thermostat_event_queue)
    {
        ESP_LOGE(TAG, "Échec de l'abonnement aux événements");
        return ESP_FAIL;
    }

    // Log le nombre de filtres ici (où filter est défini)
    ESP_LOGI(TAG, "Thermostat initialisé avec %d filtres.", sizeof(filter) / sizeof(filter[0]));

    // Crée la tâche thermostat
    BaseType_t ret = xTaskCreate(
        thermostat_task,
        "thermostat_task",
        8192,
        NULL,
        4,
        NULL);

    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Échec de la création de la tâche thermostat");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Thermostat initialisé avec succès");
    return ESP_OK;
}

void thermostat_task(void *arg)
{
    (void)arg;
    event_t evt;

    ESP_LOGI(TAG, "Tâche Thermostat démarrée."); // Log simplifié (sans référence à filter)

    esp_task_wdt_add(NULL); // ✅ Ajoute la tâche au WDT

    while (1)
    {
        esp_task_wdt_reset(); // ✅ Réinitialise le WDT
        if (event_bus_receive(s_thermostat_event_queue, &evt, pdMS_TO_TICKS(200)))
        {
            bool run_pipeline = false;

            switch (evt.type)
            {
            case EVENT_SENSOR_DHT:
            {
                ESP_LOGI(TAG, "DHT OK: Temp=%.1f°C Hum=%.1f%%",
                         evt.sensor.temperature, evt.sensor.humidity);
                dht_temperature = evt.sensor.temperature;
                dht_valid = true;
                run_pipeline = true;
                break;
            }

            case EVENT_SENSOR_SHT31:
            {
                ESP_LOGI(TAG, "SHT31 OK: Temp=%.1f°C Hum=%.1f%%",
                         evt.sensor.temperature, evt.sensor.humidity);
                sht31_temperature = evt.sensor.temperature;
                sht31_valid = true;
                run_pipeline = true;
                break;
            }

            case EVENT_SENSOR_ERROR_DHT:
            {
                ESP_LOGW(TAG, "Panne DHT reçue ! Code: %s",
                         esp_err_to_name((esp_err_t)evt.net.error_code));
                dht_valid = false;
                run_pipeline = true;
                break;
            }

            case EVENT_SENSOR_ERROR_SHT31:
            {
                ESP_LOGE(TAG, "Panne SHT31 reçue ! Code: %s",
                         esp_err_to_name((esp_err_t)evt.net.error_code));
                sht31_valid = false;
                run_pipeline = true;
                break;
            }

            case EVENT_MODE_CHANGE_REQUEST:
            {
                heating_set_mode((heating_mode_t)(int)evt.sensor.temperature);
                heating_save();
                run_pipeline = true;
                break;
            }

            case EVENT_MANUAL_SETPOINT_REQUEST:
            {
                heating_set_manual_target(evt.sensor.temperature);
                heating_save();
                run_pipeline = true;
                break;
            }

            case EVENT_WEATHER_UPDATE:
            {
                weather_data_t *weather_data = (weather_data_t *)evt.payload.payload_ptr;
                if (weather_data)
                {
                    last_known_exterior_temp = weather_data->current.temperature;
                    ESP_LOGI(TAG, "Nouvelle température extérieure: %.1f°C",
                             last_known_exterior_temp);
                }
                else
                {
                    ESP_LOGW(TAG, "EVENT_WEATHER_UPDATE reçu sans payload valide");
                }
                run_pipeline = true;
                break;
            }

            case EVENT_WEATHER_HOURLY:
            {
                ESP_LOGI(TAG, "Données météo horaires reçues: temp=%.1f°C, hum=%.1f%%, code=%d",
                         evt.weather_hourly.temperature,
                         evt.weather_hourly.humidity,
                         evt.weather_hourly.weather_code);
                thermostat_update_hourly_weather(
                    evt.weather_hourly.temperature,
                    evt.weather_hourly.humidity,
                    evt.weather_hourly.weather_code);
                break;
            }

            case EVENT_NET_TIME_SYNCED:
            {
                const char *source = "SYNC";
                float temp = sensor_arbitrate_current_temperature(&source);
                if (!isnan(temp))
                {
                    perform_thermal_regulation(temp, source);
                }
                break;
            }

            default:
            {
                ESP_LOGW(TAG, "Événement non traité : %d", evt.type);
                break;
            }
            }

            if (run_pipeline)
            {
                const char *source = "AUCUN";
                float current_room_temp = sensor_arbitrate_current_temperature(&source);

                if (!isnan(current_room_temp))
                {
                    perform_thermal_regulation(current_room_temp, source);
                }
                else
                {
                    handle_safety_fallback();
                }
            }
        }
    }
}

// Lit la configuration depuis la NVS
esp_err_t thermostat_get_config(thermostat_config_t *config) {
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("thermostat", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return err;

    // Lit les valeurs depuis la NVS
    uint8_t enabled_u8;
    err = nvs_get_u8(nvs_handle, "enabled", &enabled_u8);
    if (err == ESP_OK) config->enabled = enabled_u8;

    int32_t consigne_i32;
    err = nvs_get_i32(nvs_handle, "consigne", &consigne_i32);
    if (err == ESP_OK) config->consigne = (float)consigne_i32 / 100.0;  // Stocke en centièmes de degré

    int32_t mode_i32;
    err = nvs_get_i32(nvs_handle, "mode", &mode_i32);
    if (err == ESP_OK) config->mode = (heating_mode_t)mode_i32;

    int32_t hysteresis_i32;
    err = nvs_get_i32(nvs_handle, "hysteresis", &hysteresis_i32);
    if (err == ESP_OK) config->hysteresis = (float)hysteresis_i32 / 100.0;  // Stocke en centièmes

    int32_t calibration_i32;
    err = nvs_get_i32(nvs_handle, "calibration", &calibration_i32);
    if (err == ESP_OK) config->calibration = (float)calibration_i32 / 100.0;

    uint8_t frost_mode_u8;
    err = nvs_get_u8(nvs_handle, "frost_mode", &frost_mode_u8);
    if (err == ESP_OK) config->frost_mode = frost_mode_u8;

    nvs_close(nvs_handle);
    return ESP_OK;
}

// Écrit la configuration dans la NVS et applique les changements
esp_err_t thermostat_set_config(const thermostat_config_t *config) {
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("thermostat", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    // Écrit les valeurs dans la NVS
    nvs_set_u8(nvs_handle, "enabled", config->enabled ? 1 : 0);
    nvs_set_i32(nvs_handle, "consigne", (int32_t)(config->consigne * 100));  // Stocke en centièmes
    nvs_set_i32(nvs_handle, "mode", (int32_t)config->mode);
    nvs_set_i32(nvs_handle, "hysteresis", (int32_t)(config->hysteresis * 100));
    nvs_set_i32(nvs_handle, "calibration", (int32_t)(config->calibration * 100));
    nvs_set_u8(nvs_handle, "frost_mode", config->frost_mode ? 1 : 0);

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    // Applique les changements au système
    heating_set_manual_target(config->consigne);
    heating_set_mode(config->mode);
    heating_set_hysteresis(config->hysteresis);
    // ... (autres applications)

    return ESP_OK;
}

void thermostat_get_mode_status_str(char *buf, size_t len)
{
    thermostat_config_t cfg;

    if (thermostat_get_config(&cfg) != ESP_OK) {
        snprintf(buf, len, "UNKNOWN");
        return;
    }

    switch (cfg.mode) {
        case HEATING_MODE_AUTO:
            snprintf(buf, len, "AUTO");
            break;

        case HEATING_MODE_MANUAL:
            snprintf(buf, len, "MANUAL");
            break;

        case HEATING_MODE_ABSENT:
            snprintf(buf, len, "ABSENT");
            break;

        case HEATING_MODE_HORS_GEL:
            snprintf(buf, len, "HORS-GEL");
            break;

        default:
            snprintf(buf, len, "UNKNOWN");
            break;
    }
}

