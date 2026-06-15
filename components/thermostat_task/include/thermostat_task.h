#pragma once

#include <stdbool.h>
#include <stdint.h>

void thermostat_task(void *pvParameters);
esp_err_t thermostat_init(void);
void thermostat_get_mode_status_str(char *dest, size_t max_len);

/**
 * @brief Met à jour les données météo horaires dans le thermostat.
 * @param temperature Température en °C.
 * @param humidity Humidité en %.
 * @param weather_code Code météo (ex. 0 = ciel clair).
 */
void thermostat_update_hourly_weather(float temperature, float humidity, int weather_code);