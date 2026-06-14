#include "weather_store.h"
#include "esp_log.h"
#include <string.h>

static weather_data_t store;
static SemaphoreHandle_t mutex;

static const char *TAG = "WEATHER_STORE";

void weather_store_init(void)
{
    if (mutex == NULL)
    {
        mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    memset(&store, 0, sizeof(store));
    xSemaphoreGive(mutex);
}

void weather_store_set_all(const weather_data_t *src)
{
    if (!src || !mutex)
        return;

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        store = *src; // copie complète
        xSemaphoreGive(mutex);
    }
    else
    {
        ESP_LOGE(TAG, "Could not acquire mutex for SET");
    }
}

void weather_store_get_all(weather_data_t *dst)
{
    // Sécurité : évite le crash si le mutex n'existe pas encore
    if (!dst || !mutex)
    {
        ESP_LOGE(TAG, "Store not init or NULL destination");
        return;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE)
    {
        *dst = store;
        xSemaphoreGive(mutex);
    }
}

weather_entry_t weather_store_get_current(void)
{
    weather_entry_t temp = {0};
    if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE)
    {
        temp = store.current;
        xSemaphoreGive(mutex);
    }
    return temp;
}

float weather_store_get_jee_temp(void)
{
    float temp = 0.0f;
    if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE)
    {
        temp = store.current.jee_temp;
        xSemaphoreGive(mutex);
    }
    return temp;
}
