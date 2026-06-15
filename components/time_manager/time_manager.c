#include "time_manager.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "time_manager_storage.h"
#include <math.h>
#include "utils.h"
#include "freertos/semphr.h"
#include "event_bus.h"

static const char *TAG = "TIME_MANAGER";

// Ajout d'un mutex pour protéger l'accès aux variables partagées
static SemaphoreHandle_t time_mutex = NULL;

// Variables statiques privées au fichier
static time_t s_last_sync = 0;
static time_t s_board_time_before_sync = 0; // Mémorise le temps de la carte juste avant la requête
static time_status_t s_time_status = {0};

time_manager_config_t cfg;

// Fonction pour que les autres composants lisent l'état
void time_manager_get_status(time_status_t *dest)
{
    if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(10)))
    {
        memcpy(dest, &s_time_status, sizeof(time_status_t));
        xSemaphoreGive(time_mutex);
    }
}

/* -------------------------------------------------------------------------- */
/*  CALLBACK SNTP                                                             */
/* -------------------------------------------------------------------------- */

static void time_sync_notification_cb(struct timeval *tv)
{
    struct tm now = time_utils_get_local_time();

    ESP_LOGI(TAG,
             "SNTP OK : %04d-%02d-%02d %02d:%02d:%02d",
             now.tm_year + 1900,
             now.tm_mon + 1,
             now.tm_mday,
             now.tm_hour,
             now.tm_min,
             now.tm_sec);

    s_last_sync = tv->tv_sec;
    s_time_status.last_sync_time = (uint32_t)tv->tv_sec;

    // Calcul de l'écart réel si on a mémorisé l'heure avant l'interrogation
    if (s_board_time_before_sync > 0)
    {
        int32_t drift = (int32_t)(tv->tv_sec - s_board_time_before_sync);
        if (llabs((long long)drift) < 86400)
        {
            ESP_LOGI("time_manager", "Synchronisation SNTP réussie. Écart réel mesuré : %ld secondes", drift);
        }
        s_board_time_before_sync = 0; // Réinitialisation
    }
    else
    {
        ESP_LOGI(TAG, "Synchronisation SNTP réussie");
    }

    // Notification système
    event_t evt;
    evt.type = EVENT_NET_TIME_SYNCED;
    event_bus_publish(&evt);

    ESP_LOGI(TAG, "Notification de synchronisation envoyée.");
}

/* -------------------------------------------------------------------------- */
/*  INITIALISATION                                                            */
/* -------------------------------------------------------------------------- */

