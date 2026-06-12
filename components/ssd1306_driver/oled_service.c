
/**
 * @file oled_service.c
 * @brief Gestionnaire de haut niveau et orchestrateur des pages de l'écran OLED connecté à l'Event Bus.
 */

#include "oled_service.h"
#include "ssd1306_driver.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

// Dépendances de l'application
#include "event_bus.h"
// #include "thermostat.h"

static const char *TAG = "OLED_SVC";

// Instance unique globale du pilote matériel de l'OLED
static ssd1306_t oled_dev;
static oled_page_t current_page = OLED_PAGE_MAIN;
static oled_graph_data_t graph_data;
static TaskHandle_t oled_task_handle = NULL;

// Gestion de la file d'attente et protection des données
static QueueHandle_t oled_queue = NULL;
static SemaphoreHandle_t data_mutex = NULL;

/**
 * @brief Structure locale contenant les données fraîches reçues de l'Event Bus
 */
// Structure regroupant l'état du système pour l'affichage (à utiliser dans oled_service.c)
typedef struct
{
    float temperature;
    float humidity;
    float setpoint;
    bool relay_on;
    bool wifi_connected;            // Ajout état connexion
    char wifi_ip[OLED_IP_STR_SIZE]; // Ajout stockage IP
} oled_local_data_t;

static oled_local_data_t local_data = {
    .temperature = 0.0f,
    .humidity = 0.0f,
    .setpoint = 19.0f,
    .relay_on = false,
    .wifi_connected = false,
    .wifi_ip = "0.0.0.0"};

// Dimensions et constantes de mise en page
#define OLED_HEADER_HEIGHT 12
#define GLYPH_WIDTH 6
#define OLED_PAGE_COUNT 6

// Prototypes des fonctions d'affichage internes
static void draw_common_header(const char *page_title);
static void draw_centered_string(uint8_t y, const char *str);
static void draw_main_page(void);
static void draw_history_page(void);
static void draw_time_page(void);
static void draw_weather_page(void);
static void draw_alert_page(void);
static void draw_wifi_page(void);

/**
 * @brief Tâche OLED principale
 */
static void oled_rtos_task(void *pvParameters)
{
    const oled_task_config_t *cfg = (const oled_task_config_t *)pvParameters;
    event_t evt;

    xEventGroupWaitBits(cfg->event_group, cfg->event_bit, pdFALSE, pdTRUE, portMAX_DELAY);
    uint32_t page_tick_counter = 0;

    while (1)
    {
        // Traitement du bus
        while (event_bus_receive(oled_queue, &evt, 0))
        {
            if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(5)) == pdTRUE)
            {
                switch (evt.type)
                {
                case EVENT_SENSOR_DHT:
                case EVENT_SENSOR_SHT31:
                    local_data.temperature = evt.sensor.temperature;
                    local_data.humidity = evt.sensor.humidity;
                    oled_service_add_temp_to_history(evt.sensor.temperature);
                    break;
                case EVENT_THERMOSTAT_SET:
                    local_data.setpoint = evt.sensor.temperature;
                    break;
                case EVENT_RELAY_SET:
                    local_data.relay_on = evt.net.bool_value;
                    break;
                case EVENT_WIFI_STATUS:
                    local_data.wifi_connected = evt.net.bool_value;
                    if (local_data.wifi_connected && evt.payload.payload_ptr != NULL)
                    {
                        strlcpy(local_data.wifi_ip, (const char *)evt.payload.payload_ptr, sizeof(local_data.wifi_ip));
                    }
                    else
                    {
                        strlcpy(local_data.wifi_ip, "0.0.0.0", sizeof(local_data.wifi_ip));
                    }
                    break;
                default:
                    break;
                }
                xSemaphoreGive(data_mutex);
            }
        }

        // Cycle de rotation
        if (page_tick_counter >= 5000)
        {
            current_page = (current_page + 1) % OLED_PAGE_COUNT;
            page_tick_counter = 0;
        }

        // Rendu
        ssd1306_clear(&oled_dev);
        switch (current_page)
        {
        case OLED_PAGE_MAIN:
            draw_main_page();
            break;
        case OLED_PAGE_HISTORY:
            draw_history_page();
            break;
        case OLED_PAGE_TIME:
            draw_time_page();
            break;
        case OLED_PAGE_WEATHER:
            draw_weather_page();
            break;
        case OLED_PAGE_ALERTS:
            draw_alert_page();
            break;
        case OLED_PAGE_WIFI:
            draw_wifi_page();
            break; // Nouvelle page WiFi
        default:
            draw_main_page();
            break;
        }
        ssd1306_update(&oled_dev);

        vTaskDelay(pdMS_TO_TICKS(cfg->refresh_interval_ms));
        page_tick_counter += cfg->refresh_interval_ms;
    }
}

