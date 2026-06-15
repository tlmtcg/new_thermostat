#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        long timestamp;
        float temperature;
        float humidity;
        float pressure;
        int weather_code;
        bool meteo_valid;
    } weather_entry_t;

    typedef struct
    {
        weather_entry_t current;
        weather_entry_t forecast_48h;
        weather_entry_t forecast_7j[7];

        float forecast_48h_temp[48];
        float forecast_48h_hum[48];
        int forecast_48h_code[48];
    } weather_data_t;

    /**
     * @brief Retourne une description textuelle (avec emoji) pour un code météo Open‑Meteo.
     *
     * @param code Code météo (0, 1, 2, 3, 45, 48, 51, 53, 55, 61, 63, 65, 71, 73, 75, 80, 81, 82, 95, 96, 99)
     * @return Chaîne statique décrivant le phénomène météo.
     */
    const char *get_weather_description(int code);

    // Fonction commune
    // esp_err_t http_get_to_buffer(const char *url, int timeout_ms);

    esp_err_t weather_update(weather_data_t *data);

    extern weather_data_t g_latest_weather;

    float temperature_get_outdoor();

    float weather_get_forecast_temp(int hours);

    float weather_get_forecast_humidity(int hours);

    int weather_get_forecast_code(int hours);

    int weather_get_current_code(void);

    esp_err_t weather_geocode_city(const char *city, double *lat, double *lon);

    void weather_update_task_manually(void *arg);

    extern weather_data_t g_weather_data;

    esp_err_t weather_get_temp_in_x_hours(const weather_data_t *data, int hours_from_now, float *out_temp);

    bool temperature_get_valid();

    void temperature_set_valid(bool result);

    bool weather_publish_hourly(float temperature, float humidity, int weather_code);

    void weather_init(void);

#ifdef __cplusplus
}
#endif
