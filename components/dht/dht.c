#include "dht.h"
#include "rom/ets_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "utils.h"

static const char *TAG = "DHT_DRIVER";

// Variables globales internes pour maintenir l'état (Style SHT31)
static dht_config_t active_config = {
    .gpio_pin = CONFIG_DHT_GPIO_PIN,
    .sensor_type = CONFIG_DHT_TYPE,
    .read_interval_ms = CONFIG_DHT_READ_INTERVAL_MS};

static dht_runtime_t active_runtime = {
    .temperature = 0.0f,
    .humidity = 0.0f,
    .valid = false,
    .initialized = true,
    .running = true,
    .read_count = 0,
    .error_count = 0,
    .consecutive_error_count = 0,
    .last_error_code = ESP_OK,
    .last_error_at = 0,
    .last_success_at = 0,
    .last_update = 0,
    .last_error = "Aucun"};

// Attente de changement d'état d'une broche avec timeout microsecondes
static inline esp_err_t dht_wait_level(gpio_num_t gpio_num, uint32_t timeout_us, uint32_t level, uint32_t *duration)
{
    uint64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == level)
    {
        if ((esp_timer_get_time() - start) > timeout_us)
        {
            return ESP_ERR_TIMEOUT;
        }
    }
    if (duration)
    {
        *duration = (uint32_t)(esp_timer_get_time() - start);
    }
    return ESP_OK;
}

esp_err_t dht_read_data(gpio_num_t gpio_num, dht_sensor_type_t type, float *humidity, float *temperature)
{
    // Sécurité pointeurs
    if (!humidity || !temperature)
        return ESP_ERR_INVALID_ARG;

    uint8_t data[5] = {0};
    uint32_t duration = 0;

    // --- ENTRÉE EN ZONE CRITIQUE DE TIMING ---
    // Mémorisation de la priorité actuelle de la tâche appelante
    UBaseType_t old_prio = uxTaskPriorityGet(NULL);
    // Forçage de la priorité au maximum pour figer le scheduler FreeRTOS
    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);

    // 1. Signal de Start généré par l'ESP32
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(gpio_num, 0);

    ets_delay_us(type == DHT_TYPE_DHT11 ? 20000 : 2000);

    gpio_set_level(gpio_num, 1);
    ets_delay_us(40);

    // 2. Commutation de la broche en entrée
    gpio_set_direction(gpio_num, GPIO_MODE_INPUT);

    // Poignée de main (Handshake) du DHT
    if (dht_wait_level(gpio_num, 80, 1, NULL) != ESP_OK)
    {
        vTaskPrioritySet(NULL, old_prio); // <--- Toujours restaurer avant de quitter en erreur
        return ESP_ERR_TIMEOUT;
    }
    if (dht_wait_level(gpio_num, 90, 0, NULL) != ESP_OK)
    {
        vTaskPrioritySet(NULL, old_prio);
        return ESP_ERR_TIMEOUT;
    }
    if (dht_wait_level(gpio_num, 90, 1, NULL) != ESP_OK)
    {
        vTaskPrioritySet(NULL, old_prio);
        return ESP_ERR_TIMEOUT;
    }

    // 3. Extraction des 40 bits
    for (int i = 0; i < 40; i++)
    {
        if (dht_wait_level(gpio_num, 60, 0, NULL) != ESP_OK)
        {
            vTaskPrioritySet(NULL, old_prio);
            return ESP_ERR_TIMEOUT;
        }
        if (dht_wait_level(gpio_num, 80, 1, &duration) != ESP_OK)
        {
            vTaskPrioritySet(NULL, old_prio);
            return ESP_ERR_TIMEOUT;
        }

        data[i / 8] <<= 1;
        if (duration > 40)
        {
            data[i / 8] |= 1;
        }
    }

    // --- SORTIE DE ZONE CRITIQUE ---
    // Les phases critiques temporelles sont finies, le calcul peut être interrompu sans danger
    vTaskPrioritySet(NULL, old_prio);

    // 4. Validation du Checksum
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF))
    {
        return ESP_ERR_INVALID_CRC;
    }

    // 5. Interprétation des grandeurs physiques
    if (type == DHT_TYPE_DHT11)
    {
        *humidity = (float)data[0];
        *temperature = (float)data[2];
        if (data[1] < 10)
            *humidity += (float)data[1] * 0.1f;
        if (data[3] < 10)
            *temperature += (float)data[3] * 0.1f;
    }
    else
    {
        float h = (float)((data[0] << 8) | data[1]) * 0.1f;
        float t = (float)((data[2] & 0x7F) << 8 | data[3]) * 0.1f;
        if (data[2] & 0x80)
            t = -t;
        *humidity = h;
        *temperature = t;
    }

    // Mise à jour de l'état runtime
    active_runtime.temperature = *temperature;
    active_runtime.humidity = *humidity;
    active_runtime.valid = true;
    active_runtime.read_count++;
    active_runtime.last_success_at = time(NULL);
    active_runtime.last_update = time(NULL);
    strcpy(active_runtime.last_error, "Aucun");

    return ESP_OK;
}

// Getters et Setters pour la configuration (Appelés par l'API POST)
esp_err_t dht_get_config(dht_config_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    memcpy(out, &active_config, sizeof(dht_config_t));
    return ESP_OK;
}

