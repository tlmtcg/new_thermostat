#ifndef WEATHER_TASK_H
#define WEATHER_TASK_H

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "weather.h"

typedef struct
{
    EventGroupHandle_t event_group;
    uint32_t event_bit;
    uint32_t *delay_ms;
    bool (*is_wifi_connected)(void);
    void (*store_set_all)(const weather_data_t *data);
} weather_task_config_t;

void weather_update_task(void *pvParameters);

#endif
