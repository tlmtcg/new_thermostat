#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_task_wdt.h"
#include "weather.h"
#include "alert_manager.h"
#include "config_runtime.h"
#include "weather_store.h"
#include "thermostat.h"
#include "time_utils.h"

// Définition de MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define OPEN_WEATHER_MAP_KEY "70a685fb0de842b5f2ec972448504339"

// --- Constants ---
static const char *TAG = "WEATHER";
#define MAX_HTTP_RECV_BUFFER 25600 // 25 Ko
#define OPEN_METEO_URL "https://api.open-meteo.com/v1/forecast"
#define YR_NO_URL "https://api.met.no/weatherapi/locationforecast/2.0/compact"
#define JEEDOM_URL "http://192.168.0.21/core/api/jeeApi.php?apikey=d8RaIZcJA0iAaUkQGMVyLhk0rAZq2nGl&type=cmd&id=16109"

// --- Global Variables ---
static char *response_data = NULL;
static int weather_response_len = 0;
weather_data_t g_weather_data = {0};
esp_err_t jeedom_temp_update(weather_data_t *data);
weather_data_t latest_weather = {0};

// --- Function Prototypes ---
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);
static esp_err_t http_get_to_buffer(const char *url, int timeout_ms);
static esp_err_t parse_open_meteo_json(cJSON *root, weather_data_t *data);
static esp_err_t weather_update_fallback(weather_data_t *data);

static char *weather_to_json(const weather_data_t *data);
static esp_err_t parse_owm_forecast(cJSON *root, weather_data_t *data);
static esp_err_t parse_owm_current(cJSON *root, weather_data_t *data);
static void interpolate_48h_safe(float *src_temp, float *src_hum, int *src_code,
                                 int src_count,
                                 float *dst_temp, float *dst_hum, int *dst_code);

static int owm_to_openmeteo(int owm);

// --- Helper: Weather Description ---
const char *get_weather_description(int code)
{
    switch (code)
    {
    case 0:
        return "Ciel clair ☀️";
    case 1:
        return "Peu nuageux 🌤️";
    case 2:
        return "Partiellement nuageux ⛅";
    case 3:
        return "Couvert ☁️";
    case 45:
    case 48:
        return "Brouillard 🌫️";
    case 51:
    case 53:
    case 55:
        return "Bruine 🌦️";
    case 61:
    case 63:
    case 65:
        return "Pluie 🌧️";
    case 71:
    case 73:
    case 75:
        return "Neige ❄️";
    case 80:
    case 81:
    case 82:
        return "Averses 🌦️";
    case 95:
    case 96:
    case 99:
        return "Orage ⛈️";
    default:
        return "Inconnu 🤷";
    }
}

// --- HTTP Event Handler ---
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA)
    {
        if (!response_data)
        {
            response_data = malloc(evt->data_len + 1);
            if (!response_data)
            {
                ESP_LOGE(TAG, "Failed to allocate initial buffer");
                return ESP_ERR_NO_MEM;
            }
            weather_response_len = 0;
        }
        else if (weather_response_len + evt->data_len >= MAX_HTTP_RECV_BUFFER)
        {
            ESP_LOGE(TAG, "Buffer overflow");
            return ESP_FAIL;
        }

        char *new_data = realloc(response_data, weather_response_len + evt->data_len + 1);
        if (!new_data)
        {
            ESP_LOGE(TAG, "Failed to reallocate buffer");
            return ESP_ERR_NO_MEM;
        }

        response_data = new_data;
        memcpy(response_data + weather_response_len, evt->data, evt->data_len);
        weather_response_len += evt->data_len;
        response_data[weather_response_len] = '\0';
    }
    return ESP_OK;
}

// --- HTTP GET Request ---
static esp_err_t http_get_to_buffer(const char *url, int timeout_ms)
{
    if (response_data)
    {
        free(response_data);
        response_data = NULL;
    }
    weather_response_len = 0;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = 8192,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32Weather/1.0");
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || !response_data || weather_response_len == 0)
    {
        ESP_LOGE(TAG, "HTTP request failed or empty response");
        return ESP_FAIL;
    }

    // ESP_LOGI(TAG, "HTTP Response (first 200 chars): %.*s", MIN(200, weather_response_len), response_data);
    return ESP_OK;
}

