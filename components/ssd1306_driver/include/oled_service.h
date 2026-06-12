#ifndef OLED_SERVICE_H
#define OLED_SERVICE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Définition du nombre total de pages de l'interface
#define OLED_PAGE_COUNT 6

// Taille de l'historique des températures
#define TEMP_HISTORY_SIZE 20
#define TEMP_MIN -10.0f

// Taille nécessaire pour stocker l'ip "XXX.XXX.XXX.XXX\0"
#define OLED_IP_STR_SIZE 16

/**
 * @brief Énumération des différentes pages de l'écran OLED
 */
typedef enum {
    OLED_PAGE_MAIN = 0,
    OLED_PAGE_HISTORY,
    OLED_PAGE_TIME,
    OLED_PAGE_WEATHER,
    OLED_PAGE_ALERTS,
    OLED_PAGE_WIFI,      // Ajout de la page WiFi

} oled_page_t;

/**
 * @brief Structure de configuration pour l'initialisation de la tâche OLED
 */
typedef struct {
    EventGroupHandle_t event_group; /*!< Handle du groupe d'événements de l'application */
    EventBits_t event_bit;          /*!< Bit d'événement requis pour exécuter la tâche */
    uint32_t refresh_interval_ms;   /*!< Intervalle de rafraîchissement de l'écran en ms */
} oled_task_config_t;

/**
 * @brief Structure de stockage des données graphiques de l'historique
 */
typedef struct {
    float temp_history[TEMP_HISTORY_SIZE];
    uint8_t history_index;
    bool history_full;
} oled_graph_data_t;

/**
 * @brief Initialise le périphérique matériel OLED SSD1306 sur le bus I2C fourni
 * * @param bus Handle du bus maître I2C existant
 * @return esp_err_t ESP_OK en cas de succès, ou code d'erreur ESP-IDF
 */
esp_err_t oled_service_init(i2c_master_bus_handle_t bus);

/**
 * @brief Démarre la tâche FreeRTOS de gestion et d'affichage de l'OLED
 * * @param config Pointeur vers la structure de configuration de la tâche
 * @return esp_err_t ESP_OK en cas de succès
 */
esp_err_t oled_service_start(const oled_task_config_t *config);

/**
 * @brief Ajoute manuellement une valeur de température à l'historique du graphique
 * * @param temp Valeur de la température en Celsius
 */
void oled_service_add_temp_to_history(float temp);

/**
 * @brief Affiche l'écran de démarrage par défaut (Boot)
 */
void oled_service_show_boot(void);

/**
 * @brief Force l'affichage d'un message d'erreur critique
 * * @param msg Message d'erreur textuel
 */
void oled_service_show_error(const char *msg);

/**
 * @brief Affiche un écran générique contenant jusqu'à 3 lignes de texte
 */
void oled_service_show_text(const char *line1, const char *line2, const char *line3);

/**
 * @brief Affiche rapidement la température et l'humidité actuelles
 */
void oled_service_show_temp_hum(float temp, float hum);

esp_err_t oled_component_init(i2c_master_bus_handle_t bus, uint8_t addr);

#ifdef __cplusplus
}
#endif

#endif // OLED_SERVICE_H
