#pragma once

void thermostat_task(void *pvParameters);
esp_err_t thermostat_init(void);
void thermostat_get_mode_status_str(char *dest, size_t max_len);