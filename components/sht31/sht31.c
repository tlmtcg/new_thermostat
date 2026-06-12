/**
 * @file sht31.c
 * @brief Pilote pour le capteur SHT31 (température/humidité) sur ESP32.
 * Gère l'initialisation, la lecture, les erreurs, et la récupération.
 *
 * Compatible avec ESP-IDF v6.
 */

#include "sht31.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "utils.h"
#include "i2c_shared.h"

// =========================================================================
// CONFIGURATION
// =========================================================================

/** @brief Tag pour les logs ESP. */
static const char *TAG = "SHT31_DRIVER";

/** @brief Commande pour une mesure en haute résolution sans Clock Stretching. */
#define SHT31_CMD_MEAS_HIGHREP 0x2400

/** @brief Commande pour un reset logiciel. */
#define SHT31_CMD_SOFT_RESET 0x30A2

// =========================================================================
// STRUCTURES ET VARIABLES GLOBALES
// =========================================================================

/**
 * @brief Contexte global du SHT31.
 */
typedef struct
{
    i2c_master_bus_handle_t bus; /**< Pointeur vers le bus I2C. */
    i2c_master_dev_handle_t dev; /**< Pointeur vers le périphérique I2C. */
} sht31_ctx_t;

static sht31_ctx_t g_sht31 = {};

static sht31_config_t g_sht31_config = {
    .addr = CONFIG_SHT31_DEFAULT_ADDR,
    .read_interval_ms = CONFIG_SHT31_DEFAULT_READ_INTERVAL_MS,
};

static sht31_runtime_t g_sht31_runtime = {
    .last_error = "",
};

// =========================================================================
// FONCTIONS STATIQUES (INTERNES)
// =========================================================================

/**
 * @brief Calcule le CRC8 pour les données du SHT31.
 */
static uint8_t sht31_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;

    for (int i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (int b = 0; b < 8; b++)
        {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x31;
            else
                crc <<= 1;
        }
    }

    return crc;
}

/**
 * @brief Définit le dernier message d'erreur.
 */
static void sht31_set_error(const char *message)
{
    if (!message)
        message = "";

    snprintf(
        g_sht31_runtime.last_error,
        sizeof(g_sht31_runtime.last_error),
        "%s",
        message);
}

/**
 * @brief Efface les erreurs et réinitialise les compteurs.
 */
static void sht31_clear_error(void)
{
    g_sht31_runtime.last_error[0] = '\0';
    g_sht31_runtime.last_error_code = ESP_OK;
    g_sht31_runtime.consecutive_error_count = 0;
}

/**
 * @brief Enregistre une erreur et met à jour l'état.
 */
static void sht31_record_error(esp_err_t err)
{
    g_sht31_runtime.valid = false;
    g_sht31_runtime.error_count++;
    g_sht31_runtime.consecutive_error_count++;
    g_sht31_runtime.last_error_code = err;
    g_sht31_runtime.last_error_at = time(NULL);
    sht31_set_error(esp_err_to_name(err));

    uint32_t consecutive = g_sht31_runtime.consecutive_error_count;
    if (consecutive <= CONFIG_SHT31_ERROR_LOG_FIRST_COUNT ||
        (consecutive % CONFIG_SHT31_ERROR_LOG_EVERY_COUNT) == 0)
    {
        ESP_LOGW(
            TAG,
            "Lecture SHT31 échouée: %s (consecutives=%lu, total=%lu, last_error=%s)",
            esp_err_to_name(err),
            (unsigned long)consecutive,
            (unsigned long)g_sht31_runtime.error_count,
            g_sht31_runtime.last_error);

        ESP_LOGW(TAG,
                 "Lecture SHT31 échouée: %s (%d)",
                 esp_err_to_name(err),
                 err);
    }
}

/**
 * @brief Écrit une commande au capteur SHT31 (Sécurisée en ticks bruts).
 */
