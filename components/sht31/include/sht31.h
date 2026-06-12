/**
 * @file sht31.h
 * @brief Définitions pour le pilote SHT31.
 */

#ifndef SHT31_H
#define SHT31_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <time.h>

// =========================================================================
// STRUCTURES
// =========================================================================

/** @brief Configuration du capteur SHT31. */
typedef struct {
    uint8_t addr;                /**< Adresse I2C du capteur. */
    uint32_t read_interval_ms;   /**< Intervalle entre les lectures (en ms). */
} sht31_config_t;

/** @brief État d'exécution du capteur SHT31. */
typedef struct {
    float temperature;          /**< Dernière température lue (en °C). */
    float humidity;             /**< Dernière humidité lue (en %). */
    bool valid;                  /**< Indique si les données sont valides. */
    bool initialized;           /**< Indique si le capteur est initialisé. */
    bool running;                /**< Indique si le capteur est en cours d'exécution. */
    uint32_t read_count;         /**< Nombre de lectures réussies. */
    uint32_t error_count;        /**< Nombre total d'erreurs. */
    uint32_t consecutive_error_count; /**< Nombre d'erreurs consécutives. */
    esp_err_t last_error_code;   /**< Dernier code d'erreur. */
    time_t last_error_at;        /**< Timestamp de la dernière erreur. */
    time_t last_success_at;      /**< Timestamp de la dernière lecture réussie. */
    time_t last_update;          /**< Timestamp de la dernière mise à jour. */
    char last_error[64];         /**< Dernier message d'erreur. */
} sht31_runtime_t;

// =========================================================================
// FONCTIONS PUBLIQUES
// =========================================================================

/**
 * @brief Initialise le capteur SHT31.
 *
 * @param bus Pointeur vers le bus I2C.
 * @param addr Adresse I2C du capteur.
 * @return esp_err_t ESP_OK en cas de succès, une erreur sinon.
 */
esp_err_t sht31_init(i2c_master_bus_handle_t bus, uint8_t addr);

/**
 * @brief Désinitialise le capteur SHT31.
 */
void sht31_deinit(void);

/**
 * @brief Effectue un reset logiciel du capteur.
 *
 * @return esp_err_t ESP_OK en cas de succès, une erreur sinon.
 */
esp_err_t sht31_reset(void);

/**
 * @brief Tentative de récupération après une erreur.
 *
 * @return esp_err_t ESP_OK en cas de succès, une erreur sinon.
 */
esp_err_t sht31_recover(void);

/**
 * @brief Lit les données de température et d'humidité.
 *
 * @param temp Pointeur vers la variable de température (en °C).
 * @param hum Pointeur vers la variable d'humidité (en %).
 * @return esp_err_t ESP_OK en cas de succès, une erreur sinon.
 */
esp_err_t sht31_read(float *temp, float *hum);

/**
 * @brief Démarre le capteur SHT31.
 *
 * @param bus Pointeur vers le bus I2C.
 * @param addr Adresse I2C du capteur.
 * @return esp_err_t ESP_OK en cas de succès, une erreur sinon.
 */
esp_err_t sht31_start(i2c_master_bus_handle_t bus, uint8_t addr);

/**
 * @brief Arrête le capteur SHT31.
 */
void sht31_stop(void);

/**
 * @brief Récupère la configuration actuelle.
 *
 * @param out Pointeur vers la structure de configuration à remplir.
 * @return esp_err_t ESP_OK en cas de succès, une erreur sinon.
 */
esp_err_t sht31_get_config(sht31_config_t *out);

/**
 * @brief Définit la configuration du capteur.
 *
 * @param config Pointeur vers la nouvelle configuration.
 * @return esp_err_t ESP_OK en cas de succès, une erreur sinon.
 */
esp_err_t sht31_set_config(const sht31_config_t *config);

/**
 * @brief Récupère l'état d'exécution actuel.
 *
 * @return const sht31_runtime_t* Pointeur vers l'état d'exécution.
 */
const sht31_runtime_t *sht31_get_runtime(void);

/**
 * @brief Définit l'état "running" du capteur.
 *
 * @param running true pour activer, false pour désactiver.
 */
void sht31_set_running(bool running);

/**
 * @brief Récupère l'état complet du capteur au format JSON.
 *
 * @return char* Chaîne JSON représentant l'état du capteur.
 *         Doit être libérée avec free() après utilisation.
 */
char *sht31_get_json_status(void);

void sht31_reset_error_counter(void);

esp_err_t sht31_component_init(i2c_master_bus_handle_t bus, uint8_t addr);
esp_err_t sht31_component_read(float *temp, float *hum);


#endif  // SHT31_H