/**
 * @brief Parse une réponse JSON de l'API Open-Meteo et remplit la structure weather_data_t.
 *
 * @param root Pointeur vers l'objet racine cJSON.
 * @param data Pointeur vers la structure weather_data_t à remplir.
 * @return esp_err_t ESP_OK si le parsing réussit, ESP_FAIL sinon.
 */
static esp_err_t parse_open_meteo_json(cJSON *root, weather_data_t *data)
{
    if (!root || !data)
    {
        ESP_LOGE(TAG, "Erreur : root ou data est NULL");
        return ESP_FAIL;
    }

    // --- 1. Parsing du bloc CURRENT ---
    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!current)
    {
        ESP_LOGE(TAG, "Bloc 'current' manquant");
        return ESP_FAIL;
    }

    // Extraction des champs current
    cJSON *time_item = cJSON_GetObjectItemCaseSensitive(current, "time");
    cJSON *temp_item = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    cJSON *hum_item = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    cJSON *press_item = cJSON_GetObjectItemCaseSensitive(current, "surface_pressure");

    if (cJSON_IsString(time_item) && time_item->valuestring != NULL)
    {
        // Conversion ISO8601 -> epoch
        data->current.timestamp = parse_iso8601_to_epoch(time_item->valuestring);

        if (data->current.timestamp == 0)
        {
            ESP_LOGW(TAG, "Impossible de parser current.time (%s)", time_item->valuestring);
            data->current.timestamp = time(NULL);
        }
    }
    else
    {
        data->current.timestamp = time(NULL);
        ESP_LOGW(TAG, "Champ 'current.time' manquant ou invalide");
    }

    if (temp_item && cJSON_IsNumber(temp_item))
    {
        data->current.temperature = (float)temp_item->valuedouble;
    }
    else
    {
        data->current.temperature = 0.0f;
        ESP_LOGW(TAG, "Champ 'current.temperature_2m' manquant ou invalide");
    }

    if (hum_item && cJSON_IsNumber(hum_item))
    {
        data->current.humidity = (float)hum_item->valuedouble;
    }
    else
    {
        data->current.humidity = 0.0f;
        ESP_LOGW(TAG, "Champ 'current.relative_humidity_2m' manquant ou invalide");
    }

    const char *desc = get_weather_description(data->current.weather_code);

    snprintf(g_thermostat_runtime.desc_ext,
             sizeof(g_thermostat_runtime.desc_ext),
             "%s",
             desc);

    // Mise à jour du thermostat depuis CURRENT
    thermostat_update_outdoor_data(data->current.temperature,
                                   data->current.humidity,
                                   desc);

    if (code_item && cJSON_IsNumber(code_item))
    {
        data->current.weather_code = owm_to_openmeteo(code_item->valueint);
    }
    else
    {
        data->current.weather_code = 0;
        ESP_LOGW(TAG, "Champ 'current.weather_code' manquant ou invalide");
    }

    if (press_item && cJSON_IsNumber(press_item))
    {
        data->current.pressure = (float)press_item->valuedouble;
    }
    else
    {
        data->current.pressure = 0.0f;
        ESP_LOGW(TAG, "Champ 'current.surface_pressure' manquant ou invalide");
    }

    // ESP_LOGI(TAG, "Parsing 'current' réussi : temp=%.1f°C, hum=%.1f%%, code=%d, pres=%.1fhPa",
    //          data->current.temperature, data->current.humidity,
    //          data->current.weather_code, data->current.pressure);

    // --- 2. Parsing du bloc HOURLY (48h) ---
    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    if (!hourly)
    {
        ESP_LOGE(TAG, "Bloc 'hourly' manquant");
        return ESP_FAIL;
    }

    cJSON *t_arr = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    cJSON *h_arr = cJSON_GetObjectItemCaseSensitive(hourly, "relative_humidity_2m");
    cJSON *c_arr = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");

    if (!t_arr || !h_arr || !c_arr)
    {
        ESP_LOGE(TAG, "Un des tableaux hourly est manquant");
        return ESP_FAIL;
    }

    int hourly_size = cJSON_GetArraySize(t_arr);
    for (int i = 0; i < MIN(hourly_size, 48); i++)
    {
        cJSON *t_item = cJSON_GetArrayItem(t_arr, i);
        cJSON *h_item = cJSON_GetArrayItem(h_arr, i);
        cJSON *c_item = cJSON_GetArrayItem(c_arr, i);

        data->forecast_48h_temp[i] = (t_item && cJSON_IsNumber(t_item)) ? (float)t_item->valuedouble : 0.0f;
        data->forecast_48h_hum[i] = (h_item && cJSON_IsNumber(h_item)) ? (float)h_item->valuedouble : 0.0f;
        data->forecast_48h_code[i] = (c_item && cJSON_IsNumber(c_item)) ? c_item->valueint : 0;
    }

    float temp_1h = (float)data->forecast_48h_temp[0];
    float hum_1h = (float)data->forecast_48h_hum[0];
    const char *desc_1h = get_weather_description(data->forecast_48h_code[0]);
    thermostat_update_forecast_data(temp_1h, hum_1h, desc_1h);

    // --- Parsing du bloc DAILY ---
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (daily)
    {
        cJSON *daily_time = cJSON_GetObjectItemCaseSensitive(daily, "time");
        cJSON *daily_temp_max = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        cJSON *daily_code = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");

        if (daily_time && daily_temp_max && daily_code)
        {
            int daily_size = cJSON_GetArraySize(daily_time);
            for (int i = 0; i < MIN(daily_size, 7); i++)
            {
                cJSON *time_item = cJSON_GetArrayItem(daily_time, i);
                cJSON *temp_item = cJSON_GetArrayItem(daily_temp_max, i);
                cJSON *code_item = cJSON_GetArrayItem(daily_code, i);

                // Conversion de la chaîne ISO 8601 en timestamp Unix
                if (time_item && cJSON_IsString(time_item))
                {
                    struct tm tm = {0};
                    if (strptime(time_item->valuestring, "%Y-%m-%d", &tm))
                    {
                        data->forecast_7j[i].timestamp = mktime(&tm);
                    }
                    else
                    {
                        data->forecast_7j[i].timestamp = 0;
                        ESP_LOGW(TAG, "Échec de la conversion de la date : %s", time_item->valuestring);
                    }
                }
                else
                {
                    data->forecast_7j[i].timestamp = 0;
                }

                // Remplissage des autres champs
                if (temp_item && cJSON_IsNumber(temp_item))
                {
                    data->forecast_7j[i].temperature = (float)temp_item->valuedouble;
                }
                else
                {
                    data->forecast_7j[i].temperature = 0.0f;
                }

                if (code_item && cJSON_IsNumber(code_item))
                {
                    data->forecast_7j[i].weather_code = code_item->valueint;
                }
                else
                {
                    data->forecast_7j[i].weather_code = 0;
                }

                // ESP_LOGI(TAG, "Daily[%d] : date=%s, timestamp=%ld, temp=%.1f°C, code=%d",
                //          i, time_item->valuestring, data->forecast_7j[i].timestamp,
                //          data->forecast_7j[i].temperature, data->forecast_7j[i].weather_code);
            }
        }
    }

    // Synchronisation avec g_weather_data
    memcpy(&g_weather_data, data, sizeof(weather_data_t));
    return ESP_OK;
}

