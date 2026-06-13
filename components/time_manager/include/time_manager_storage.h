#ifndef TIME_MANAGER_STORAGE_H
#define TIME_MANAGER_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char ntp_server[64];
    uint8_t ntp_max_retry;
    uint32_t ntp_sync_interval_sec; // Nom unifié ici
} time_manager_config_t;

// Prototypes propres
bool time_manager_storage_load(time_manager_config_t *cfg);
bool time_manager_storage_save(const time_manager_config_t *cfg);
bool time_manager_storage_reset_defaults(void);

#endif
