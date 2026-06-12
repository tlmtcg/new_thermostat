#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t sensor_service_init(i2c_master_bus_handle_t bus);
esp_err_t sensor_service_start(i2c_master_bus_handle_t bus);
