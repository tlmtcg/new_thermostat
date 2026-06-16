#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "heating_program.h"
#include "esp_err.h"
#include "sensor_aggregator.h"

typedef struct {
    bool enabled;            // Thermostat activé/désactivé
    float consigne;          // Consigne de température (target_temp)
    heating_mode_t mode;  // Mode (AUTO, MANU, ABSENT, HORS_GEL)
    float hysteresis;        // Hystérésis
    float calibration;       // Calibration des capteurs
    bool frost_mode;         // Mode hors-gel activé
} thermostat_config_t;

typedef enum {
    TEMP_SOURCE_SHT31,  // Source principale (SHT31)
    TEMP_SOURCE_DHT,     // Source secondaire (DHT)
    TEMP_SOURCE_NONE     // Aucune source valide
} temperature_source_t;

typedef struct {
    // On regroupe les données capteurs ici
    sensor_data_t sensors; 
    temperature_source_t current_source;
    // Les autres données du thermostat
    bool relay_state;
    float last_known_exterior_temp;
    float current_calculated_target;
} thermostat_ctx_t;


void thermostat_task(void *pvParameters);
esp_err_t thermostat_init(void);
char* thermostat_get_json_status(thermostat_ctx_t *ctx);

/**
 * @brief Met à jour les données météo horaires dans le thermostat.
 * @param temperature Température en °C.
 * @param humidity Humidité en %.
 * @param weather_code Code météo (ex. 0 = ciel clair).
 */
void thermostat_update_hourly_weather(float temperature, float humidity, int weather_code);

esp_err_t thermostat_get_config(thermostat_config_t *config);

esp_err_t thermostat_set_config(const thermostat_config_t *config);

void thermostat_get_mode_status_str(char *dest, size_t max_len);

extern thermostat_ctx_t *g_thermostat_ctx;

// Dans thermostat_task.h
void temperature_set_source(thermostat_ctx_t *ctx, temperature_source_t source);