#include "system_manager.h"
#include "event_bus.h"
#include "esp_log.h"
#include "tasks.h"
#include "relay.h"
#include "driver/i2c_master.h"
#include "sht31.h"
#include "sdkconfig.h"
#include "oled_service.h"
#include "ssd1306_driver.h"
#include "i2c_shared.h"
#include "sensor_service.h"
#include "thermostat_task.h"
#include "wifi_app.h"

static const char *TAG = "system_manager";

// Variables globales partagées du système
EventGroupHandle_t g_app_event_group = NULL;

void system_manager_start(void)
{
    ESP_LOGI(TAG, "System start");

    // ---------------------------------------------------------------------
    // 1. BASE SYSTEM
    // ---------------------------------------------------------------------
    event_bus_init();
    relay_init();

    g_app_event_group = xEventGroupCreate();
    if (!g_app_event_group)
    {
        ESP_LOGE(TAG, "Failed to create system EventGroup");
        return;
    }

    // IMPORTANT : mutex I2C AVANT tout accès bus
    i2c_mutex_init();

    // ---------------------------------------------------------------------
    // 2. I2C BUS INIT
    // ---------------------------------------------------------------------
    esp_err_t err = hardware_i2c_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_bus_scan(i2c_bus_handle);

    // ---------------------------------------------------------------------
    // 3. SENSOR INIT (SANS TASK)
    // ---------------------------------------------------------------------
    ESP_LOGI(TAG, "Init sensor service");

    err = sensor_service_init(i2c_bus_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Sensor init failed: %s", esp_err_to_name(err));
    }

    // ---------------------------------------------------------------------
    // 4. OLED INIT (SANS TASK)
    // ---------------------------------------------------------------------
    ESP_LOGI(TAG, "Init OLED service");

    err = oled_service_init(i2c_bus_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(err));
    }

    // ---------------------------------------------------------------------
    // 5. THERMOSTAT INIT & START TASK SYSTEM (RTOS)
    // ---------------------------------------------------------------------
    ESP_LOGI(TAG, "Init Thermostat core");
    
    // Initialise le calendrier (NVS) et crée l'UNIQUE instance de thermostat_task
    err = thermostat_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Thermostat init failed: %s", esp_err_to_name(err));
    }

    // IMPORTANT : Ouvre ton fichier "tasks.c" et assure-toi que la fonction 
    // tasks_start() NE contient PLUS d'appel à xTaskCreate pour thermostat_task.
    tasks_start();

    vTaskDelay(pdMS_TO_TICKS(50));

    wifi_app_start();

    // ---------------------------------------------------------------------
    // 6. START SERVICES (MAINTENANT QUE TOUT EST STABLE)
    // ---------------------------------------------------------------------

    ESP_LOGI(TAG, "Starting sensor service");

    err = sensor_service_start(i2c_bus_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Sensor start failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    oled_task_config_t oled_config = {
        .event_group = g_app_event_group,
        .event_bit = BIT(0),
        .refresh_interval_ms = 500
    };

    ESP_LOGI(TAG, "Starting OLED service");

    err = oled_service_start(&oled_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OLED start failed: %s", esp_err_to_name(err));
    }

    // ---------------------------------------------------------------------
    // 7. ACTIVATE SYSTEM
    // ---------------------------------------------------------------------
    xEventGroupSetBits(g_app_event_group, BIT(0));

    ESP_LOGI(TAG, "System ready");
}

// void system_manager_start(void)
// {
//     ESP_LOGI(TAG, "System start");

//     // ---------------------------------------------------------------------
//     // 1. BASE SYSTEM
//     // ---------------------------------------------------------------------
//     event_bus_init();
//     relay_init();

//     g_app_event_group = xEventGroupCreate();
//     if (!g_app_event_group)
//     {
//         ESP_LOGE(TAG, "Failed to create system EventGroup");
//         return;
//     }

//     // IMPORTANT : mutex I2C AVANT tout accès bus
//     i2c_mutex_init();

//     // ---------------------------------------------------------------------
//     // 2. I2C BUS INIT
//     // ---------------------------------------------------------------------
//     esp_err_t err = hardware_i2c_init();
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
//         return;
//     }

//     vTaskDelay(pdMS_TO_TICKS(50));

//     i2c_bus_scan(i2c_bus_handle);

//     // ---------------------------------------------------------------------
//     // 3. SENSOR INIT (SANS TASK)
//     // ---------------------------------------------------------------------
//     ESP_LOGI(TAG, "Init sensor service");

//     err = sensor_service_init(i2c_bus_handle);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "Sensor init failed: %s", esp_err_to_name(err));
//     }

//     // ---------------------------------------------------------------------
//     // 4. OLED INIT (SANS TASK)
//     // ---------------------------------------------------------------------
//     ESP_LOGI(TAG, "Init OLED service");

//     err = oled_service_init(i2c_bus_handle);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(err));
//     }

//     // ---------------------------------------------------------------------
//     // 5. START TASK SYSTEM (UNIQUEMENT RTOS)
//     // ---------------------------------------------------------------------
//     tasks_start();

//     vTaskDelay(pdMS_TO_TICKS(50));

//     // ---------------------------------------------------------------------
//     // 6. START SERVICES (MAINTENANT QUE TOUT EST STABLE)
//     // ---------------------------------------------------------------------

//     ESP_LOGI(TAG, "Starting sensor service");

//     err = sensor_service_start(i2c_bus_handle);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "Sensor start failed: %s", esp_err_to_name(err));
//     }

//     vTaskDelay(pdMS_TO_TICKS(50));

//     oled_task_config_t oled_config = {
//         .event_group = g_app_event_group,
//         .event_bit = BIT(0),
//         .refresh_interval_ms = 500
//     };

//     ESP_LOGI(TAG, "Starting OLED service");

//     err = oled_service_start(&oled_config);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "OLED start failed: %s", esp_err_to_name(err));
//     }

//     // ---------------------------------------------------------------------
//     // 7. ACTIVATE SYSTEM
//     // ---------------------------------------------------------------------
//     xEventGroupSetBits(g_app_event_group, BIT(0));

//     ESP_LOGI(TAG, "System ready");
// }