// --- Weather Update (Main Function) ---
esp_err_t weather_update(weather_data_t *data)
{
    if (!data)
        return ESP_ERR_INVALID_ARG;
    memset(data, 0, sizeof(weather_data_t));

    // --- Try Open-Meteo ---
    char url[512];
    snprintf(url, sizeof(url),
             "%s?latitude=%.5f&longitude=%.5f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,surface_pressure"
             "&hourly=temperature_2m,relative_humidity_2m,weather_code&forecast_hours=48"
             "&daily=weather_code,temperature_2m_max,relative_humidity_2m_max"
             "&timezone=auto",
             OPEN_METEO_URL, g_cfg.weather_lat, g_cfg.weather_lon);

    if (http_get_to_buffer(url, 20000) == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        // ESP_LOGD(TAG, "Open-Meteo Response: %s", response_data);
        cJSON *root = cJSON_ParseWithLength(response_data, weather_response_len);
        vTaskDelay(pdMS_TO_TICKS(5));
        if (root)
        {
            if (parse_open_meteo_json(root, data) == ESP_OK)
            {
                cJSON_Delete(root);
                goto success;
            }
            cJSON_Delete(root);
        }
    }

    // --- Fallback: OpenWeatherMap ---
    free(response_data);
    response_data = NULL;
    weather_response_len = 0;

    ESP_LOGW(TAG, "Falling back to OpenWeatherMap");

    // -------------------------
    // 1) CURRENT WEATHER
    // -------------------------
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/weather?lat=%.4f&lon=%.4f&appid=%s&units=metric",
             g_cfg.weather_lat, g_cfg.weather_lon, OPEN_WEATHER_MAP_KEY);

    if (http_get_to_buffer(url, 20000) != ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGE(TAG, "OWM current: HTTP error");
        goto fail;
    }

    cJSON *root_current = cJSON_ParseWithLength(response_data, weather_response_len);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!root_current)
    {
        ESP_LOGE(TAG, "OWM current: JSON parse error");
        goto fail;
    }

    if (parse_owm_current(root_current, data) != ESP_OK)
    {
        ESP_LOGE(TAG, "OWM current: parsing failed");
        cJSON_Delete(root_current);
        goto fail;
    }
    cJSON_Delete(root_current);

    vTaskDelay(pdMS_TO_TICKS(50)); // évite WDT

    // -------------------------
    // 2) FORECAST 48H + 7J (approx)
    // -------------------------
    free(response_data);
    response_data = NULL;
    weather_response_len = 0;

    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/forecast?lat=%.4f&lon=%.4f&appid=%s&units=metric",
             g_cfg.weather_lat, g_cfg.weather_lon, OPEN_WEATHER_MAP_KEY);

    if (http_get_to_buffer(url, 30000) != ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGE(TAG, "OWM forecast: HTTP error");
        goto fail;
    }

    cJSON *root_forecast = cJSON_ParseWithLength(response_data, weather_response_len);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!root_forecast)
    {
        ESP_LOGE(TAG, "OWM forecast: JSON parse error");
        goto fail;
    }

    if (parse_owm_forecast(root_forecast, data) != ESP_OK)
    {
        ESP_LOGE(TAG, "OWM forecast: parsing failed");
        cJSON_Delete(root_forecast);
        goto fail;
    }

    cJSON_Delete(root_forecast);
    goto success;

