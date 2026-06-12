#pragma once

typedef enum
{
    WIFI_STATE_INIT = 0,
    WIFI_STATE_AP_ONLY,
    WIFI_STATE_STA_CONNECTING,
    WIFI_STATE_STA_CONNECTED,
    WIFI_STATE_STA_FAILED,
    WIFI_STATE_AP_AND_STA
} wifi_state_t;

wifi_state_t wifi_state_get(void);
void wifi_state_update(wifi_state_t new_state);
