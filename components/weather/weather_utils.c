#include "weather_utils.h"
#include "esp_log.h"
#include "weather.h"
#include <time.h>

static const char *TAG = "WEATHER_UTILS";

// Tableaux de traduction FR
static const char *jours_fr[] = {
    "Dimanche", "Lundi", "Mardi", "Mercredi",
    "Jeudi", "Vendredi", "Samedi"
};

static const char *mois_fr[] = {
    "Janvier", "Février", "Mars", "Avril", "Mai", "Juin",
    "Juillet", "Août", "Septembre", "Octobre", "Novembre", "Décembre"
};

void log_weather_entry(const char *label, const weather_entry_t *entry)
{
    if (!entry) {
        ESP_LOGW(TAG, "Entrée météo NULL pour %s", label);
        return;
    }

    struct tm timeinfo;
    time_t t = (time_t)entry->timestamp;

    // Conversion timestamp → struct tm locale
    localtime_r(&t, &timeinfo);

    ESP_LOGI(TAG, "=== %s ===", label);

    ESP_LOGI(TAG, "Date : %s %d %s, %02d:%02d",
             jours_fr[timeinfo.tm_wday],
             timeinfo.tm_mday,
             mois_fr[timeinfo.tm_mon],
             timeinfo.tm_hour,
             timeinfo.tm_min);

    ESP_LOGI(TAG, "Temp : %.1f°C", entry->temperature);
    ESP_LOGI(TAG, "Hum  : %.1f%%", entry->humidity);
    ESP_LOGI(TAG, "Etat : %d (%s)",
             entry->weather_code,
             get_weather_description(entry->weather_code));
}