esp_err_t dht_set_config(const dht_config_t *config)
{
    if (!config)
        return ESP_ERR_INVALID_ARG;

    // Sécurité : évite une broche invalide
    if (config->gpio_pin == 0)
        return ESP_ERR_INVALID_ARG;

    // 1. On crée une copie locale de travail
    dht_config_t new_config = *config;

    // 2. CORRECTION : Si l'intervalle est à 0, on applique une valeur par défaut cohérente (ex: 2000 ms)
    if (new_config.read_interval_ms == 0)
    {
        new_config.read_interval_ms = 2000;
    }

    // 3. CORRECTION : On copie la structure modifiée (new_config) dans la config active
    memcpy(&active_config, &new_config, sizeof(dht_config_t));

    ESP_LOGI(TAG,
             "Config appliquee (gpio_pin=0x%02X, interval=%u ms)",
             active_config.gpio_pin,
             (unsigned)active_config.read_interval_ms);

    return ESP_OK;
}

// Récupération de l'état runtime brut
const dht_runtime_t *dht_get_runtime(void)
{
    return &active_runtime;
}

// --- Fonction principale ---
char *dht_get_json_status(void)
{
    // Synchronisation du timestamp
    uint32_t now = get_ms();
    active_runtime.last_update = now;

    const char *json_format = 
        "{\"runtime\":{\"temperature\":%.1f,\"humidity\":%.1f,\"valid\":%s,\"initialized\":%s,\"running\":%s,"
        "\"read_count\":%lu,\"error_count\":%lu,\"consecutive_error_count\":%lu,\"last_error_code\":%d,"
        "\"last_error_at\":%lld,\"last_success_at\":%lld,\"last_update\":%lld,\"last_error\":\"%s\"},"
        "\"config\":{\"gpio_pin\":%d,\"sensor_type\":%s,\"read_interval_ms\":%lu}}";

    // Appel unique : la liste d'arguments n'est écrite qu'une seule fois ici
    return format_json_alloc(json_format,
             active_runtime.temperature, active_runtime.humidity,
             active_runtime.valid ? "true" : "false",
             active_runtime.initialized ? "true" : "false",
             active_runtime.running ? "true" : "false",
             (unsigned long)active_runtime.read_count,
             (unsigned long)active_runtime.error_count,
             (unsigned long)active_runtime.consecutive_error_count,
             active_runtime.last_error_code,
             (long long)active_runtime.last_error_at,
             (long long)active_runtime.last_success_at,
             (long long)active_runtime.last_update,
             active_runtime.last_error,
             active_config.gpio_pin,
             active_config.sensor_type == 0 ? "\"DHT11\"" : "\"DHT22\"",
             (unsigned long)active_config.read_interval_ms);
}

esp_err_t dht_init(gpio_num_t gpio, dht_sensor_type_t type)
{
    active_config.gpio_pin = gpio;
    active_config.sensor_type = type;
    gpio_reset_pin(gpio);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY); // 👈 Gardé comme dans ton original
    return ESP_OK;
}

esp_err_t dht_perform_measurement(void)
{
    float temperature = 0.0f;
    float humidity = 0.0f;

    esp_err_t err = dht_read_data(
        active_config.gpio_pin,
        active_config.sensor_type,
        &humidity,
        &temperature);

    if (err != ESP_OK)
    {
        active_runtime.valid = false;
        active_runtime.error_count++;
        active_runtime.consecutive_error_count++;
        active_runtime.last_error_code = err;
        active_runtime.last_error_at = time(NULL);

        snprintf(active_runtime.last_error,
                 sizeof(active_runtime.last_error),
                 "Erreur DHT (%s)",
                 esp_err_to_name(err));

        ESP_LOGW(TAG,
                 "Lecture DHT echouee: %s (%lu/%d)",
                 esp_err_to_name(err),
                 (unsigned long)active_runtime.consecutive_error_count,
                 CONFIG_DHT_PANNE_SEUIL_CONSECUTIF);
    

        // 👉 Détection panne capteur
        if (active_runtime.consecutive_error_count >= CONFIG_DHT_PANNE_SEUIL_CONSECUTIF)
        {
            active_runtime.recovery_attempts++;

            ESP_LOGW(TAG, "Tentative auto-recovery #%lu",
                     active_runtime.recovery_attempts);

            gpio_reset_pin(active_config.gpio_pin);
            gpio_set_direction(active_config.gpio_pin, GPIO_MODE_INPUT);
            gpio_set_pull_mode(active_config.gpio_pin, GPIO_PULLUP_ONLY);

            vTaskDelay(pdMS_TO_TICKS(100));

            active_runtime.consecutive_error_count = 0; // reset pour retry
        }

        return err;
    }

    // ✅ SUCCESS
    active_runtime.temperature = temperature;
    active_runtime.humidity = humidity;
    active_runtime.valid = true;
    active_runtime.running = true;

    active_runtime.consecutive_error_count = 0;
    active_runtime.last_error_code = ESP_OK;
    active_runtime.last_success_at = time(NULL);

    strncpy(active_runtime.last_error, "Aucun", sizeof(active_runtime.last_error));

    ESP_LOGD(TAG,
             "Mesure OK: %.1f°C %.1f%%RH",
             temperature,
             humidity);

    return ESP_OK;
}
