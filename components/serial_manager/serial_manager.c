#include "serial_manager.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "relay.h"
#include "sht31.h"
#include "thermostat_task.h"
#include "heating_program.h"

static const char *TAG = "SERIAL_MGR";

#define SERIAL_UART UART_NUM_0
#define SERIAL_BUF_SIZE 512
#define SERIAL_ENABLE_BIT (1 << 5)

static const char *skip_spaces(const char *s)
{
    while (s && isspace((unsigned char)*s))
        s++;

    return s;
}

static bool starts_with(const char *s, const char *prefix)
{
    size_t len = strlen(prefix);
    return strncmp(s, prefix, len) == 0;
}

static void print_json_alloc(char *json)
{
    if (!json)
    {
        printf("{\"success\":false,\"error\":\"json unavailable\"}\n");
        return;
    }

    printf("%s\n", json);
    free(json);
}

static void print_error(const char *message)
{
    printf("{\"success\":false,\"error\":\"%s\"}\n", message);
}

static void print_ok(void)
{
    printf("{\"success\":true}\n");
}

static void do_help(void)
{
    printf("\nCommandes JSON disponibles:\n");
    printf("  HELP\n");
    printf("  GET RELAY\n");
    printf("  GET SHT31\n");
    printf("  GET THERMOSTAT\n");
    printf("  GET ALL\n");
    printf("  SET RELAY {\"gpio\":18,\"inverted\":false,\"min_delay_s\":60}\n");
    printf("  SET SHT31 {\"addr\":68,\"read_interval_ms\":5000,\"log_to_sd\":true}\n");
    printf("  SET THERMOSTAT {\"enabled\":true,\"consigne\":21,\"mode\":3,\"hysteresis\":0.3,\"calibration\":0,\"frost_mode\":false}\n");
}

static void do_get_all(void)
{
    char *relay_json = relay_get_json_status();
    char *sht31_json = sht31_get_json_status();
    char *thermostat_json = thermostat_get_json_status();

    cJSON *root = cJSON_CreateObject();

    if (relay_json)
    {
        cJSON *relay = cJSON_Parse(relay_json);
        if (relay)
            cJSON_AddItemToObject(root, "relay", relay);
    }

    if (thermostat_json)
    {
        cJSON *thermostat = cJSON_Parse(thermostat_json);
        if (thermostat)
            cJSON_AddItemToObject(root, "thermostat", thermostat);
    }

    if (sht31_json)
    {
        cJSON *sht31 = cJSON_Parse(sht31_json);
        if (sht31)
            cJSON_AddItemToObject(root, "sht31", sht31);
    }

    char *out = cJSON_PrintUnformatted(root);
    print_json_alloc(out);

    cJSON_Delete(root);
    free(relay_json);
    free(sht31_json);
    free(thermostat_json);
}

static esp_err_t apply_relay_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return ESP_ERR_INVALID_ARG;

    relay_config_t config;
    esp_err_t err = relay_get_config(&config);
    if (err != ESP_OK)
    {
        cJSON_Delete(root);
        return err;
    }

    cJSON *gpio = cJSON_GetObjectItem(root, "gpio");
    if (cJSON_IsNumber(gpio))
        config.gpio_pin = gpio->valueint;

    cJSON *inverted = cJSON_GetObjectItem(root, "inverted");
    if (cJSON_IsBool(inverted))
        config.inverted = cJSON_IsTrue(inverted);

    cJSON *min_delay = cJSON_GetObjectItem(root, "min_delay_s");
    if (!min_delay)
        min_delay = cJSON_GetObjectItem(root, "min_delay");
    if (cJSON_IsNumber(min_delay))
        config.min_delay_s = (uint32_t)min_delay->valuedouble;

    cJSON_Delete(root);
    return relay_set_config(&config);
}

/**
 * @brief Applique une configuration de thermostat depuis un JSON.
 * @param json Chaîne JSON contenant la configuration.
 * @return ESP_OK en cas de succès, une erreur sinon.
 */
