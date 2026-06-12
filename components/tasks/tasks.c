#include "tasks.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dht_task.h"
#include "thermostat_task.h"
#include "relay.h"
#include "sht31_task.h"

// Variables de configuration (à lier ou déclarer selon votre architecture globale)
extern EventGroupHandle_t g_app_event_group; // Exemple : votre EventGroup global
static uint32_t sht31_default_delay = 5000;

static sht31_task_config_t sht31_config;

static const task_definition_t task_list[] =
{
    {dht_task,          "dht_task",            CONFIG_DHT_STACK_SIZE, NULL, CONFIG_DHT_TASK_PRIORITY},
    // {thermostat_task,   "thermostat_task",     CONFIG_THERMOSTAT_STACK_SIZE, NULL, CONFIG_THERMOSTAT_TASK_PRIORITY},
    {relay_task,        "relay_task",          CONFIG_RELAY_STACK_SIZE, NULL, CONFIG_RELAY_TASK_PRIORITY},
    {sht31_task,        "sht31_task",          CONFIG_SHT31_STACK_SIZE, &sht31_config, CONFIG_SHT31_TASK_PRIORITY}
};

void tasks_start(void)
{
    // CORRECTION CRITIQUE : Initialiser les champs de la structure avant de créer la tâche
    sht31_config.event_group = g_app_event_group; // Pointeur vers votre EventGroup réel
    sht31_config.event_bit   = BIT(0);            // Le bit d'activation (ex: BIT(0))
    sht31_config.delay_ms    = &sht31_default_delay;

    // Lancement de toutes les tâches de la liste
    for (size_t i = 0; i < sizeof(task_list)/sizeof(task_list[0]); i++)
    {
        xTaskCreate(
            task_list[i].function,
            task_list[i].name,
            task_list[i].stack_size,
            task_list[i].config,
            task_list[i].priority,
            NULL);
    }
}
