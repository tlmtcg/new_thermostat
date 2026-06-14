#include "heating_program.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include "time_manager.h"

static const char *TAG = "HEATING_PRG";

/* =========================================================
 * CONFIG GLOBALE (UNE SEULE INSTANCE EN RAM)
 * ========================================================= */
static chauffage_config_t config;

/* =========================================================
 * SET POINT (PROGRAMMATION)
 * ========================================================= */
esp_err_t heating_set_point(jour_t j, int index, int h, int m, int s, float temp)
{
    if (j >= NB_JOURS || index >= NB_PLAGES)
        return ESP_ERR_INVALID_ARG;

    if (h > 23 || m > 59 || s > 59)
        return ESP_ERR_INVALID_ARG;

    config.planning[j][index].secondes_minuit = (uint32_t)(h * 3600 + m * 60 + s);
    config.planning[j][index].temperature = temp;

    return ESP_OK;
}

/* =========================================================
 * TEMPERATURE SELON PLANNING
 * ========================================================= */
float heating_get_temp(jour_t j, uint32_t now_sec)
{
    if (j >= NB_JOURS)
        return -1.0f;

    float temp_cible = config.planning[j][NB_PLAGES - 1].temperature;
    uint32_t dernier_seuil = 0;

    for (int i = 0; i < NB_PLAGES; i++)
    {
        uint32_t seuil = config.planning[j][i].secondes_minuit;

        if (now_sec >= seuil && seuil >= dernier_seuil)
        {
            dernier_seuil = seuil;
            temp_cible = config.planning[j][i].temperature;
        }
    }

    return temp_cible;
}

/* =========================================================
 * GESTION DES MODES ET LOGIQUE DE CONSIGNE DYNAMIQUE
 * ========================================================= */

void heating_set_mode(heating_mode_t new_mode)
{
    if (new_mode >= HEATING_MODE_COUNT) return;
    config.mode = new_mode;
    ESP_LOGI(TAG, "Nouveau mode de chauffage selectionne : %d", new_mode);
}

heating_mode_t heating_get_mode(void)
{
    return config.mode;
}

void heating_set_manual_target(float temp)
{
    config.manual_target = temp;
    ESP_LOGI(TAG, "Nouvelle consigne manuelle fixee : %.1f C", temp);
}

float heating_get_manual_target(void)
{
    return config.manual_target;
}

/**
 * @brief Calcule la consigne dynamique en fonction du mode et de la température extérieure
 * @param ext_temp Température extérieure actuelle (nécessaire pour la loi d'éco du mode Hors-gel)
 * @return La consigne de température calculée en °C
 */
float heating_calculate_target_temperature(float ext_temp)
{
    switch (config.mode)
    {
        case HEATING_MODE_MANUAL:
            // Mode Manuel : On applique strictement la consigne manuelle utilisateur
            return config.manual_target;

        case HEATING_MODE_ABSENT:
            // Mode Absent : Auto - 4°C
            return heating_get_temp_current() - 4.0f;

        case HEATING_MODE_HORS_GEL:
            // Mode Hors-Gel : Température intérieure indexée sur l'extérieur pour économies
            // Si Ext < 0°C  -> On maintient 12°C à l'intérieur (sécurité tuyaux gelés)
            // Si Ext 0-10°C -> Loi linéaire décroissante (Plus il fait doux dehors, moins on chauffe)
            // Si Ext > 10°C -> Plus besoin de maintenir un hors-gel élevé (8°C de base)
            if (ext_temp < 0.0f) {
                return 12.0f; 
            } else if (ext_temp >= 0.0f && ext_temp <= 10.0f) {
                // Équation de droite : à 0°C -> 12°C int. À 10°C -> 8°C int.
                return 12.0f - (ext_temp * 0.4f); 
            } else {
                return 8.0f;
            }

        case HEATING_MODE_AUTO:
        default:
            // Mode Auto : On suit le planning horaire/jour standard
            return heating_get_temp_current();
    }
}

/* =========================================================
 * INIT NVS + CONFIG
 * ========================================================= */
esp_err_t heating_init(void)
{
    nvs_handle_t h;
    esp_err_t err;

    /* 1. INIT RAM par défaut */
    memset(&config, 0, sizeof(config));
    config.mode = HEATING_MODE_AUTO;       // Mode auto par défaut
    config.manual_target = 20.0f;          // Consigne manuelle de repli

    for (int d = 0; d < NB_JOURS; d++)
    {
        for (int p = 0; p < NB_PLAGES; p++)
        {
            config.planning[d][p].temperature = 17.0f;
            config.planning[d][p].secondes_minuit = p * 14400; // Toutes les 4h
        }
    }

    /* 2. OUVERTURE NVS */
    err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS open error: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. LECTURE CONFIG */
    size_t size = sizeof(chauffage_config_t);
    err = nvs_get_blob(h, "heat_v5", &config, &size);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Config chargee (%u bytes). Mode: %d", (unsigned int)size, config.mode);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "NVS vide -> save defaults");
        nvs_set_blob(h, "heat_v5", &config, sizeof(chauffage_config_t));
        nvs_commit(h);
    }
    else
    {
        ESP_LOGE(TAG, "NVS read error: %s", esp_err_to_name(err));
    }

    nvs_close(h);
    return ESP_OK;
}

