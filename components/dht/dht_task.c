#include "dht_task.h"

#include <stdlib.h>
#include <time.h>
#include <math.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "dht.h"
#include "event_bus.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define DHT_TEMP_DELTA ((float)atof(CONFIG_DHT_TEMP_DELTA))
#define DHT_HUM_DELTA  ((float)atof(CONFIG_DHT_HUM_DELTA))

static float last_published_temp = NAN;
static float last_published_hum = NAN;
static uint32_t last_publish_ms = 0;

static const char *TAG = "DHT_TASK";

static uint32_t consecutive_errors = 0;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void dht_task(void *pvParameters)
{
    (void)pvParameters;

    gpio_reset_pin(CONFIG_DHT_GPIO_PIN);
    gpio_set_pull_mode(CONFIG_DHT_GPIO_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "DHT initialise sur GPIO %d avec Pull-Up", CONFIG_DHT_GPIO_PIN);

    ESP_ERROR_CHECK(dht_init(CONFIG_DHT_GPIO_PIN, CONFIG_DHT_TYPE));

    vTaskDelay(pdMS_TO_TICKS(CONFIG_DHT_SETUP_DELAY));

    while (1)
    {
        esp_err_t ret = dht_perform_measurement();
        const dht_runtime_t *runtime = dht_get_runtime();

        // =========================================================
        // CAS 1 : LECTURE OK
        // =========================================================
        if (ret == ESP_OK)
        {
            consecutive_errors = 0;

            bool changed =
                isnan(last_published_temp) ||
                fabsf(runtime->temperature - last_published_temp) >= DHT_TEMP_DELTA ||
                fabsf(runtime->humidity - last_published_hum) >= DHT_HUM_DELTA;

            bool heartbeat =
                (now_ms() - last_publish_ms) >= CONFIG_DHT_HEARTBEAT_MS;

            if (changed || heartbeat)
            {
                // CORRECTION: On initialise UNIQUEMENT le sous-bloc .sensor
                event_t evt = {
                    .type = EVENT_SENSOR_DHT,
                    .priority = EVENT_PRIO_NORMAL,
                    .timestamp_ms = now_ms(),
                    .sensor = {
                        .temperature = runtime->temperature,
                        .humidity = runtime->humidity
                    }
                };

                event_bus_publish(&evt);

                last_published_temp = runtime->temperature;
                last_published_hum = runtime->humidity;
                last_publish_ms = now_ms();
            }
        }
        // =========================================================
        // CAS 2 : ERREUR DE LECTURE
        // =========================================================
        else
        {
            consecutive_errors++;

            if (consecutive_errors < 3)
            {
                ESP_LOGD(TAG, "Lecture DHT echouee (essai %lu/3): %s",
                         (unsigned long)consecutive_errors, esp_err_to_name(ret));
            }
            else
            {
                ESP_LOGE(TAG, "DHT en panne apres 3 essais consecutifs: %s", esp_err_to_name(ret));

                // CORRECTION: On initialise UNIQUEMENT le sous-bloc .net
                event_t evt = {
                    .type = EVENT_SENSOR_ERROR_DHT,
                    .priority = EVENT_PRIO_HIGH,
                    .timestamp_ms = now_ms(),
                    .net = {
                        .bool_value = false, // Utile ici car on n'utilise pas le bloc sensor
                        .retry_count = consecutive_errors,
                        .error_code = ret
                    }
                };

                event_bus_publish(&evt);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_DHT_READ_INTERVAL_MS));
    }
}
