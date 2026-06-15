#include "system_manager.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

// ✅ Variable globale pour vérifier si le WDT est déjà initialisé
static bool s_wdt_initialized = false;

void app_main(void) {
    // Initialisation de la NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ✅ Vérifie si le WDT est déjà initialisé
    if (!s_wdt_initialized) {
        esp_task_wdt_config_t wdt_config = {
            .timeout_ms = 30000  // 30 secondes
        };
        esp_task_wdt_init(&wdt_config);
        s_wdt_initialized = true;  // Marque le WDT comme initialisé
    }

    // Démarre le système
    system_manager_start();
}