fail:
    ESP_LOGE(TAG, "OpenWeatherMap fallback failed");
    return ESP_FAIL;

    // --- Fallback: Jeedom ---
    free(response_data);
    response_data = NULL;
    weather_response_len = 0;

    ESP_LOGW(TAG, "Falling back to Jeedom");
    if (http_get_to_buffer(JEEDOM_URL, 3000) == ESP_OK && response_data && response_data[0] != '<')
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        data->current.jee_temp = atof(response_data);
        data->current.weather_code = 0; // Default to clear sky
        // Mettre à jour latest_weather aussi
        latest_weather.current.jee_temp = data->current.jee_temp;
        goto success;
    }

    // --- All fallbacks failed ---
    alert_add("Absence METEO");
    free(response_data);
    response_data = NULL;
    weather_response_len = 0;
    return ESP_FAIL;

success:
    // Copy data to global variable
    memcpy(&g_weather_data, data, sizeof(weather_data_t));
    weather_store_set_all(data);

    // Free response data
    free(response_data);
    response_data = NULL;
    weather_response_len = 0;

    // Sauvegarde dans le magasin de stockage local
    weather_store_set_all(data);

    // Mettre aussi à jour latest_weather pour les getters/UI
    latest_weather = *data;

    return ESP_OK;
}

