// #include "thermostat_manager.h"

// static thermostat_state_t g_state = {0};

// void thermostat_update(float temp, float hum, bool valid)
// {
//     g_state.temperature = temp;
//     g_state.humidity = hum;
//     g_state.valid = valid;
//     g_state.timestamp = time(NULL);
// }

// void thermostat_get(thermostat_state_t *out)
// {
//     if (out)
//     {
//         *out = g_state;
//     }
// }