#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_task_wdt.h"
#include "weather.h"
#include "event_bus.h" // Pour event_t, EVENT_WEATHER_HOURLY, etc.
#include "weather_store.h"

// Définition de MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))

// Clé API OpenWeatherMap (à définir dans menuconfig)
#define OPEN_WEATHER_MAP_KEY CONFIG_OPEN_WEATHER_MAP_KEY

// --- Constants ---
static const char *TAG = "WEATHER";
#define MAX_HTTP_RECV_BUFFER 25600 // 25 Ko
#define OPEN_METEO_URL "https://api.open-meteo.com/v1/forecast"

// --- Global Variables ---
// Buffer statique pour éviter malloc/free
static char response_buffer[MAX_HTTP_RECV_BUFFER + 1];
static size_t response_len = 0;
static weather_data_t s_cached_weather_data;
static time_t s_last_weather_update = 0;
static const int WEATHER_CACHE_DURATION = 3600; // 1 heure (en secondes)

// Données météo globales (à remplacer par s_weather_data si tu préfères)
weather_data_t g_weather_data = {0};

// Variable globale pour la queue d'événements WiFi
static QueueHandle_t s_wifi_event_queue = NULL;

// Mutex pour protéger g_weather_data
static SemaphoreHandle_t g_weather_mutex = NULL;

// --- Function Prototypes ---
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);
static esp_err_t http_get_to_buffer(const char *url, int timeout_ms);
static esp_err_t parse_open_meteo_json(cJSON *root, weather_data_t *data);
static esp_err_t parse_owm_forecast(cJSON *root, weather_data_t *data);
static esp_err_t parse_owm_current(cJSON *root, weather_data_t *data);
static void interpolate_48h_safe(float *src_temp, float *src_hum, int *src_code,
                                 int src_count,
                                 float *dst_temp, float *dst_hum, int *dst_code);
static int owm_to_openmeteo(int owm);
static time_t parse_iso8601_to_epoch(const char *iso8601_str);

// Fonction pour vérifier si le cache est valide
static bool weather_cache_is_valid(void)
{
    time_t now = time(NULL);
    return (now - s_last_weather_update) < WEATHER_CACHE_DURATION;
}

// Fonction pour mettre à jour le cache
static void weather_cache_update(const weather_data_t *data)
{
    if (data)
    {
        memcpy(&s_cached_weather_data, data, sizeof(weather_data_t));
        s_last_weather_update = time(NULL);
    }
}