esp_err_t apply_thermostat_json(const char *json) {
    if (json == NULL) {
        ESP_LOGE(TAG, "JSON NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Parse le JSON
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Échec du parsing JSON");
        return ESP_ERR_INVALID_ARG;
    }

    // Récupère la configuration actuelle
    thermostat_config_t config;
    esp_err_t err = thermostat_get_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Échec de la lecture de la config: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    // --- 1. Champ "enabled" ---
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(enabled)) {
        config.enabled = cJSON_IsTrue(enabled);
    }

    // --- 2. Champ "consigne" ou "target_temp" ---
    cJSON *consigne = cJSON_GetObjectItem(root, "consigne");
    if (!consigne) {
        consigne = cJSON_GetObjectItem(root, "target_temp");  // Alternative
    }
    if (cJSON_IsNumber(consigne)) {
        config.consigne = (float)consigne->valuedouble;
    }

    // --- 3. Champ "mode" ---
    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(mode)) {
        // Convertit la chaîne en enum (ex: "AUTO" → HEATING_MODE_AUTO)
        if (strcmp(mode->valuestring, "AUTO") == 0) {
            config.mode = HEATING_MODE_AUTO;
        } else if (strcmp(mode->valuestring, "MANU") == 0) {
            config.mode = HEATING_MODE_MANUAL;
        } else if (strcmp(mode->valuestring, "ABSENT") == 0) {
            config.mode = HEATING_MODE_ABSENT;
        } else if (strcmp(mode->valuestring, "H-GEL") == 0) {
            config.mode = HEATING_MODE_HORS_GEL;
        }
    } else if (cJSON_IsNumber(mode)) {
        // Si le mode est un nombre (ex: 0, 1, 2...)
        config.mode = (heating_mode_t)mode->valueint;
    }

    // --- 4. Champ "hysteresis" ---
    cJSON *hysteresis = cJSON_GetObjectItem(root, "hysteresis");
    if (cJSON_IsNumber(hysteresis)) {
        config.hysteresis = (float)hysteresis->valuedouble;
    }

    // --- 5. Champ "calibration" ---
    cJSON *calibration = cJSON_GetObjectItem(root, "calibration");
    if (cJSON_IsNumber(calibration)) {
        config.calibration = (float)calibration->valuedouble;
    }

    // --- 6. Champ "frost_mode" ---
    cJSON *frost_mode = cJSON_GetObjectItem(root, "frost_mode");
    if (cJSON_IsBool(frost_mode)) {
        config.frost_mode = cJSON_IsTrue(frost_mode);
    }

    // --- 7. Champ "temp_source" (optionnel) ---
    cJSON *temp_source = cJSON_GetObjectItem(root, "temp_source");
    if (cJSON_IsString(temp_source)) {
        // Si tu veux forcer une source de température (ex: "SHT31" ou "DHT")
        if (strcmp(temp_source->valuestring, "SHT31") == 0) {
            temperature_set_source(TEMP_SOURCE_SHT31);
        } else if (strcmp(temp_source->valuestring, "DHT") == 0) {
            temperature_set_source(TEMP_SOURCE_DHT);
        }
    }

    // --- 8. Applique la nouvelle configuration ---
    err = thermostat_set_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Échec de l'application de la config: %s", esp_err_to_name(err));
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t apply_sht31_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return ESP_ERR_INVALID_ARG;

    sht31_config_t config;
    esp_err_t err = sht31_get_config(&config);
    if (err != ESP_OK)
    {
        cJSON_Delete(root);
        return err;
    }

    cJSON *addr = cJSON_GetObjectItem(root, "addr");
    if (cJSON_IsNumber(addr))
        config.addr = (uint8_t)addr->valueint;

    cJSON *read_interval_ms = cJSON_GetObjectItem(root, "read_interval_ms");
    if (cJSON_IsNumber(read_interval_ms))
        config.read_interval_ms = (uint32_t)read_interval_ms->valuedouble;

    cJSON_Delete(root);
    return sht31_set_config(&config);
}

