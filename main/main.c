#include "system_manager.h"
#include "nvs_flash.h"
#include "esp_log.h"

void app_main(void)
{
// =========================================================================
    // CORRECTION CRITIQUE : Initialisation de la NVS avant le System Manager
    // =========================================================================
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // En cas de corruption ou de changement de version, on nettoie et réinitialise
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Maintenant que la Flash est prête pour le calendrier de chauffage, on démarre
    system_manager_start();
}