esp_err_t time_manager_init(void)
{
    time_mutex = xSemaphoreCreateMutex();
    if (!time_mutex)
        return ESP_ERR_NO_MEM;

    // 1. Charger la config
    if (!time_manager_storage_load(&cfg))
    {
        ESP_LOGW(TAG, "Config NVS introuvable, usage par défaut");
        strlcpy(cfg.ntp_server, CONFIG_SNTP_SERVER_NAME, sizeof(cfg.ntp_server));
        cfg.ntp_max_retry = CONFIG_SNTP_MAX_RETRY;
    }

    // 2. Setup Zone Horaire (Indépendant du réseau)
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    ESP_LOGI(TAG, "Time Manager initialisé (en attente du WiFi)");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  UTILITAIRES TEMPS                                                         */
/* -------------------------------------------------------------------------- */

struct tm time_manager_get_local_time(void)
{
    time_t now;
    struct tm info;
    time(&now);
    localtime_r(&now, &info);
    return info;
}

uint64_t time_manager_get_timestamp(void)
{
    return (uint64_t)time(NULL);
}

void time_manager_get_time_str(char *dest, size_t max_size)
{
    struct tm info = time_manager_get_local_time();
    strftime(dest, max_size, "%d/%m/%Y %H:%M:%S", &info);
}

void time_manager_get_complete_time_str(char *dest, size_t max_size)
{
    // Récupération du temps local (remplacez par votre fonction réelle si nécessaire)
    struct tm info = time_manager_get_local_time();

    char date_time_part[32];

    // Sécurisation de l'index
    int day_idx = (info.tm_wday >= 0 && info.tm_wday <= 6) ? info.tm_wday : 0;

    // 1. On génère le reste de la date et l'heure (ex: "02/06/2026 10:45:00")
    strftime(date_time_part, sizeof(date_time_part), "%d/%m/%Y %H:%M:%S", &info);

    // 2. On combine le jour en français (via info.tm_wday qui va de 0 à 6) avec le reste
    // info.tm_wday : 0 = Dimanche, 1 = Lundi, etc.
    snprintf(dest, max_size, "%s %s", JOURS_FR[day_idx], date_time_part);
}

time_t time_manager_get_last_sync(void)
{
    return s_last_sync;
}

void time_manager_status_dump()
{
    ESP_LOGI(TAG, "=== SNTP STATUS DUMP ===");
    ESP_LOGI(TAG, "NTP Server: %s", cfg.ntp_server);
    ESP_LOGI(TAG, "Max Retry: %d", cfg.ntp_max_retry);
    ESP_LOGI(TAG, "Sync Interval (sec): %d", cfg.ntp_sync_interval_sec);
    ESP_LOGI(TAG, "Current Retry: %d", s_time_status.current_retry);
    ESP_LOGI(TAG, "Is Syncing: %s", s_time_status.is_syncing ? "Yes" : "No");
    if (s_time_status.last_sync_time != 0)
    {
        char time_str[64];
        time_manager_get_time_str(time_str, sizeof(time_str));
        ESP_LOGI(TAG, "Last Sync Time: %s", time_str);
    }
    else
    {
        ESP_LOGI(TAG, "Last Sync Time: Never");
    }
}

void time_manager_get_hour_str(char *dest, size_t max)
{
    char full[32];
    time_manager_get_time_str(full, sizeof(full));
    strncpy(dest, full + 11, max);
}

struct tm time_manager_localtime_from_ts(int64_t ts)
{
    struct tm info;
    time_t t = (time_t)ts;
    localtime_r(&t, &info);
    return info;
}

/**
 * @brief Vérifie l'écart entre la carte et le temps réel. Réaligne si l'écart dépasse 1 minute.
 * @param real_timestamp
 *  */
void time_manager_check_and_sync(uint64_t real_timestamp)
{
    uint64_t board_time = (uint64_t)time(NULL);
    int32_t drift = (int32_t)(real_timestamp - board_time);

    // Si la carte avance ou retarde de plus de 60 secondes (1 minute)
    if (abs(drift) >= 60)
    {
        ESP_LOGW(TAG, "Dérive importante détectée (%ld sec). Resynchronisation forcée...", drift);

        struct timeval tv = {
            .tv_sec = (time_t)real_timestamp,
            .tv_usec = 0};

        if (xSemaphoreTake(time_mutex, portMAX_DELAY))
        {
            settimeofday(&tv, NULL);
            s_last_sync = tv.tv_sec;
            s_time_status.last_sync_time = (uint32_t)tv.tv_sec;
            xSemaphoreGive(time_mutex);
        }
    }
    else
    {
        ESP_LOGD(TAG, "Écart temporel négligeable (%ld sec). Pas de réalignement nécessaire.", drift);
    }
}

/**
 * @brief Prépare le système à enregistrer la dérive en sauvegardant l'heure courante de la carte avant la synchro.
 */
void time_manager_prepare_for_sync(void)
{
    // On enregistre le timestamp Unix actuel de la carte (qui peut être décalé)
    s_board_time_before_sync = time(NULL);

    ESP_LOGD("time_manager", "Heure de la carte mémorisée avant synchro : %ld", s_board_time_before_sync);
}

time_t parse_iso8601_to_epoch(const char *iso)
{
    struct tm t = {0};

    // Format attendu : "2026-06-06T07:45"
    if (sscanf(iso, "%d-%d-%dT%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min) != 5)
    {
        return 0; // erreur
    }

    t.tm_year -= 1900; // struct tm = années depuis 1900
    t.tm_mon -= 1;     // struct tm = mois 0-11

    return mktime(&t);
}

void time_manager_start_sntp(void)
{
    if (esp_sntp_enabled())
        return; // Déjà actif

    ESP_LOGI(TAG, "Démarrage SNTP : %s", cfg.ntp_server);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, cfg.ntp_server);

    // Demande à l'ESP de se resynchroniser périodiquement
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    // On notifie le système qu'on commence
    s_time_status.is_syncing = true;

    // Note : Ne pas faire de boucle d'attente bloquante ici.
    // Le callback 'time_sync_notification_cb' gérera la suite.
}

struct tm time_utils_get_local_time(void)
{
    time_t now;
    struct tm info;
    time(&now);
    localtime_r(&now, &info);

    return info;
}
