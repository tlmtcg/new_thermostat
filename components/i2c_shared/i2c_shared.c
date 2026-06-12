#include "i2c_shared.h"
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char* TAG = "i2c_shared";

SemaphoreHandle_t g_i2c_mutex = NULL;

void i2c_mutex_init(void)
{
    g_i2c_mutex = xSemaphoreCreateMutex();
}

i2c_master_bus_handle_t i2c_bus_handle = NULL;

/**
 * @brief Utility function to scan and debug the I2C Bus
 */
esp_err_t i2c_bus_scan(i2c_master_bus_handle_t bus)
{
    if (!bus)
    {
        ESP_LOGE(TAG, "Bus I2C non initialisé");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Début scan I2C...");

    for (uint8_t addr = 0x08; addr <= 0x77; addr++)
    {
        esp_err_t err = i2c_master_probe(bus, addr, 100);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Device trouvé: 0x%02X", addr);
        }
    }
    return ESP_OK;
}

/**
 * @brief Initialisation matérielle du bus I2C
 */
esp_err_t hardware_i2c_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = CONFIG_I2C_SCL_GPIO,
        .sda_io_num = CONFIG_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 0,               // Désactivé pour éviter de filtrer les fronts d'horloge valides
        .flags.enable_internal_pullup = true, // Force les pull-ups internes si le hardware n'en a pas assez
    };

    return i2c_new_master_bus(&bus_config, &i2c_bus_handle);
}