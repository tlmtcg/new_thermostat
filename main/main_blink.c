#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "dht.h"

// Déclare le TAG pour les logs
static const char *TAG = "Main";

// Déclare la variable d'état de la LED
static bool s_led_state = false;

// Déclarations des fonctions (si elles sont définies ailleurs)
void configure_led(void);
void blink_led(void);

void app_main(void)
{
    // Utilise ESP_LOGI au lieu de printf
    ESP_LOGI(TAG, "Hello from ESP32");

    /* Configure the peripheral according to the LED type */
    configure_led();

    // configure dht


    while (1) {
        ESP_LOGI(TAG, "Turning the LED %s!", s_led_state == true ? "ON" : "OFF");
        blink_led();
        /* Toggle the LED state */
        s_led_state = !s_led_state;
        vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
    }
}