// --- Generate JSON from weather_data_t ---
static char *weather_to_json(const weather_data_t *data)
{
    if (!data)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    // --- Section "now" ---
    cJSON *now = cJSON_AddObjectToObject(root, "now");
    if (!now)
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddNumberToObject(now, "temp", data->current.temperature);
    cJSON_AddNumberToObject(now, "hum", data->current.humidity);
    cJSON_AddStringToObject(now, "desc", get_weather_description(data->current.weather_code));
    cJSON_AddNumberToObject(now, "time", (double)data->current.timestamp);
    cJSON_AddNumberToObject(now, "jee_temp", data->current.jee_temp);
    cJSON_AddNumberToObject(now, "pres", data->current.pressure);

    // --- Section "f48_temps" (tableau de 48 floats) ---
    cJSON *f48_temps = cJSON_AddArrayToObject(root, "f48_temps");
    if (!f48_temps)
    {
        cJSON_Delete(root);
        return NULL;
    }
    for (int i = 0; i < 48; i++)
    {
        cJSON *num = cJSON_CreateNumber(data->forecast_48h_temp[i]);
        if (!num)
        {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(f48_temps, num);
    }

    // --- Section "f48_hums" (tableau de 48 floats) ---
    cJSON *f48_hums = cJSON_AddArrayToObject(root, "f48_hums");
    if (!f48_hums)
    {
        cJSON_Delete(root);
        return NULL;
    }
    for (int i = 0; i < 48; i++)
    {
        cJSON *num = cJSON_CreateNumber(data->forecast_48h_hum[i]);
        if (!num)
        {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(f48_hums, num);
    }

    g_thermostat_runtime.temp_ext = data->current.temperature;
    g_thermostat_runtime.humidity_ext = data->current.humidity;
    g_thermostat_runtime.temp_forecast_1h = data->forecast_48h_temp[1];

    // --- Section "f7j" (7 jours) ---
    cJSON *f7j = cJSON_AddArrayToObject(root, "f7j");
    if (!f7j)
    {
        cJSON_Delete(root);
        return NULL;
    }

    for (int i = 0; i < 7; i++)
    {
        cJSON *day_obj = cJSON_CreateObject();
        if (!day_obj)
        {
            cJSON_Delete(root);
            return NULL;
        }

        // Ajoute le jour de la semaine (ex: "jeu. 5")
        char day_str[16];
        if (data->forecast_7j[i].timestamp != 0)
        {
            // Conversion explicite de long en time_t pour localtime
            time_t temp_time = (time_t)data->forecast_7j[i].timestamp;
            struct tm *tm = localtime(&temp_time);
            if (tm)
            {
                strftime(day_str, sizeof(day_str), "%a. %d", tm); // "jeu. 5"
            }
            else
            {
                strcpy(day_str, "Inconnu");
            }
        }
        else
        {
            strcpy(day_str, "Inconnu");
        }
        cJSON_AddStringToObject(day_obj, "day", day_str);

        // Ajoute les autres champs
        cJSON_AddNumberToObject(day_obj, "temp", data->forecast_7j[i].temperature);
        cJSON_AddStringToObject(day_obj, "desc", get_weather_description(data->forecast_7j[i].weather_code));
        cJSON_AddNumberToObject(day_obj, "time", (double)data->forecast_7j[i].timestamp);

        cJSON_AddItemToArray(f7j, day_obj);
    }

    // Génère la chaîne JSON
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

// --- Public Functions ---
char *weather_generate_json(void)
{
    return weather_to_json(&g_weather_data);
}

float temperature_get_outdoor(void)
{
    return g_weather_data.current.jee_temp;
}

bool temperature_get_valid(void)
{
    return g_weather_data.current.meteo_valid;
}

void temperature_set_valid(bool result)
{
    g_weather_data.current.meteo_valid = result;
}

float weather_get_forecast_temp(int hours)
{
    if (hours < 0 || hours >= 48)
        return 0.0f;
    return g_weather_data.forecast_48h_temp[hours];
}

float weather_get_forecast_humidity(int hours)
{
    if (hours < 0 || hours >= 48)
        return 0.0f;
    return g_weather_data.forecast_48h_hum[hours];
}

int weather_get_current_code(void)
{
    return g_weather_data.current.weather_code;
}

int weather_get_forecast_code(int hours)
{
    if (hours < 0 || hours >= 48)
        return g_weather_data.current.weather_code;
    return g_weather_data.forecast_48h_code[hours];
}

esp_err_t weather_get_temp_in_x_hours(const weather_data_t *data, int hours_from_now, float *out_temp)
{
    if (!data || !out_temp || hours_from_now < 0 || hours_from_now >= 48)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_temp = data->forecast_48h_temp[hours_from_now];
    return ESP_OK;
}

// --- Task for Manual Update ---
void weather_update_task_manually(void *arg)
{
    esp_task_wdt_reset();
    weather_data_t *tmp = calloc(1, sizeof(weather_data_t));
    if (tmp)
    {
        if (weather_update(tmp) == ESP_OK)
        {
            // ESP_LOGI(TAG, "Manual weather update successful");
        }
        else
        {
            ESP_LOGE(TAG, "Manual weather update failed");
        }
        free(tmp);
    }
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

// --- Geocoding ---
esp_err_t weather_geocode_city(const char *city, double *lat, double *lon)
{
    if (!city || !lat || !lon)
        return ESP_ERR_INVALID_ARG;

    char encoded_city[64] = {0};
    int j = 0;
    for (int i = 0; city[i] != '\0' && j < sizeof(encoded_city) - 4; i++)
    {
        if (city[i] == ' ')
        {
            strcat(encoded_city, "%20");
            j += 3;
        }
        else
        {
            encoded_city[j++] = city[i];
        }
    }

    char url[256];
    snprintf(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=fr&format=json", encoded_city);

    if (http_get_to_buffer(url, 20000) != ESP_OK || !response_data)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        free(response_data);
        response_data = NULL;
        weather_response_len = 0;
        return ESP_FAIL;
    }

    esp_task_wdt_reset();
    cJSON *root = cJSON_Parse(response_data);
    if (!root)
    {
        free(response_data);
        response_data = NULL;
        weather_response_len = 0;
        return ESP_FAIL;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0)
    {
        cJSON_Delete(root);
        free(response_data);
        response_data = NULL;
        weather_response_len = 0;
        return ESP_FAIL;
    }

    cJSON *item = cJSON_GetArrayItem(results, 0);
    if (item)
    {
        cJSON *lat_item = cJSON_GetObjectItem(item, "latitude");
        cJSON *lon_item = cJSON_GetObjectItem(item, "longitude");
        if (lat_item)
            *lat = lat_item->valuedouble;
        if (lon_item)
            *lon = lon_item->valuedouble;
    }

    cJSON_Delete(root);
    free(response_data);
    response_data = NULL;
    weather_response_len = 0;
    esp_task_wdt_reset();
    return ESP_OK;
}

static esp_err_t parse_owm_current(cJSON *root, weather_data_t *data)
{
    if (!root || !data)
        return ESP_FAIL;

    cJSON *main = cJSON_GetObjectItem(root, "main");
    if (!main)
    {
        ESP_LOGE(TAG, "OWM current: 'main' missing");
        return ESP_FAIL;
    }

    cJSON *temp = cJSON_GetObjectItem(main, "temp");
    cJSON *hum = cJSON_GetObjectItem(main, "humidity");
    cJSON *pres = cJSON_GetObjectItem(main, "pressure");

    if (cJSON_IsNumber(temp))
        data->current.temperature = temp->valuedouble;

    if (cJSON_IsNumber(hum))
        data->current.humidity = hum->valuedouble;

    if (cJSON_IsNumber(pres))
        data->current.pressure = pres->valuedouble;

    // Weather code
    cJSON *weather = cJSON_GetObjectItem(root, "weather");
    if (cJSON_IsArray(weather))
    {
        cJSON *w0 = cJSON_GetArrayItem(weather, 0);
        if (w0)
        {
            cJSON *id = cJSON_GetObjectItem(w0, "id");
            if (cJSON_IsNumber(id))
            {
                int owm_code = id->valueint;
                data->current.weather_code = owm_to_openmeteo(owm_code);
            }
        }
    }

    data->current.timestamp = time(NULL);
    data->current.meteo_valid = true;

    return ESP_OK;
}

static esp_err_t parse_owm_forecast(cJSON *root, weather_data_t *data)
{
    if (!root || !data)
        return ESP_FAIL;

    cJSON *list = cJSON_GetObjectItem(root, "list");
    if (!cJSON_IsArray(list))
        return ESP_FAIL;

    int count = cJSON_GetArraySize(list);
    int limit48 = MIN(count, 16);

    float temp3h[16] = {0};
    float hum3h[16] = {0};
    int code3h[16] = {0};

    // -------------------------
    // Extraction des points OWM (3h)
    // -------------------------
    for (int i = 0; i < limit48; i++)
    {
        cJSON *entry = cJSON_GetArrayItem(list, i);
        if (!entry)
            continue;

        cJSON *main = cJSON_GetObjectItem(entry, "main");
        if (!main)
            continue;

        cJSON *t = cJSON_GetObjectItem(main, "temp");
        cJSON *h = cJSON_GetObjectItem(main, "humidity");

        if (cJSON_IsNumber(t))
            temp3h[i] = t->valuedouble;
        if (cJSON_IsNumber(h))
            hum3h[i] = h->valuedouble;

        cJSON *weather = cJSON_GetObjectItem(entry, "weather");
        if (cJSON_IsArray(weather))
        {
            cJSON *w0 = cJSON_GetArrayItem(weather, 0);
            if (w0)
            {
                cJSON *id = cJSON_GetObjectItem(w0, "id");
                if (cJSON_IsNumber(id))
                    code3h[i] = owm_to_openmeteo(id->valueint);
            }
        }
    }

    // -------------------------
    // Interpolation → 48 points horaires
    // -------------------------
    interpolate_48h_safe(temp3h, hum3h, code3h, limit48,
                         data->forecast_48h_temp,
                         data->forecast_48h_hum,
                         data->forecast_48h_code);

    vTaskDelay(pdMS_TO_TICKS(2));
    data->forecast_48h.temperature = data->forecast_48h_temp[47];
    data->forecast_48h.humidity = data->forecast_48h_hum[47];
    data->forecast_48h.timestamp = data->current.timestamp + (47 * 3600);

    // -------------------------
    // Prévisions 7 jours (point le plus proche de midi)
    // -------------------------
    time_t now = data->current.timestamp;
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    for (int d = 0; d < 7; d++)
    {
        struct tm target = tm_now;
        target.tm_mday += (d + 1);
        target.tm_hour = 12;
        target.tm_min = 0;
        target.tm_sec = 0;

        time_t target_ts = mktime(&target);

        int best_idx = 0;
        long best_diff = LONG_MAX;

        for (int i = 0; i < count; i++)
        {
            cJSON *entry = cJSON_GetArrayItem(list, i);
            if (!entry)
                continue;

            cJSON *dt = cJSON_GetObjectItem(entry, "dt");
            if (!cJSON_IsNumber(dt))
                continue;

            long ts = dt->valueint;
            long long ts_ll = (long long)ts;
            long long target_ll = (long long)target_ts;
            long long diff = llabs(ts_ll - target_ll);

            if (diff < best_diff)
            {
                best_diff = diff;
                best_idx = i;
            }
            if ((i & 3) == 0)
                vTaskDelay(1); // toutes les 4 itérations
        }

        cJSON *entry = cJSON_GetArrayItem(list, best_idx);
        if (!entry)
            continue;

        cJSON *main = cJSON_GetObjectItem(entry, "main");
        if (!main)
            continue;

        cJSON *t = cJSON_GetObjectItem(main, "temp");
        if (cJSON_IsNumber(t))
            data->forecast_7j[d].temperature = t->valuedouble;

        cJSON *weather = cJSON_GetObjectItem(entry, "weather");
        if (cJSON_IsArray(weather))
        {
            cJSON *w0 = cJSON_GetArrayItem(weather, 0);
            if (w0)
            {
                cJSON *id = cJSON_GetObjectItem(w0, "id");
                if (cJSON_IsNumber(id))
                    data->forecast_7j[d].weather_code = owm_to_openmeteo(id->valueint);
            }
        }

        cJSON *dt = cJSON_GetObjectItem(entry, "dt");
        if (cJSON_IsNumber(dt))
            data->forecast_7j[d].timestamp = dt->valueint;
    }

    return ESP_OK;
}

static void interpolate_48h_safe(float *src_temp, float *src_hum, int *src_code,
                                 int src_count,
                                 float *dst_temp, float *dst_hum, int *dst_code)
{
    if (src_count < 2)
    {
        float t = src_temp[0];
        float h = src_hum[0];
        int c = src_code[0];

        for (int i = 0; i < 48; i++)
        {
            dst_temp[i] = t;
            dst_hum[i] = h;
            dst_code[i] = c;
        }
        return;
    }

    int segments = src_count - 1;
    float hours_per_segment = 48.0f / segments;

    int idx = 0;

    for (int s = 0; s < segments; s++)
    {
        float t0 = src_temp[s];
        float t1 = src_temp[s + 1];

        float h0 = src_hum[s];
        float h1 = src_hum[s + 1];

        int c0 = src_code[s];

        int steps = (int)hours_per_segment;

        for (int h = 0; h < steps && idx < 48; h++)
        {
            float k = (float)h / (float)steps;

            dst_temp[idx] = t0 + (t1 - t0) * k;
            dst_hum[idx] = h0 + (h1 - h0) * k;
            dst_code[idx] = c0;

            idx++;
        }
    }

    float t = src_temp[src_count - 1];
    float h = src_hum[src_count - 1];
    int c = src_code[src_count - 1];

    while (idx < 48)
    {
        dst_temp[idx] = t;
        dst_hum[idx] = h;
        dst_code[idx] = c;
        idx++;
    }
}

static int owm_to_openmeteo(int owm)
{
    // Thunderstorm
    if (owm >= 200 && owm <= 232)
        return 95;

    // Drizzle
    if (owm >= 300 && owm <= 321)
        return 51;

    // Rain
    if (owm >= 500 && owm <= 504)
        return 61;
    if (owm == 511)
        return 67; // pluie verglaçante
    if (owm >= 520 && owm <= 531)
        return 80;

    // Snow
    if (owm >= 600 && owm <= 602)
        return 71;
    if (owm >= 611 && owm <= 616)
        return 73;
    if (owm >= 620 && owm <= 622)
        return 75;

    // Atmosphere
    if (owm == 701)
        return 45;
    if (owm == 711)
        return 45;
    if (owm == 721)
        return 45;
    if (owm == 731 || owm == 751 || owm == 761)
        return 45;
    if (owm == 741)
        return 45;
    if (owm == 762)
        return 45;
    if (owm == 771)
        return 80;
    if (owm == 781)
        return 99;

    // Clear
    if (owm == 800)
        return 0;

    // Clouds
    if (owm == 801)
        return 1;
    if (owm == 802)
        return 2;
    if (owm == 803)
        return 3;
    if (owm == 804)
        return 3;

    return 3; // fallback: couvert
}
