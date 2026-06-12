#include "wifi_state_machine.h"
#include "esp_log.h"

static const char *TAG = "WIFI_STATE";
// Encapsulation stricte de l'état du Wi-Fi
static wifi_state_t s_state = WIFI_STATE_INIT;

wifi_state_t wifi_state_get(void)
{
    return s_state;
}

void wifi_state_update(wifi_state_t new_state)
{
    if (s_state == new_state)
        return;

    ESP_LOGI(TAG, "Transition %d -> %d", s_state, new_state);
    s_state = new_state;
}
