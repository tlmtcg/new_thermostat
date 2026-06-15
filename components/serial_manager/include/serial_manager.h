#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t serial_manager_init(void);

void serial_manager_handle_command(const char *cmd);

void handle_command(const char *cmd);

void serial_task(void *arg);

#ifdef __cplusplus
}
#endif