static esp_err_t sht31_write_cmd(uint16_t cmd)
{
    if (!g_sht31.dev)
    {
        ESP_LOGE(TAG, "sht31_write_cmd: g_sht31.dev is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[2] = {
        (uint8_t)(cmd >> 8),
        (uint8_t)(cmd & 0xFF),
    };

    esp_err_t err = i2c_master_transmit(g_sht31.dev,
                                        data,
                                        sizeof(data),
                                        100);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "TX failed: %s",
                 esp_err_to_name(err));
    }

    return err;
}

/**
 * @brief Attache le périphérique SHT31 au bus I2C.
 */
esp_err_t sht31_attach_device(uint16_t dev_addr)
{
    if (g_sht31.bus == NULL)
    {
        ESP_LOGE(TAG, "Impossible d'attacher le peripherique : g_sht31.bus est NULL !");
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = 100000, // Aligné sur la vitesse standard stabilisée
    };

    return i2c_master_bus_add_device(g_sht31.bus, &cfg, &g_sht31.dev);
}

// =========================================================================
// FONCTIONS PUBLIQUES
// =========================================================================

/**
 * @brief Initialise le capteur SHT31.
 */
esp_err_t sht31_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (!bus || addr == 0)
    {
        ESP_LOGE(TAG, "sht31_init: Arguments invalides (bus=%p, addr=0x%02X)", bus, addr);
        return ESP_ERR_INVALID_ARG;
    }

    if (g_sht31_runtime.initialized)
    {
        if (g_sht31.bus != bus || g_sht31_config.addr != addr)
        {
            ESP_LOGW(TAG, "Configuration ou Bus I2C changé, réinitialisation du SHT31");
            sht31_deinit();
        }
        else
        {
            ESP_LOGW(TAG, "SHT31 déjà initialisé avec la même configuration");
            return ESP_OK;
        }
    }

    g_sht31.bus = bus;
    g_sht31_config.addr = addr;

    esp_err_t err = sht31_attach_device(g_sht31_config.addr);
    if (err != ESP_OK)
    {
        g_sht31_runtime.initialized = false;
        g_sht31_runtime.valid = false;
        sht31_record_error(err);
        g_sht31.bus = NULL;
        ESP_LOGE(TAG, "Erreur lors de l'attachement du device I2C: %s", esp_err_to_name(err));
        return err;
    }

    g_sht31_runtime.initialized = true;
    sht31_clear_error();

    ESP_LOGI(TAG, "SHT31 initialisé @0x%02X", g_sht31_config.addr);
    return ESP_OK;
}

/**
 * @brief Désinitialise le capteur SHT31.
 */
void sht31_deinit(void)
{
    g_sht31_runtime.running = false;

    if (g_sht31.dev)
    {
        i2c_master_bus_rm_device(g_sht31.dev);
        g_sht31.dev = NULL;
    }

    g_sht31.bus = NULL;
    g_sht31_runtime.initialized = false;
    g_sht31_runtime.valid = false;

    ESP_LOGI(TAG, "SHT31 désinitialisé");
}

/**
 * @brief Effectue un reset logiciel du capteur.
 */
esp_err_t sht31_reset(void)
{
    return sht31_write_cmd(SHT31_CMD_SOFT_RESET);
}

/**
 * @brief Tentative de récupération après une erreur.
 */
