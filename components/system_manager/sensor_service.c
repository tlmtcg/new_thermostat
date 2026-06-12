#include "sensor_service.h"
#include "sht31.h"
#include "dht.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "SENSOR_SERVICE";

static bool g_sensor_running = false;

esp_err_t sensor_service_init(i2c_master_bus_handle_t bus)
{
    if (!bus)
    {
        ESP_LOGE(TAG, "Bus I2C NULL lors de l'init");
        return ESP_ERR_INVALID_ARG;
    }

    // 1. Initialisation matérielle du SHT31 (On prépare le driver I2C sans lancer de boucle)
    // esp_err_t err = sht31_init(bus, 0x44); // <-- Utilise la vraie fonction d'init si elle existe, sinon laisse sht31_start ici mais lis la suite
    // if (err != ESP_OK)
    // {
    //     ESP_LOGE(TAG, "SHT31 driver init failed: %s", esp_err_to_name(err));
    //     return err;
    // }

    // 2. Initialisation du DHT (On prépare le GPIO)
    // dht_init(CONFIG_DHT_GPIO_PIN, DHT_TYPE_DHT11); // Optionnel : déplacer l'init du DHT ici est plus propre

    ESP_LOGI(TAG, "Sensor service initialized");
    return ESP_OK;
}

esp_err_t sensor_service_start(i2c_master_bus_handle_t bus)
{
    if (g_sensor_running)
    {
        ESP_LOGW(TAG, "Sensor service already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting sensor service tasks");

    // Si ton driver sht31_start lance la TÂCHE FreeRTOS (sht31_task), c'est ICI qu'il faut l'appeler, et PAS dans l'init.
    esp_err_t err = sht31_start(bus, 0x44); 
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SHT31 task start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Initialisation/Démarrage du DHT si pas fait dans l'init
    dht_init(CONFIG_DHT_GPIO_PIN, DHT_TYPE_DHT11);

    g_sensor_running = true;

    ESP_LOGI(TAG, "Sensor service started");
    return ESP_OK;
}