/* =========================================================
 * SAVE CONFIG
 * ========================================================= */
esp_err_t heating_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    err = nvs_set_blob(h, "heat_v5", &config, sizeof(chauffage_config_t));
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

/* =========================================================
 * RESET DEFAULTS
 * ========================================================= */
void heating_reset_defaults(void)
{
    float temps[] = {21.0f, 20.0f, 21.0f, 17.0f};
    uint32_t heures[] = {6, 12, 18, 22};

    config.mode = HEATING_MODE_AUTO;
    config.manual_target = 20.0f;

    for (int d = 0; d < NB_JOURS; d++)
    {
        for (int p = 0; p < 4; p++)
        {
            config.planning[d][p].temperature = temps[p];
            config.planning[d][p].secondes_minuit = heures[p] * 3600;
        }
    }
}

/* =========================================================
 * JSON EXPORT (CORRIGÉ POUR ÉVITER LES LEAKS)
 * ========================================================= */
char *heating_get_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "mode", config.mode);
    cJSON_AddNumberToObject(root, "manual_target", config.manual_target);

    cJSON *days = cJSON_CreateArray();
    if (!days) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "planning", days);

    for (int d = 0; d < NB_JOURS; d++)
    {
        cJSON *day = cJSON_CreateObject();
        if (!day) goto fail;
        cJSON_AddItemToArray(days, day);

        cJSON_AddNumberToObject(day, "day_idx", d);

        cJSON *slots = cJSON_CreateArray();
        if (!slots) goto fail;
        cJSON_AddItemToObject(day, "slots", slots);

        for (int p = 0; p < NB_PLAGES; p++)
        {
            cJSON *slot = cJSON_CreateObject();
            if (!slot) goto fail;
            cJSON_AddItemToArray(slots, slot);

            cJSON_AddNumberToObject(slot, "id", p);
            cJSON_AddNumberToObject(slot, "time", config.planning[d][p].secondes_minuit);
            cJSON_AddNumberToObject(slot, "temp", config.planning[d][p].temperature);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;

fail:
    ESP_LOGE(TAG, "Erreur memoire lors de la construction du cJSON");
    cJSON_Delete(root);
    return NULL;
}

/* =========================================================
 * TEMPERATURE ACTUELLE (LIVE)
 * ========================================================= */
float heating_get_temp_current()
{
    struct tm t = time_utils_get_local_time();
    int j = (t.tm_wday + 6) % 7; 

    if (j >= NB_JOURS)
        return -1.0f;

    uint32_t now_sec = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    return heating_get_temp((jour_t)j, now_sec);
}

/* =========================================================
 * ACCES A LA CONFIG GLOBALE
 * ========================================================= */
const chauffage_config_t *heating_get_config(void)
{
    return &config;
}

chauffage_config_t *heating_get_config_rw(void)
{
    return &config;
}

esp_err_t heating_get_program_json(char **out_json)
{
    if (!out_json)
        return ESP_FAIL;

    *out_json = heating_get_json();
    if (*out_json == NULL)
        return ESP_FAIL;

    return ESP_OK;
}

esp_err_t heating_reset_program(void)
{
    heating_reset_defaults();
    return heating_save();
}

int64_t heating_program_get_next_target_timestamp(void)
{
    struct tm now_tm = time_utils_get_local_time();
    time_t now_ts = mktime(&now_tm);

    int cur_day = (now_tm.tm_wday + 6) % 7;
    uint32_t sec_since_midnight = now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec;

    for (int p = 0; p < NB_PLAGES; ++p)
    {
        uint32_t s = config.planning[cur_day][p].secondes_minuit;

        if (s <= sec_since_midnight) {
            continue;
        }

        struct tm slot_tm = now_tm;
        slot_tm.tm_hour = s / 3600;
        slot_tm.tm_min  = (s % 3600) / 60;
        slot_tm.tm_sec  = s % 60;

        time_t slot_ts = mktime(&slot_tm);

        if (slot_ts > now_ts) {
            return slot_ts;
        }
    }

    int next_day = (cur_day + 1) % 7;
    struct tm tomorrow_tm = now_tm;
    tomorrow_tm.tm_mday += 1;
    tomorrow_tm.tm_hour = 0;
    tomorrow_tm.tm_min  = 0;
    tomorrow_tm.tm_sec  = 0;

    int64_t best_ts = -1;

    for (int p = 0; p < NB_PLAGES; ++p)
    {
        uint32_t s = config.planning[next_day][p].secondes_minuit;

        struct tm slot_tm = tomorrow_tm;
        slot_tm.tm_hour = s / 3600;
        slot_tm.tm_min  = (s % 3600) / 60;
        slot_tm.tm_sec  = s % 60;

        time_t slot_ts = mktime(&slot_tm);

        if (best_ts < 0 || slot_ts < best_ts)
            best_ts = slot_ts;
    }

    return best_ts;
}