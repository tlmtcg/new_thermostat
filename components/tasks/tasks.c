#include "tasks.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dht_task.h"
// #include "thermostat_task.h"
#include "relay.h"
#include "sht31_task.h"
#include "weather_task.h"
#include <math.h>
#include "weather_store.h" 

// Variables de configuration (à lier ou déclarer selon votre architecture globale)
extern EventGroupHandle_t g_app_event_group; // Exemple : votre EventGroup global
static uint32_t sht31_default_delay = 5000;
static uint32_t weather_default_delay = 60 * 60 * 1000;

static sht31_task_config_t sht31_config;
static weather_task_config_t weather_config;

void tasks_start(void)
{
    // Configuration pour SHT31
    sht31_config.event_group = g_app_event_group; // Pointeur vers votre EventGroup réel
    sht31_config.event_bit = BIT(0);              // Le bit d'activation (ex: BIT(0))
    sht31_config.delay_ms = &sht31_default_delay;

// ✅ Configuration pour weather_update_task (sans is_wifi_connected)
    weather_config.event_group = g_app_event_group;
    weather_config.event_bit = BIT(1);  // Bit différent
    weather_config.store_set_all = weather_store_set_all;
    weather_config.delay_ms = &weather_default_delay;

    static const task_definition_t task_list[] =
    {
        {dht_task, "dht_task", CONFIG_DHT_STACK_SIZE, NULL, CONFIG_DHT_TASK_PRIORITY},
        {relay_task, "relay_task", CONFIG_RELAY_STACK_SIZE, NULL, CONFIG_RELAY_TASK_PRIORITY},
        {sht31_task, "sht31_task", CONFIG_SHT31_STACK_SIZE, &sht31_config, CONFIG_SHT31_TASK_PRIORITY},
        {weather_update_task, "weather_task", 10240, &weather_config, 4}};

    // Lancement de toutes les tâches de la liste
    for (size_t i = 0; i < sizeof(task_list) / sizeof(task_list[0]); i++)
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
