#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

typedef enum
{
    DHT_TYPE_DHT11,
    DHT_TYPE_DHT22,
} dht_sensor_type_t;

typedef struct
{
    gpio_num_t gpio_pin;
    dht_sensor_type_t sensor_type;
    uint32_t read_interval_ms;
} dht_config_t;

typedef struct
{
    float temperature;
    float humidity;
    bool valid;
    bool initialized;
    bool running;
    uint32_t read_count;
    uint32_t error_count;
    uint32_t consecutive_error_count;
    esp_err_t last_error_code;
    time_t last_error_at;
    time_t last_success_at;
    time_t last_update;
    char last_error[64];
    uint32_t recovery_attempts;
} dht_runtime_t;

esp_err_t dht_init(gpio_num_t gpio, dht_sensor_type_t type);
esp_err_t dht_read_data(gpio_num_t gpio_num, dht_sensor_type_t type, float *humidity, float *temperature);
esp_err_t dht_get_config(dht_config_t *out);
esp_err_t dht_set_config(const dht_config_t *config);
const dht_runtime_t *dht_get_runtime(void);
char *dht_get_json_status(void);
esp_err_t dht_perform_measurement(void);
