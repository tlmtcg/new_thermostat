#ifndef SYSTEM_MANAGER_H
#define SYSTEM_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void system_manager_start(void);

extern SemaphoreHandle_t g_i2c_mutex;

#endif