/**
 * @brief Rendu de la page WiFi
 */
static void draw_wifi_page(void)
{
    draw_common_header("NETWORK STATUS");

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    bool conn = local_data.wifi_connected;
    char ip[16];
    strlcpy(ip, local_data.wifi_ip, sizeof(ip));
    xSemaphoreGive(data_mutex);

    char buf[32];
    ssd1306_draw_string(&oled_dev, 10, 25, conn ? "WiFi: CONNECTED" : "WiFi: OFFLINE");
    snprintf(buf, sizeof(buf), "IP: %s", ip);
    ssd1306_draw_string(&oled_dev, 10, 45, buf);
}

esp_err_t oled_service_init(i2c_master_bus_handle_t bus)
{
    data_mutex = xSemaphoreCreateMutex();
    if (data_mutex == NULL)
        return ESP_ERR_NO_MEM;

    esp_err_t err = ssd1306_init(&oled_dev, bus, 0x3C);
    if (err == ESP_OK)
    {
        memset(&graph_data, 0, sizeof(graph_data));

        // S'ABONNER AUX FLUX DE L'EVENT BUS
        static const event_type_t oled_filter[] = {
            EVENT_SENSOR_DHT,
            EVENT_SENSOR_SHT31,
            EVENT_THERMOSTAT_SET,
            EVENT_RELAY_SET,
            EVENT_SENSOR_ERROR_DHT,
            EVENT_SENSOR_ERROR_SHT31,
            EVENT_WIFI_STATUS,
        };

        oled_queue = event_bus_subscribe("oled_service", oled_filter, sizeof(oled_filter));
        if (!oled_queue)
        {
            ESP_LOGE(TAG, "Échec de l'abonnement à l'Event Bus");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Service OLED initialisé.");
    }
    return err;
}

esp_err_t oled_service_start(const oled_task_config_t *config)
{
    if (config == NULL)
        return ESP_ERR_INVALID_ARG;

    // Sauvegarde statique de la configuration d'origine pour sécuriser la tâche
    static oled_task_config_t task_cfg;
    memcpy(&task_cfg, config, sizeof(oled_task_config_t));

    BaseType_t ret = xTaskCreatePinnedToCore(
        oled_rtos_task,
        "oled_task",
        4096, // Légèrement augmenté à 4KB pour la sécurité de l'Event Bus
        &task_cfg,
        3,
        &oled_task_handle,
        1);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void oled_service_add_temp_to_history(float temp)
{
    graph_data.temp_history[graph_data.history_index] = temp;
    graph_data.history_index++;
    if (graph_data.history_index >= TEMP_HISTORY_SIZE)
    {
        graph_data.history_index = 0;
        graph_data.history_full = true;
    }
}

/* =========================================================================
    FONCTIONS DE RENDU DES GRAPHISMES & PAGES (Inchangées, branchées sur local_data)
   ========================================================================= */

static void draw_centered_string(uint8_t y, const char *str)
{
    int len = strlen(str);
    int x = (SSD1306_WIDTH - (len * GLYPH_WIDTH)) / 2;
    if (x < 0)
        x = 0;
    ssd1306_draw_string(&oled_dev, x, y, str);
}

static void draw_common_header(const char *page_title)
{
    ssd1306_draw_string(&oled_dev, 2, 0, page_title);
    ssd1306_draw_line(&oled_dev, 0, OLED_HEADER_HEIGHT, SSD1306_WIDTH - 1, OLED_HEADER_HEIGHT, true);
}

static void draw_main_page(void)
{
    draw_common_header("THERMOSTAT MAIN");

    // Récupération sécurisée des données issues du bus
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    float temp = local_data.temperature;
    float hum = local_data.humidity;
    bool relay = local_data.relay_on;
    xSemaphoreGive(data_mutex);

    char buf[24];
    snprintf(buf, sizeof(buf), "TEMP: %.1f C", temp);
    ssd1306_draw_string(&oled_dev, 10, 20, buf);

    snprintf(buf, sizeof(buf), "HUMI: %.1f %%", hum);
    ssd1306_draw_string(&oled_dev, 10, 35, buf);

    snprintf(buf, sizeof(buf), "CHAUFFAGE: %s", relay ? "ON" : "OFF");
    ssd1306_draw_string(&oled_dev, 10, 50, buf);
}

static void draw_history_page(void)
{
    draw_common_header("TEMP HISTORY");

    // 1. Dessin des axes X et Y d'origine
    ssd1306_draw_line(&oled_dev, 15, 60, 115, 60, true); // Axe X
    ssd1306_draw_line(&oled_dev, 15, 20, 15, 60, true);  // Axe Y

    // 2. Recherche des valeurs Min et Max réelles pour calibrer l'échelle verticale
    float min_t = 100.0f;
    float max_t = -100.0f;
    uint8_t total_points = graph_data.history_full ? TEMP_HISTORY_SIZE : graph_data.history_index;

    if (total_points < 2)
    {
        // Pas assez de points pour tracer un graphique
        draw_centered_string(35, "[RECUEILLLEMENT DATA...]");
        return;
    }

    for (uint8_t i = 0; i < total_points; i++)
    {
        if (graph_data.temp_history[i] < min_t)
            min_t = graph_data.temp_history[i];
        if (graph_data.temp_history[i] > max_t)
            max_t = graph_data.temp_history[i];
    }

    // Sécurité si la température est parfaitement constante (évite la division par zéro)
    if (max_t == min_t)
    {
        max_t += 1.0f;
        min_t -= 1.0f;
    }

    // Affichage discret des bornes textuelles Min/Max sur le côté de l'axe Y
    char txt_buf[8];
    snprintf(txt_buf, sizeof(txt_buf), "%.0f", max_t);
    ssd1306_draw_string(&oled_dev, 0, 20, txt_buf);
    snprintf(txt_buf, sizeof(txt_buf), "%.0f", min_t);
    ssd1306_draw_string(&oled_dev, 0, 52, txt_buf);

    // 3. Tracé de la courbe
    // Paramètres géométriques du graphique
    const uint8_t graph_left = 16;
    const uint8_t graph_width = 98;
    const uint8_t graph_bottom = 59;
    const uint8_t graph_height = 38; // Espace vertical utile de 21 à 59

    uint8_t prev_x = 0;
    uint8_t prev_y = 0;

    for (uint8_t i = 0; i < total_points; i++)
    {
        // L'indice de départ doit suivre l'ordre chronologique (du plus ancien au plus récent)
        uint8_t idx = i;
        if (graph_data.history_full)
        {
            idx = (graph_data.history_index + i) % TEMP_HISTORY_SIZE;
        }

        // Calcul de la coordonnée X (étalée sur la largeur de l'axe X)
        uint8_t x = graph_left + (i * graph_width) / (total_points - 1);

        // Calcul de la coordonnée Y (inversée car l'origine 0,0 de l'écran est en haut à gauche)
        float pct = (graph_data.temp_history[idx] - min_t) / (max_t - min_t);
        uint8_t y = graph_bottom - (uint8_t)(pct * graph_height);

        // Tracer le segment depuis le point précédent
        if (i > 0)
        {
            ssd1306_draw_line(&oled_dev, prev_x, prev_y, x, y, true);
        }

        prev_x = x;
        prev_y = y;
    }
}

static void draw_time_page(void)
{
    draw_common_header("SYSTEM TIME");

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char hour_buffer[16];
    char date_buffer[24];

    strftime(hour_buffer, sizeof(hour_buffer), "%H:%M:%S", &timeinfo);
    strftime(date_buffer, sizeof(date_buffer), "%d/%m/%Y", &timeinfo);

    draw_centered_string(25, hour_buffer);
    draw_centered_string(45, date_buffer);
}

static void draw_weather_page(void)
{
    draw_common_header("WEATHER FORECAST");
    ssd1306_draw_string(&oled_dev, 5, 25, "Paris: Nuageux");
    ssd1306_draw_string(&oled_dev, 5, 45, "Ext: 14.2 C");
}

static void draw_alert_page(void)
{
    draw_common_header("SYSTEM ALERTS");
    ssd1306_draw_string(&oled_dev, 10, 30, "Pas d'alerte");
    ssd1306_draw_string(&oled_dev, 10, 45, "Statut : Normal");
}

void oled_service_show_error(const char *msg)
{
    ssd1306_clear(&oled_dev);
    draw_common_header("CRITICAL ERROR");
    if (msg != NULL)
    {
        draw_centered_string(32, msg);
    }
    ssd1306_update(&oled_dev);
}

esp_err_t oled_component_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    return ssd1306_init(&oled_dev, bus, addr);
}