esp_err_t sht31_recover(void)
{
    if (!g_sht31.bus)
    {
        ESP_LOGE(TAG, "sht31_recover: g_sht31.bus is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(
        TAG,
        "Tentative de récupération SHT31 après %lu erreur(s) consécutive(s)",
        (unsigned long)g_sht31_runtime.consecutive_error_count);

    esp_err_t err = sht31_reset();
    if (err == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(30));
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Soft reset SHT31 impossible (%s), reconstruction du handle périphérique", esp_err_to_name(err));

    if (g_sht31.dev)
    {
        i2c_master_bus_rm_device(g_sht31.dev);
        g_sht31.dev = NULL;
    }

    err = sht31_attach_device(g_sht31_config.addr);
    if (err != ESP_OK)
    {
        g_sht31_runtime.initialized = false;
        g_sht31_runtime.valid = false;
        sht31_record_error(err);
        return err;
    }

    g_sht31_runtime.initialized = true;
    vTaskDelay(pdMS_TO_TICKS(30));
    return ESP_OK;
}

/**
 * @brief Lit les données de température et d'humidité du capteur.
 */
/**
 * @brief Lit les données du SHT31 avec masquage des erreurs transitoires (Filtre de 3 essais).
 */
esp_err_t sht31_read(float *temp, float *hum)
{
    if (!g_sht31.dev || !g_sht31_runtime.initialized)
    {
        sht31_record_error(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    if (!temp || !hum)
    {
        ESP_LOGE(TAG, "sht31_read: Pointeur de sortie NULL");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t rx[6];

    // Prise du mutext
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    // Envoi de la commande de mesure
    esp_err_t err = sht31_write_cmd(SHT31_CMD_MEAS_HIGHREP);
    if (err != ESP_OK)
        goto fail;

    // Attente de fin de conversion matérielle
    vTaskDelay(pdMS_TO_TICKS(100));

    for (int i = 0; i < 3; i++)
    {
        // Lecture de la trame (100 ticks bruts pour tolérer les salves de l'OLED)
        err = i2c_master_receive(g_sht31.dev, rx, sizeof(rx), 100);
        if (err == ESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "RX failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    // Vérification du CRC
    if (sht31_crc8(&rx[0], 2) != rx[2] || sht31_crc8(&rx[3], 2) != rx[5])
    {
        err = ESP_ERR_INVALID_CRC;
        goto fail;
    }

    // Extraction des données en cas de succès total
    uint16_t raw_t = ((uint16_t)rx[0] << 8) | rx[1];
    uint16_t raw_h = ((uint16_t)rx[3] << 8) | rx[4];

    *temp = -45.0f + (175.0f * ((float)raw_t / 65535.0f));
    *hum = 100.0f * ((float)raw_h / 65535.0f);

    g_sht31_runtime.temperature = *temp;
    g_sht31_runtime.humidity = *hum;
    g_sht31_runtime.valid = true;
    g_sht31_runtime.last_update = time(NULL);
    g_sht31_runtime.last_success_at = g_sht31_runtime.last_update;
    g_sht31_runtime.read_count++;

    // Nettoyage immédiat du compteur d'erreurs consécutives en cas de succès
    sht31_clear_error();
    xSemaphoreGive(g_i2c_mutex);

    return ESP_OK;

fail:

    xSemaphoreGive(g_i2c_mutex);
    // Enregistre l'échec en interne (incrémente g_sht31_runtime.consecutive_error_count)
    sht31_record_error(err);

    // CORRECTION CRITIQUE : On masque l'erreur vis-à-vis de la tâche supérieure
    // Tant qu'on a moins de 3 échecs consécutifs, on renvoie ESP_OK pour éviter l'alerte panic.
    if (g_sht31_runtime.consecutive_error_count < 3)
    {
        ESP_LOGD(TAG, "Erreur I2C masquée (essai %lu/3), conservation des anciennes valeurs",
                 (unsigned long)g_sht31_runtime.consecutive_error_count);

        // On maintient les anciennes valeurs valides pour ne pas perturber le thermostat
        *temp = g_sht31_runtime.temperature;
        *hum = g_sht31_runtime.humidity;

        return ESP_OK;
    }

    // Si on arrive ici, c'est le 3ème échec consécutif : on transmet la vraie erreur pour déclarer la panne
    return err;
}

/**
 * @brief Démarre le capteur SHT31.
 */
esp_err_t sht31_start(i2c_master_bus_handle_t bus, uint8_t addr)
{
    esp_err_t err = sht31_init(bus, addr);
    if (err != ESP_OK)
        return err;

    g_sht31_runtime.running = true;
    ESP_LOGI(TAG, "SHT31 actif");
    return ESP_OK;
}

/**
 * @brief Arrête le capteur SHT31.
 */
void sht31_stop(void)
{
    g_sht31_runtime.running = false;
    ESP_LOGI(TAG, "SHT31 arrêté");
}

/**
 * @brief Récupère la configuration actuelle.
 */
esp_err_t sht31_get_config(sht31_config_t *out)
{
    if (!out)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = g_sht31_config;
    return ESP_OK;
}

/**
 * @brief Définit la configuration du capteur.
 */
esp_err_t sht31_set_config(const sht31_config_t *config)
{
    if (!config || config->addr == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    sht31_config_t new_config = *config;

    if (new_config.read_interval_ms == 0)
    {
        new_config.read_interval_ms = CONFIG_SHT31_DEFAULT_READ_INTERVAL_MS;
    }

    bool addr_changed = new_config.addr != g_sht31_config.addr;
    if (addr_changed && g_sht31_runtime.initialized)
    {
        if (g_sht31.dev)
        {
            i2c_master_bus_rm_device(g_sht31.dev);
            g_sht31.dev = NULL;
        }

        esp_err_t err = sht31_attach_device(new_config.addr);
        if (err != ESP_OK)
        {
            g_sht31_runtime.initialized = false;
            g_sht31_runtime.valid = false;
            sht31_record_error(err);
            return err;
        }

        g_sht31_runtime.valid = false;
        sht31_clear_error();
    }

    g_sht31_config = new_config;
    return ESP_OK;
}

/**
 * @brief Récupère l'état d'exécution actuel.
 */
const sht31_runtime_t *sht31_get_runtime(void)
{
    return &g_sht31_runtime;
}

/**
 * @brief Définit l'état "running" du capteur.
 */
void sht31_set_running(bool running)
{
    g_sht31_runtime.running = running;
}

/**
 * @brief Récupère l'état complet du capteur au format JSON.
 */
char *sht31_get_json_status(void)
{
    // CORRECTION : Remplacement de get_ms() par time(NULL) pour correspondre au type Epoch time_t de la structure
    time_t now = time(NULL);
    g_sht31_runtime.last_update = now;

    const char *json_format =
        "{\"runtime\":{\"temperature\":%.1f,\"humidity\":%.1f,\"valid\":%s,\"initialized\":%s,\"running\":%s,"
        "\"read_count\":%lu,\"error_count\":%lu,\"consecutive_error_count\":%lu,\"last_error_code\":%d,"
        "\"last_error_at\":%lld,\"last_success_at\":%lld,\"last_update\":%lld,\"last_error\":\"%s\"},"
        "\"config\":{\"gpio_pin\":0x%X,\"read_interval_ms\":%lu}}";

    return format_json_alloc(json_format,
                             g_sht31_runtime.temperature, g_sht31_runtime.humidity,
                             g_sht31_runtime.valid ? "true" : "false",
                             g_sht31_runtime.initialized ? "true" : "false",
                             g_sht31_runtime.running ? "true" : "false",
                             (unsigned long)g_sht31_runtime.read_count,
                             (unsigned long)g_sht31_runtime.error_count,
                             (unsigned long)g_sht31_runtime.consecutive_error_count,
                             g_sht31_runtime.last_error_code,
                             (long long)g_sht31_runtime.last_error_at,
                             (long long)g_sht31_runtime.last_success_at,
                             (long long)g_sht31_runtime.last_update,
                             g_sht31_runtime.last_error,
                             g_sht31_config.addr,
                             (unsigned long)g_sht31_config.read_interval_ms);
}

/**
 * @brief Réinitialise les compteurs d’erreurs du SHT31.
 */
void sht31_reset_error_counter(void)
{
    sht31_clear_error();
}

static i2c_master_bus_handle_t sht31_bus;
static uint8_t sht31_addr;

esp_err_t sht31_component_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    sht31_bus = bus;
    sht31_addr = addr;

    return sht31_init(bus, addr);
}
