#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"

extern SemaphoreHandle_t g_i2c_mutex;

extern i2c_master_bus_handle_t i2c_bus_handle;

void i2c_mutex_init(void);

esp_err_t i2c_bus_scan(i2c_master_bus_handle_t bus);

esp_err_t hardware_i2c_init(void);