static void do_set(const char *arg)
{
    arg = skip_spaces(arg);

    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (starts_with(arg, "RELAY"))
    {
        err = apply_relay_json(skip_spaces(arg + strlen("RELAY")));
    }
    else if (starts_with(arg, "SHT31"))
    {
        err = apply_sht31_json(skip_spaces(arg + strlen("SHT31")));
    }
    else if (starts_with(arg, "THERMOSTAT"))
    {
        err = apply_thermostat_json(skip_spaces(arg + strlen("THERMOSTAT")));
    }
    else
    {
        print_error("unknown component");
        return;
    }

    if (err == ESP_OK)
        print_ok();
    else
        printf("{\"success\":false,\"error\":\"%s\"}\n", esp_err_to_name(err));
}

static void do_get(const char *arg)
{
    arg = skip_spaces(arg);

    if (strcmp(arg, "RELAY") == 0)
        print_json_alloc(relay_get_json_status());
    else if (strcmp(arg, "SHT31") == 0)
        print_json_alloc(sht31_get_json_status());
    else if (strcmp(arg, "THERMOSTAT") == 0)
        print_json_alloc(thermostat_get_json_status());
    else if (strcmp(arg, "ALL") == 0)
        do_get_all();
    else
        print_error("unknown component");
}

void serial_manager_handle_command(const char *cmd)
{
    cmd = skip_spaces(cmd);
    if (!cmd || *cmd == '\0')
        return;

    ESP_LOGD(TAG, "CMD: %s", cmd);

    if (strcmp(cmd, "HELP") == 0)
        do_help();
    else if (starts_with(cmd, "GET "))
        do_get(cmd + strlen("GET "));
    else if (starts_with(cmd, "SET "))
        do_set(cmd + strlen("SET "));
    else
        print_error("unknown command");

    printf("\n> ");
    fflush(stdout);
}

void handle_command(const char *cmd)
{
    serial_manager_handle_command(cmd);
}

void serial_task(void *arg)
{
    ESP_LOGI(TAG, "serial_task started");

    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(SERIAL_UART, &cfg));
    ESP_ERROR_CHECK(uart_driver_install(
        SERIAL_UART,
        SERIAL_BUF_SIZE * 2,
        0,
        0,
        NULL,
        0));
        
    uint8_t c;
    char line[SERIAL_BUF_SIZE];
    int pos = 0;

    EventGroupHandle_t ev_group = (EventGroupHandle_t)arg;

    printf("\nTerminal JSON pret\n> ");
    fflush(stdout);

    while (1)
    {
        if (ev_group)
        {
            xEventGroupWaitBits(ev_group,
                                SERIAL_ENABLE_BIT,
                                pdFALSE,
                                pdTRUE,
                                portMAX_DELAY);
        }

        int len = uart_read_bytes(SERIAL_UART, &c, 1, pdMS_TO_TICKS(20));

        if (len > 0)
        {
            if (c == '\n' || c == '\r')
            {
                line[pos] = '\0';
                serial_manager_handle_command(line);
                pos = 0;
            }
            else if (c == 0x08 || c == 0x7F)
            {
                if (pos > 0)
                    pos--;
                uart_write_bytes(SERIAL_UART, "\b \b", 3);
            }
            else if (pos < SERIAL_BUF_SIZE - 1 && c >= 32 && c <= 126)
            {
                line[pos++] = (char)c;
                uart_write_bytes(SERIAL_UART, (const char *)&c, 1);
            }
        }
    }
}

esp_err_t serial_manager_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(SERIAL_UART, &cfg);
    if (err != ESP_OK)
        return err;

    err = uart_driver_install(SERIAL_UART, SERIAL_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK)
        return err;

    ESP_LOGI(TAG, "Serial manager ready");
    return ESP_OK;
}
