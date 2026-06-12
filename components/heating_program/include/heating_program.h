#ifndef HEATING_PROGRAM_H
#define HEATING_PROGRAM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define NB_JOURS  7
#define NB_PLAGES 4

typedef enum {
    JOUR_LUNDI = 0,
    JOUR_MARDI,
    JOUR_MERCREDI,
    JOUR_JEUDI,
    JOUR_VENDREDI,
    JOUR_SAMEDI,
    JOUR_DIMANCHE,
} jour_t;

typedef enum {
    HEATING_MODE_AUTO = 0,
    HEATING_MODE_MANUAL,
    HEATING_MODE_ABSENT,
    HEATING_MODE_HORS_GEL,
    HEATING_MODE_COUNT
} heating_mode_t;

typedef struct {
    uint32_t secondes_minuit;
    float temperature;
} plage_horaire_t;

typedef struct {
    plage_horaire_t planning[NB_JOURS][NB_PLAGES];
    heating_mode_t mode;
    float manual_target;
} chauffage_config_t;

// API Principale
esp_err_t heating_init(void);
esp_err_t heating_save(void);
void heating_reset_defaults(void);
float heating_get_temp(jour_t j, uint32_t now_sec);
float heating_get_temp_current(void);

// Nouveaux contrôles des modes
void heating_set_mode(heating_mode_t new_mode);
heating_mode_t heating_get_mode(void);
void heating_set_manual_target(float temp);
float heating_get_manual_target(void);
float heating_calculate_target_temperature(float ext_temp);

// JSON & Utilitaires
char *heating_get_json(void);
const chauffage_config_t *heating_get_config(void);
chauffage_config_t *heating_get_config_rw(void);
esp_err_t heating_get_program_json(char **out_json);
esp_err_t heating_reset_program(void);
int64_t heating_program_get_next_target_timestamp(void);

#endif