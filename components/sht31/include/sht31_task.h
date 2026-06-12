/**
 * @file sht31_task.h
 * @brief Définitions pour la tâche SHT31.
 */

#ifndef SHT31_TASK_H
#define SHT31_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/** @brief Structure de configuration pour la tâche SHT31. */
typedef struct {
    EventGroupHandle_t event_group;  /**< Groupe d'événements pour synchronisation. */
    EventBits_t event_bit;           /**< Bit à attendre pour démarrer la tâche. */
    uint32_t *delay_ms;              /**< Pointeur vers le délai entre les lectures (en ms). */
} sht31_task_config_t;

/** @brief Crée et démarre la tâche SHT31. */
void sht31_task(void *pvParameters);

#endif  // SHT31_TASK_H