// --- Helper: Parse ISO8601 to Epoch ---
static time_t parse_iso8601_to_epoch(const char *iso8601_str)
{
    if (!iso8601_str)
        return -1;

    struct tm tm = {0};
    int year, month, day, hour, min, sec = 0;

    // Exemple de formats supportés :
    // "2026-06-14T18:30" (sans secondes)
    // "2026-06-14T18:30:00" (avec secondes)
    // "2026-06-14T18:30:00Z" (avec Z pour UTC)

    // Essaye de parser avec secondes et Z
    if (sscanf(iso8601_str, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        return timegm(&tm);
    }
    // Essaye de parser avec secondes (sans Z)
    else if (sscanf(iso8601_str, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        return timegm(&tm);
    }
    // Essaye de parser sans secondes (format Open-Meteo)
    else if (sscanf(iso8601_str, "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &min) == 5)
    {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = 0; // Secondes à 0
        return timegm(&tm);
    }

    // Si aucun format ne correspond
    ESP_LOGW(TAG, "Format de timestamp non reconnu : %s", iso8601_str);
    return -1;
}

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

// Fonction pour initialiser l'abonnement aux événements WiFi
static void weather_init_event_bus(void)
{
    event_type_t filter[] = {EVENT_WIFI_STATUS};
    s_wifi_event_queue = event_bus_subscribe("weather_service", filter, 1);
    assert(s_wifi_event_queue != NULL);
}

// Tâche pour écouter les événements WiFi
static void weather_wifi_task(void *arg)
{
    (void)arg;
    event_t evt;

    while (1)
    {
        if (event_bus_receive(s_wifi_event_queue, &evt, portMAX_DELAY))
        {
            if (evt.type == EVENT_WIFI_STATUS && evt.net.bool_value)
            {
                ESP_LOGI(TAG, "WiFi connecté, lancement de la mise à jour météo...");

                // Attend 3 secondes pour stabiliser le réseau
                vTaskDelay(pdMS_TO_TICKS(3000));

                // Lance la mise à jour météo
                weather_data_t weather_data;
                if (weather_update(&weather_data) == ESP_OK)
                {
                    ESP_LOGI(TAG, "Mise à jour météo réussie après connexion WiFi");
                }
                else
                {
                    ESP_LOGE(TAG, "Échec de la mise à jour météo");
                }
            }
        }
    }
}

void weather_init(void)
{
    g_weather_mutex = xSemaphoreCreateMutex();
    assert(g_weather_mutex != NULL);

    // Initialise l'abonnement aux événements WiFi
    weather_init_event_bus();

    // Crée la tâche pour écouter les événements WiFi
    xTaskCreate(
        weather_wifi_task,
        "weather_wifi_task",
        8192,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL);
}

// --- HTTP Event Handler ---
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA)
    {
        if (response_len + evt->data_len >= MAX_HTTP_RECV_BUFFER)
        {
            ESP_LOGE(TAG, "Buffer overflow (max: %d, current: %zu, new: %d)",
                     MAX_HTTP_RECV_BUFFER, response_len, evt->data_len);
            return ESP_FAIL;
        }
        memcpy(response_buffer + response_len, evt->data, evt->data_len);
        response_len += evt->data_len;
        response_buffer[response_len] = '\0'; // Terminaison null
    }
    return ESP_OK;
}

// --- HTTP GET Request ---
static esp_err_t http_get_to_buffer(const char *url, int timeout_ms)
{
    // Réinitialise le buffer et sa taille
    response_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = 8192, // Taille du buffer interne ESP-HTTP-Client
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

    if (err != ESP_OK || response_len == 0)
    {
        ESP_LOGE(TAG, "HTTP request failed or empty response");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// --- Parsing Functions ---
static esp_err_t parse_owm_current(cJSON *root, weather_data_t *data)
{
    if (!root || !data)
    {
        ESP_LOGE(TAG, "root ou data est NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Initialisation des valeurs par défaut
    data->current.temperature = 0.0f;
    data->current.humidity = 0.0f;
    data->current.pressure = 0.0f;
    data->current.weather_code = 0;
    data->current.timestamp = 0;
    data->current.meteo_valid = false;

    // Parsing de "main"
    cJSON *main = cJSON_GetObjectItem(root, "main");
    if (!main)
    {
        ESP_LOGE(TAG, "OWM current: 'main' manquant");
        return ESP_FAIL;
    }

    cJSON *temp = cJSON_GetObjectItem(main, "temp");
    cJSON *hum = cJSON_GetObjectItem(main, "humidity");
    cJSON *pres = cJSON_GetObjectItem(main, "pressure");

    if (cJSON_IsNumber(temp))
    {
        data->current.temperature = (float)temp->valuedouble;
    }
    else
    {
        ESP_LOGW(TAG, "OWM current: 'temp' manquant ou invalide");
    }

    if (cJSON_IsNumber(hum))
    {
        data->current.humidity = (float)hum->valuedouble;
    }
    else
    {
        ESP_LOGW(TAG, "OWM current: 'humidity' manquant ou invalide");
    }

    if (cJSON_IsNumber(pres))
    {
        data->current.pressure = (float)pres->valuedouble;
    }
    else
    {
        ESP_LOGW(TAG, "OWM current: 'pressure' manquant ou invalide");
    }

    // Parsing de "weather" (code météo)
    cJSON *weather = cJSON_GetObjectItem(root, "weather");
    if (cJSON_IsArray(weather) && cJSON_GetArraySize(weather) > 0)
    {
        cJSON *w0 = cJSON_GetArrayItem(weather, 0);
        if (w0)
        {
            cJSON *id = cJSON_GetObjectItem(w0, "id");
            if (cJSON_IsNumber(id))
            {
                data->current.weather_code = owm_to_openmeteo(id->valueint);
            }
            else
            {
                ESP_LOGW(TAG, "OWM current: 'weather[0].id' manquant ou invalide");
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "OWM current: 'weather' manquant ou vide");
    }

    // Timestamp (UTC)
    cJSON *dt = cJSON_GetObjectItem(root, "dt");
    if (cJSON_IsNumber(dt))
    {
        data->current.timestamp = (time_t)dt->valueint;
    }
    else
    {
        data->current.timestamp = time(NULL);
        ESP_LOGW(TAG, "OWM current: 'dt' manquant ou invalide, utilisation de time(NULL)");
    }

    // Valide si tous les champs critiques sont présents
    data->current.meteo_valid =
        (data->current.temperature != 0.0f) &&
        (data->current.humidity != 0.0f) &&
        (data->current.weather_code != 0);

    return ESP_OK;
}

static esp_err_t parse_owm_forecast(cJSON *root, weather_data_t *data)
{
    if (!root || !data)
    {
        ESP_LOGE(TAG, "root ou data est NULL");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *list = cJSON_GetObjectItem(root, "list");
    if (!cJSON_IsArray(list))
    {
        ESP_LOGE(TAG, "OWM forecast: 'list' manquant ou invalide");
        return ESP_FAIL;
    }

    int count = cJSON_GetArraySize(list);
    if (count == 0)
    {
        ESP_LOGE(TAG, "OWM forecast: 'list' vide");
        return ESP_FAIL;
    }

    // Tableaux temporaires pour les données OWM (3h)
    float temp3h[count];
    float hum3h[count];
    int code3h[count];
    time_t ts3h[count];

    // Extraction des données OWM
    for (int i = 0; i < count; i++)
    {
        cJSON *entry = cJSON_GetArrayItem(list, i);
        if (!entry)
            continue;

        cJSON *main = cJSON_GetObjectItem(entry, "main");
        cJSON *weather = cJSON_GetObjectItem(entry, "weather");
        cJSON *dt = cJSON_GetObjectItem(entry, "dt");

        if (main)
        {
            cJSON *t = cJSON_GetObjectItem(main, "temp");
            cJSON *h = cJSON_GetObjectItem(main, "humidity");
            temp3h[i] = (t && cJSON_IsNumber(t)) ? (float)t->valuedouble : 0.0f;
            hum3h[i] = (h && cJSON_IsNumber(h)) ? (float)h->valuedouble : 0.0f;
        }
        else
        {
            temp3h[i] = 0.0f;
            hum3h[i] = 0.0f;
        }

        if (weather && cJSON_IsArray(weather) && cJSON_GetArraySize(weather) > 0)
        {
            cJSON *w0 = cJSON_GetArrayItem(weather, 0);
            if (w0)
            {
                cJSON *id = cJSON_GetObjectItem(w0, "id");
                code3h[i] = (id && cJSON_IsNumber(id)) ? owm_to_openmeteo(id->valueint) : 0;
            }
            else
            {
                code3h[i] = 0;
            }
        }
        else
        {
            code3h[i] = 0;
        }

        ts3h[i] = (dt && cJSON_IsNumber(dt)) ? (time_t)dt->valueint : 0;
    }

    // Interpolation vers 48 points horaires
    interpolate_48h_safe(temp3h, hum3h, code3h, count,
                         data->forecast_48h_temp,
                         data->forecast_48h_hum,
                         data->forecast_48h_code);

    // Remplit les données pour la prévision 48h (dernier point)
    if (count > 0)
    {
        data->forecast_48h.temperature = data->forecast_48h_temp[47];
        data->forecast_48h.humidity = data->forecast_48h_hum[47];
        data->forecast_48h.timestamp = data->current.timestamp + (47 * 3600);
    }

    // Prévisions 7 jours (point le plus proche de midi UTC)
    for (int d = 0; d < 7; d++)
    {
        time_t target_ts = data->current.timestamp + (d + 1) * 86400; // Midi UTC le jour suivant
        target_ts += 12 * 3600;                                       // 12h00 UTC

        int best_idx = 0;
        long min_diff = LONG_MAX;

        for (int i = 0; i < count; i++)
        {
            if (ts3h[i] == 0)
                continue;
            long diff = labs((long)ts3h[i] - (long)target_ts);
            if (diff < min_diff)
            {
                min_diff = diff;
                best_idx = i;
            }
        }

        if (min_diff != LONG_MAX)
        {
            data->forecast_7j[d].temperature = temp3h[best_idx];
            data->forecast_7j[d].weather_code = code3h[best_idx];
            data->forecast_7j[d].timestamp = ts3h[best_idx];
        }
        else
        {
            data->forecast_7j[d].temperature = 0.0f;
            data->forecast_7j[d].weather_code = 0;
            data->forecast_7j[d].timestamp = 0;
        }
    }

    return ESP_OK;
}

static void interpolate_48h_safe(float *src_temp, float *src_hum, int *src_code,
                                 int src_count,
                                 float *dst_temp, float *dst_hum, int *dst_code)
{
    if (!src_temp || !src_hum || !src_code || !dst_temp || !dst_hum || !dst_code)
    {
        return;
    }

    if (src_count <= 0)
    {
        for (int i = 0; i < 48; i++)
        {
            dst_temp[i] = 0.0f;
            dst_hum[i] = 0.0f;
            dst_code[i] = 0;
        }
        return;
    }

    if (src_count == 1)
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

    for (int i = 0; i < 48; i++)
    {
        float pos = (float)i / 48.0f * (src_count - 1);
        int seg = (int)pos;
        float k = pos - seg;

        if (seg >= src_count - 1)
        {
            seg = src_count - 2;
            k = 1.0f;
        }

        dst_temp[i] = src_temp[seg] + (src_temp[seg + 1] - src_temp[seg]) * k;
        dst_hum[i] = src_hum[seg] + (src_hum[seg + 1] - src_hum[seg]) * k;
        dst_code[i] = (k < 0.5f) ? src_code[seg] : src_code[seg + 1];
    }
}

static int owm_to_openmeteo(int owm)
{
    // Thunderstorm (Orage)
    if (owm >= 200 && owm <= 202)
        return 95;
    if (owm >= 210 && owm <= 212)
        return 95;
    if (owm >= 221 && owm <= 232)
        return 96;

    // Drizzle (Bruine)
    if (owm >= 300 && owm <= 302)
        return 51;
    if (owm >= 310 && owm <= 314)
        return 53;
    if (owm == 321)
        return 55;

    // Rain (Pluie)
    if (owm >= 500 && owm <= 501)
        return 61;
    if (owm == 502)
        return 63;
    if (owm >= 503 && owm <= 504)
        return 65;
    if (owm == 511)
        return 67;
    if (owm >= 520 && owm <= 531)
        return 80;

    // Snow (Neige)
    if (owm >= 600 && owm <= 602)
        return 71;
    if (owm >= 611 && owm <= 613)
        return 73;
    if (owm >= 615 && owm <= 616)
        return 75;
    if (owm >= 620 && owm <= 622)
        return 77;

    // Atmosphere (Brouillard, etc.)
    if (owm == 701)
        return 45;
    if (owm == 711)
        return 45;
    if (owm == 721)
        return 48;
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

    // Clear (Ciel clair)
    if (owm == 800)
        return 0;

    // Clouds (Nuages)
    if (owm == 801)
        return 1;
    if (owm == 802)
        return 2;
    if (owm >= 803 && owm <= 804)
        return 3;

    return 3; // Fallback: couvert
}

static esp_err_t parse_open_meteo_json(cJSON *root, weather_data_t *data)
{
    if (!root || !data)
    {
        ESP_LOGE(TAG, "root ou data est NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // --- 1. Parsing du bloc CURRENT ---
    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!current)
    {
        ESP_LOGE(TAG, "Bloc 'current' manquant");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Extraction des champs current
    cJSON *time_item = cJSON_GetObjectItemCaseSensitive(current, "time");
    cJSON *temp_item = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    cJSON *hum_item = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    cJSON *press_item = cJSON_GetObjectItemCaseSensitive(current, "surface_pressure");

    // Parsing de current.time
    if (cJSON_IsString(time_item) && time_item->valuestring != NULL)
    {
        data->current.timestamp = parse_iso8601_to_epoch(time_item->valuestring);
        if (data->current.timestamp == -1)
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

    // Parsing de current.temperature_2m
    if (temp_item && cJSON_IsNumber(temp_item))
    {
        float temp = (float)temp_item->valuedouble;
        if (temp >= -50.0f && temp <= 60.0f)
        {
            data->current.temperature = temp;
        }
        else
        {
            ESP_LOGW(TAG, "Température invalide: %.1f°C", temp);
            data->current.temperature = 0.0f;
        }
    }
    else
    {
        data->current.temperature = 0.0f;
        ESP_LOGW(TAG, "Champ 'current.temperature_2m' manquant ou invalide");
    }

    // Parsing de current.relative_humidity_2m
    if (hum_item && cJSON_IsNumber(hum_item))
    {
        float hum = (float)hum_item->valuedouble;
        if (hum >= 0.0f && hum <= 100.0f)
        {
            data->current.humidity = hum;
        }
        else
        {
            ESP_LOGW(TAG, "Humidité invalide: %.1f%%", hum);
            data->current.humidity = 0.0f;
        }
    }
    else
    {
        data->current.humidity = 0.0f;
        ESP_LOGW(TAG, "Champ 'current.relative_humidity_2m' manquant ou invalide");
    }

    // Parsing de current.weather_code
    if (code_item && cJSON_IsNumber(code_item))
    {
        data->current.weather_code = owm_to_openmeteo(code_item->valueint);
    }
    else
    {
        data->current.weather_code = 0;
        ESP_LOGW(TAG, "Champ 'current.weather_code' manquant ou invalide");
    }

    // Parsing de current.surface_pressure
    if (press_item && cJSON_IsNumber(press_item))
    {
        float press = (float)press_item->valuedouble;
        if (press >= 800.0f && press <= 1100.0f)
        {
            data->current.pressure = press;
        }
        else
        {
            ESP_LOGW(TAG, "Pression invalide: %.1fhPa", press);
            data->current.pressure = 0.0f;
        }
    }
    else
    {
        data->current.pressure = 0.0f;
        ESP_LOGW(TAG, "Champ 'current.surface_pressure' manquant ou invalide");
    }

    ESP_LOGI(TAG, "Parsing 'current' réussi : temp=%.1f°C, hum=%.1f%%, code=%d, pres=%.1fhPa",
             data->current.temperature, data->current.humidity,
             data->current.weather_code, data->current.pressure);

    // --- 2. Parsing du bloc HOURLY (48h) ---
    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    if (!hourly)
    {
        ESP_LOGE(TAG, "Bloc 'hourly' manquant");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *t_arr = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    cJSON *h_arr = cJSON_GetObjectItemCaseSensitive(hourly, "relative_humidity_2m");
    cJSON *c_arr = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");

    if (!t_arr || !h_arr || !c_arr)
    {
        ESP_LOGE(TAG, "Un des tableaux hourly est manquant");
        return ESP_ERR_INVALID_RESPONSE;
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

    // --- 3. Parsing du bloc DAILY ---
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

                // Parsing de la date (format YYYY-MM-DD)
                if (time_item && cJSON_IsString(time_item))
                {
                    int year, month, day;
                    if (sscanf(time_item->valuestring, "%d-%d-%d", &year, &month, &day) == 3)
                    {
                        struct tm tm = {0};
                        tm.tm_year = year - 1900;
                        tm.tm_mon = month - 1;
                        tm.tm_mday = day;
                        data->forecast_7j[i].timestamp = timegm(&tm); // UTC
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

                // Parsing de temperature_2m_max
                if (temp_item && cJSON_IsNumber(temp_item))
                {
                    float temp = (float)temp_item->valuedouble;
                    if (temp >= -50.0f && temp <= 60.0f)
                    {
                        data->forecast_7j[i].temperature = temp;
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Température daily invalide: %.1f°C", temp);
                        data->forecast_7j[i].temperature = 0.0f;
                    }
                }
                else
                {
                    data->forecast_7j[i].temperature = 0.0f;
                }

                // Parsing de weather_code
                if (code_item && cJSON_IsNumber(code_item))
                {
                    data->forecast_7j[i].weather_code = code_item->valueint;
                }
                else
                {
                    data->forecast_7j[i].weather_code = 0;
                }
            }
        }
    }

    return ESP_OK;
}

// --- Weather Update (Main Function) ---
esp_err_t weather_update(weather_data_t *data)
{
    char url[512];

    if (!data)
    {
        ESP_LOGE(TAG, "data est NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Vérifie si le cache est valide
    if (weather_cache_is_valid())
    {
        memcpy(data, &s_cached_weather_data, sizeof(weather_data_t));
        ESP_LOGI(TAG, "Utilisation des données météo en cache");
        return ESP_OK;
    }

    memset(data, 0, sizeof(weather_data_t));

    // --- 1. Essayer Open-Meteo (avec réessais) ---
    for (int attempt = 0; attempt < 3; attempt++)
    { // 3 tentatives
        char url[512];
        snprintf(url, sizeof(url),
                 "%s?latitude=%.5f&longitude=%.5f"
                 "&current=temperature_2m,relative_humidity_2m,weather_code,surface_pressure"
                 "&hourly=temperature_2m,relative_humidity_2m,weather_code&forecast_hours=48"
                 "&daily=weather_code,temperature_2m_max,relative_humidity_2m_max"
                 "&timezone=auto",
                 OPEN_METEO_URL, CONFIG_WEATHER_LAT, CONFIG_WEATHER_LON);

        if (http_get_to_buffer(url, 10000) == ESP_OK && response_len > 0)
        {
            cJSON *root = cJSON_ParseWithLength(response_buffer, response_len);
            if (root)
            {
                esp_err_t err = parse_open_meteo_json(root, data);
                cJSON_Delete(root);
                if (err == ESP_OK)
                {
                    ESP_LOGI(TAG, "Open-Meteo: succès (attempt %d)", attempt + 1);
                    goto success;
                }
                ESP_LOGW(TAG, "Open-Meteo: parsing échoué (attempt %d)", attempt + 1);
            }
            else
            {
                ESP_LOGE(TAG, "Open-Meteo: JSON invalide (attempt %d)", attempt + 1);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Open-Meteo: requête HTTP échouée (attempt %d)", attempt + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); // Attend 5s avant réessai
    }

    // --- 2. Fallback: OpenWeatherMap (Current + Forecast) ---
    ESP_LOGW(TAG, "Fallback vers OpenWeatherMap");
    for (int attempt = 0; attempt < 3; attempt++)
    {
        // Current
        snprintf(url, sizeof(url),
                 "https://api.openweathermap.org/data/2.5/weather?lat=%.4f&lon=%.4f&appid=%s&units=metric",
                 CONFIG_WEATHER_LAT, CONFIG_WEATHER_LON, CONFIG_OPEN_WEATHER_MAP_KEY);

        if (http_get_to_buffer(url, 10000) != ESP_OK || response_len == 0)
        {
            ESP_LOGE(TAG, "OWM Current: requête HTTP échouée (attempt %d)", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        cJSON *root_current = cJSON_ParseWithLength(response_buffer, response_len);
        if (!root_current)
        {
            ESP_LOGE(TAG, "OWM Current: JSON invalide (attempt %d)", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (parse_owm_current(root_current, data) != ESP_OK)
        {
            ESP_LOGE(TAG, "OWM Current: parsing échoué (attempt %d)", attempt + 1);
            cJSON_Delete(root_current);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        cJSON_Delete(root_current);

        // Forecast
        snprintf(url, sizeof(url),
                 "https://api.openweathermap.org/data/2.5/forecast?lat=%.4f&lon=%.4f&appid=%s&units=metric",
                 CONFIG_WEATHER_LAT, CONFIG_WEATHER_LON, CONFIG_OPEN_WEATHER_MAP_KEY);

        if (http_get_to_buffer(url, 10000) != ESP_OK || response_len == 0)
        {
            ESP_LOGE(TAG, "OWM Forecast: requête HTTP échouée (attempt %d)", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        cJSON *root_forecast = cJSON_ParseWithLength(response_buffer, response_len);
        if (!root_forecast)
        {
            ESP_LOGE(TAG, "OWM Forecast: JSON invalide (attempt %d)", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (parse_owm_forecast(root_forecast, data) != ESP_OK)
        {
            ESP_LOGE(TAG, "OWM Forecast: parsing échoué (attempt %d)", attempt + 1);
            cJSON_Delete(root_forecast);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        cJSON_Delete(root_forecast);
        ESP_LOGI(TAG, "OpenWeatherMap: succès (attempt %d)", attempt + 1);
        goto success;
    }

    // --- Toutes les sources ont échoué ---
    ESP_LOGE(TAG, "Toutes les sources météo ont échoué");
    return ESP_FAIL;

success:
    // Sauvegarde dans le magasin de stockage
    weather_store_set_all(data);
    // Met à jour le cache
    weather_cache_update(data);

    // Publie un événement EVENT_WEATHER_UPDATE (toutes les données)
    event_t event;
    event.type = EVENT_WEATHER_UPDATE;
    event.priority = EVENT_PRIO_NORMAL;
    event.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    event.payload.payload_ptr = (void *)data;
    event.payload.payload_len = sizeof(weather_data_t);
    if (!event_bus_publish(&event)) {
        ESP_LOGE(TAG, "Échec de la publication de EVENT_WEATHER_UPDATE");
    }

    // ✅ Publie EVENT_WEATHER_HOURLY avec les données de la première heure
    // Utilise data->forecast_48h_temp[0] et data->forecast_48h_hum[0] pour l'heure actuelle
    if (data->forecast_48h_temp[0] != 0.0f || data->forecast_48h_hum[0] != 0.0f) {
        if (!weather_publish_hourly(
                data->forecast_48h_temp[0],  // Température pour l'heure actuelle
                data->forecast_48h_hum[0],   // Humidité pour l'heure actuelle
                data->forecast_48h_code[0]   // Code météo pour l'heure actuelle
            )) {
            ESP_LOGE(TAG, "Échec de la publication de EVENT_WEATHER_HOURLY");
        }
    } else {
        ESP_LOGW(TAG, "Données horaires invalides (0.0), EVENT_WEATHER_HOURLY non publié");
    }

    return ESP_OK;
}

// --- JSON Generation ---
static char *weather_to_json(const weather_data_t *data)
{
    if (!data)
    {
        ESP_LOGE(TAG, "data est NULL");
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Échec de la création de l'objet JSON racine");
        return NULL;
    }

    // --- Section "now" ---
    cJSON *now = cJSON_AddObjectToObject(root, "now");
    if (!now)
    {
        ESP_LOGE(TAG, "Échec de l'ajout de l'objet 'now'");
        cJSON_Delete(root);
        return NULL;
    }

    // Ajout des champs avec validation
    float temp = (data->current.temperature >= -50.0f && data->current.temperature <= 60.0f)
                     ? data->current.temperature
                     : 0.0f;
    if (!cJSON_AddNumberToObject(now, "temp", temp))
    {
        ESP_LOGE(TAG, "Échec de l'ajout de 'now.temp'");
        cJSON_Delete(root);
        return NULL;
    }

    float hum = (data->current.humidity >= 0.0f && data->current.humidity <= 100.0f)
                    ? data->current.humidity
                    : 0.0f;
    if (!cJSON_AddNumberToObject(now, "hum", hum))
    {
        ESP_LOGE(TAG, "Échec de l'ajout de 'now.hum'");
        cJSON_Delete(root);
        return NULL;
    }

    const char *desc = get_weather_description(data->current.weather_code);
    if (!cJSON_AddStringToObject(now, "desc", desc))
    {
        ESP_LOGE(TAG, "Échec de l'ajout de 'now.desc'");
        cJSON_Delete(root);
        return NULL;
    }

    if (!cJSON_AddNumberToObject(now, "time", (double)data->current.timestamp))
    {
        ESP_LOGE(TAG, "Échec de l'ajout de 'now.time'");
        cJSON_Delete(root);
        return NULL;
    }

    if (!cJSON_AddNumberToObject(now, "pres", data->current.pressure))
    {
        ESP_LOGE(TAG, "Échec de l'ajout de 'now.pres'");
        cJSON_Delete(root);
        return NULL;
    }

    // --- Section "f48_temps" (tableau de 48 floats) ---
    cJSON *f48_temps = cJSON_AddArrayToObject(root, "f48_temps");
    if (!f48_temps)
    {
        ESP_LOGE(TAG, "Échec de l'ajout du tableau 'f48_temps'");
        cJSON_Delete(root);
        return NULL;
    }
    for (int i = 0; i < 48; i++)
    {
        cJSON *num = cJSON_CreateNumber(data->forecast_48h_temp[i]);
        if (!num)
        {
            ESP_LOGE(TAG, "Échec de la création du nombre pour f48_temps[%d]", i);
            cJSON_Delete(root);
            return NULL;
        }
        if (!cJSON_AddItemToArray(f48_temps, num))
        {
            ESP_LOGE(TAG, "Échec de l'ajout de f48_temps[%d]", i);
            cJSON_Delete(num);
            cJSON_Delete(root);
            return NULL;
        }
    }

    // --- Section "f48_hums" (tableau de 48 floats) ---
    cJSON *f48_hums = cJSON_AddArrayToObject(root, "f48_hums");
    if (!f48_hums)
    {
        ESP_LOGE(TAG, "Échec de l'ajout du tableau 'f48_hums'");
        cJSON_Delete(root);
        return NULL;
    }
    for (int i = 0; i < 48; i++)
    {
        cJSON *num = cJSON_CreateNumber(data->forecast_48h_hum[i]);
        if (!num)
        {
            ESP_LOGE(TAG, "Échec de la création du nombre pour f48_hums[%d]", i);
            cJSON_Delete(root);
            return NULL;
        }
        if (!cJSON_AddItemToArray(f48_hums, num))
        {
            ESP_LOGE(TAG, "Échec de l'ajout de f48_hums[%d]", i);
            cJSON_Delete(num);
            cJSON_Delete(root);
            return NULL;
        }
    }

    // --- Section "f7j" (7 jours) ---
    cJSON *f7j = cJSON_AddArrayToObject(root, "f7j");
    if (!f7j)
    {
        ESP_LOGE(TAG, "Échec de l'ajout du tableau 'f7j'");
        cJSON_Delete(root);
        return NULL;
    }

    for (int i = 0; i < 7; i++)
    {
        cJSON *day_obj = cJSON_CreateObject();
        if (!day_obj)
        {
            ESP_LOGE(TAG, "Échec de la création de l'objet day[%d]", i);
            cJSON_Delete(root);
            return NULL;
        }

        // Gestion du timestamp et du jour
        char day_str[16] = "Inconnu";
        if (data->forecast_7j[i].timestamp != 0)
        {
            time_t temp_time = (time_t)data->forecast_7j[i].timestamp;
            struct tm tm;
            if (localtime_r(&temp_time, &tm))
            {
                strftime(day_str, sizeof(day_str), "%a. %d", &tm);
            }
        }

        if (!cJSON_AddStringToObject(day_obj, "day", day_str))
        {
            ESP_LOGE(TAG, "Échec de l'ajout de 'day[%d].day'", i);
            cJSON_Delete(day_obj);
            cJSON_Delete(root);
            return NULL;
        }

        // Ajout des autres champs avec validation
        float temp_7j = (data->forecast_7j[i].temperature >= -50.0f && data->forecast_7j[i].temperature <= 60.0f)
                            ? data->forecast_7j[i].temperature
                            : 0.0f;
        if (!cJSON_AddNumberToObject(day_obj, "temp", temp_7j))
        {
            ESP_LOGE(TAG, "Échec de l'ajout de 'day[%d].temp'", i);
            cJSON_Delete(day_obj);
            cJSON_Delete(root);
            return NULL;
        }

        const char *desc_7j = get_weather_description(data->forecast_7j[i].weather_code);
        if (!cJSON_AddStringToObject(day_obj, "desc", desc_7j))
        {
            ESP_LOGE(TAG, "Échec de l'ajout de 'day[%d].desc'", i);
            cJSON_Delete(day_obj);
            cJSON_Delete(root);
            return NULL;
        }

        if (!cJSON_AddNumberToObject(day_obj, "time", (double)data->forecast_7j[i].timestamp))
        {
            ESP_LOGE(TAG, "Échec de l'ajout de 'day[%d].time'", i);
            cJSON_Delete(day_obj);
            cJSON_Delete(root);
            return NULL;
        }

        if (!cJSON_AddItemToArray(f7j, day_obj))
        {
            ESP_LOGE(TAG, "Échec de l'ajout de day[%d] au tableau", i);
            cJSON_Delete(day_obj);
            cJSON_Delete(root);
            return NULL;
        }
    }

    // Génère la chaîne JSON
    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str)
    {
        ESP_LOGE(TAG, "Échec de la sérialisation JSON");
    }
    cJSON_Delete(root);
    return json_str;
}

// --- Public Functions ---
char *weather_generate_json(void)
{
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        char *json = weather_to_json(&g_weather_data);
        xSemaphoreGive(g_weather_mutex);
        return json;
    }
    return NULL;
}

float temperature_get_outdoor(void)
{
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        float temp = g_weather_data.current.meteo_valid ? g_weather_data.current.temperature : 0.0f;
        xSemaphoreGive(g_weather_mutex);
        return temp;
    }
    return 0.0f;
}

bool temperature_get_valid(void)
{
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        bool valid = g_weather_data.current.meteo_valid;
        xSemaphoreGive(g_weather_mutex);
        return valid;
    }
    return false;
}

void temperature_set_valid(bool result)
{
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        g_weather_data.current.meteo_valid = result;
        xSemaphoreGive(g_weather_mutex);
    }
}

float weather_get_forecast_temp(int hours)
{
    if (hours < 0 || hours >= 48)
    {
        return 0.0f;
    }
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        float temp = g_weather_data.forecast_48h_temp[hours];
        xSemaphoreGive(g_weather_mutex);
        return temp;
    }
    return 0.0f;
}

float weather_get_forecast_humidity(int hours)
{
    if (hours < 0 || hours >= 48)
    {
        return 0.0f;
    }
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        float hum = g_weather_data.forecast_48h_hum[hours];
        xSemaphoreGive(g_weather_mutex);
        return hum;
    }
    return 0.0f;
}

int weather_get_current_code(void)
{
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        int code = g_weather_data.current.weather_code;
        xSemaphoreGive(g_weather_mutex);
        return code;
    }
    return 0;
}

int weather_get_forecast_code(int hours)
{
    if (hours < 0 || hours >= 48)
    {
        return weather_get_current_code();
    }
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        int code = g_weather_data.forecast_48h_code[hours];
        xSemaphoreGive(g_weather_mutex);
        return code;
    }
    return weather_get_current_code();
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

// --- Geocoding ---
esp_err_t weather_geocode_city(const char *city, double *lat, double *lon)
{
    if (!city || !lat || !lon)
    {
        ESP_LOGE(TAG, "city, lat ou lon est NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Encodage URL de la ville (remplace les espaces par %20)
    char encoded_city[64];
    int j = 0;
    for (int i = 0; city[i] != '\0' && j < sizeof(encoded_city) - 1; i++)
    {
        if (city[i] == ' ')
        {
            if (j + 3 >= sizeof(encoded_city))
                break;
            encoded_city[j++] = '%';
            encoded_city[j++] = '2';
            encoded_city[j++] = '0';
        }
        else
        {
            encoded_city[j++] = city[i];
        }
    }
    encoded_city[j] = '\0';

    // Requête HTTP
    char url[256];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=fr&format=json",
             encoded_city);

    esp_err_t err = http_get_to_buffer(url, 20000);
    if (err != ESP_OK || response_len == 0)
    {
        ESP_LOGE(TAG, "Requête HTTP échouée (%s)", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Parsing JSON
    cJSON *root = cJSON_ParseWithLength(response_buffer, response_len);
    if (!root)
    {
        ESP_LOGE(TAG, "Échec du parsing JSON");
        return ESP_FAIL;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0)
    {
        ESP_LOGE(TAG, "Aucun résultat trouvé");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *item = cJSON_GetArrayItem(results, 0);
    if (item)
    {
        cJSON *lat_item = cJSON_GetObjectItem(item, "latitude");
        cJSON *lon_item = cJSON_GetObjectItem(item, "longitude");
        if (lat_item && cJSON_IsNumber(lat_item))
        {
            *lat = lat_item->valuedouble;
        }
        else
        {
            *lat = 0.0;
        }
        if (lon_item && cJSON_IsNumber(lon_item))
        {
            *lon = lon_item->valuedouble;
        }
        else
        {
            *lon = 0.0;
        }
    }
    else
    {
        *lat = 0.0;
        *lon = 0.0;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// --- Event Bus Integration ---
bool weather_publish_hourly(float temperature, float humidity, int weather_code)
{
    event_t event;
    event.type = EVENT_WEATHER_HOURLY;
    event.priority = EVENT_PRIO_NORMAL;
    event.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    event.weather_hourly.temperature = temperature;
    event.weather_hourly.humidity = humidity;
    event.weather_hourly.weather_code = weather_code;
    ESP_LOGI(TAG, "Publication de EVENT_WEATHER_HOURLY");
    return event_bus_publish(&event);
}
