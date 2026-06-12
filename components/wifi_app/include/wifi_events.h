#pragma once

#include "esp_event.h"
#include "esp_netif_ip_addr.h"

typedef struct
{
    void (*on_ap_started)(void);
    void (*on_sta_connected)(const esp_ip4_addr_t *ip);
    void (*on_sta_failed)(int reason);
} wifi_callbacks_t;

void wifi_events_init(const wifi_callbacks_t *callbacks);
