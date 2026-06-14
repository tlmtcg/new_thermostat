#pragma once
#include "weather.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void weather_store_init(void);

// setters
void weather_store_set_all(const weather_data_t *src);

// getters
void weather_store_get_all(weather_data_t *dst);
weather_entry_t weather_store_get_current(void);
float weather_store_get_jee_temp(void);
