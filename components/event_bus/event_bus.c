#include "event_bus.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "utils.h"

static const char *TAG = "EVENT_BUS";

// ===================== STATE =====================

static event_subscriber_t subscribers[EVENT_BUS_MAX_SUBSCRIBERS];
static uint8_t subscriber_count = 0;

static event_bus_stats_t stats = {0};

// CORRECTION : Utilisation d'un Mutex Récursif pour éviter l'interblocage 
// si une tâche publie un événement au sein de son propre traitement d'événement.
static SemaphoreHandle_t bus_mutex = NULL;

// ===================== MATCH FILTER =====================

static bool match_filter(event_subscriber_t *sub, event_t *evt)
{
    if (sub->filter_count == 0)
        return true;

    for (int i = 0; i < sub->filter_count; i++)
    {
        if (sub->filter[i] == evt->type)
            return true;
    }

    return false;
}

// ===================== INIT =====================

void event_bus_init(void)
{
    // CORRECTION : Initialisation en mode récursif
    bus_mutex = xSemaphoreCreateRecursiveMutex();

    subscriber_count = 0;
    memset(&stats, 0, sizeof(stats));

    ESP_LOGI(TAG, "Event bus initialized");
}

// ===================== SUBSCRIBE =====================

QueueHandle_t event_bus_subscribe(
    const char *name,
    const event_type_t *filter,
    uint8_t filter_count)
{
    if (!bus_mutex)
        return NULL;

    if (xSemaphoreTakeRecursive(bus_mutex, portMAX_DELAY) != pdTRUE)
        return NULL;

    if (subscriber_count >= EVENT_BUS_MAX_SUBSCRIBERS)
    {
        xSemaphoreGiveRecursive(bus_mutex);
        return NULL;
    }

    event_subscriber_t *sub = &subscribers[subscriber_count++];

    memset(sub, 0, sizeof(*sub));

    sub->queue = xQueueCreate(CONFIG_SUBSCRIBER_QUEUE_SIZE, sizeof(event_t));

    if (!sub->queue)
    {
        subscriber_count--;
        xSemaphoreGiveRecursive(bus_mutex);
        return NULL;
    }

    if (name)
    {
        strncpy(sub->name, name, sizeof(sub->name) - 1);
        sub->name[sizeof(sub->name) - 1] = '\0';
    }

    if (filter && filter_count > 0)
    {
        if (filter_count > EVENT_BUS_MAX_FILTER)
            filter_count = EVENT_BUS_MAX_FILTER;

        memcpy(sub->filter, filter, filter_count * sizeof(event_type_t));
        sub->filter_count = filter_count;
    }
    else
    {
        sub->filter_count = 0;
    }

    xSemaphoreGiveRecursive(bus_mutex);

    ESP_LOGI(TAG,
             "Subscriber '%s' registered (%u filters)",
             sub->name,
             sub->filter_count);

    return sub->queue;
}

// ===================== PUBLISH =====================

bool event_bus_publish(const event_t *event)
{
    if (!event || !bus_mutex)
        return false;

    event_t evt = *event;
    evt.timestamp_ms = get_ms();

    if (xSemaphoreTakeRecursive(bus_mutex, portMAX_DELAY) != pdTRUE)
        return false;

    // ===================== DROP LOGIC =====================
    if (evt.priority == EVENT_PRIO_LOW && stats.dropped > CONFIG_EVENT_LOW_DROP_THRESHOLD)
    {
        stats.dropped++;
        xSemaphoreGiveRecursive(bus_mutex);
        return true; 
    }

    stats.sent++;

    for (int i = 0; i < subscriber_count; i++)
    {
        event_subscriber_t *sub = &subscribers[i];

        if (!match_filter(sub, &evt))
        {
            stats.filtered++;
            continue;
        }

        if (xQueueSend(sub->queue, &evt, 0) == pdTRUE)
        {
            stats.delivered++;
            sub->received++;
        }
        else
        {
            sub->dropped++;
            stats.dropped++;
        }
    }

    xSemaphoreGiveRecursive(bus_mutex);
    return true;
}

// ===================== RECEIVE =====================

bool event_bus_receive(QueueHandle_t q, event_t *evt, TickType_t timeout)
{
    if (!q || !evt)
        return false;

    return xQueueReceive(q, evt, timeout) == pdTRUE;
}

// ===================== STATS =====================

void event_bus_get_stats(event_bus_stats_t *out)
{
    if (!out || !bus_mutex)
        return;

    if (xSemaphoreTakeRecursive(bus_mutex, portMAX_DELAY) == pdTRUE)
    {
        *out = stats;
        xSemaphoreGiveRecursive(bus_mutex);
    }
}

// CORRECTION : Remplacement direct des types internes
static void get_casted_stats(event_bus_stats_t *dst)
{
    event_bus_get_stats(dst);
}

void event_bus_print_stats(void)
{
    event_bus_stats_t s;
    event_bus_get_stats(&s);

    ESP_LOGI(TAG,
             "sent=%lu delivered=%lu dropped=%lu filtered=%lu",
             (unsigned long)s.sent, 
             (unsigned long)s.delivered, 
             (unsigned long)s.dropped, 
             (unsigned long)s.filtered);
}

// Génération dynamique du JSON
char *event_bus_get_json_status(void)
{
    event_bus_stats_t s;
    get_casted_stats(&s);

    const char *json_format = "{\"stats\":{\"sent\":%lu,\"delivered\":%lu,\"dropped\":%lu,\"filtered\":%lu}}";

    int len = snprintf(NULL, 0, json_format, 
                       (unsigned long)s.sent, 
                       (unsigned long)s.delivered, 
                       (unsigned long)s.dropped, 
                       (unsigned long)s.filtered);

    if (len < 0)
        return NULL;

    char *buf = malloc(len + 1);
    if (!buf)
        return NULL;

    snprintf(buf, len + 1, json_format, 
             (unsigned long)s.sent, 
             (unsigned long)s.delivered, 
             (unsigned long)s.dropped, 
             (unsigned long)s.filtered);

    return buf;
}

void event_bus_print_subscribers(void)
{
    if (!bus_mutex || xSemaphoreTakeRecursive(bus_mutex, portMAX_DELAY) != pdTRUE)
        return;

    ESP_LOGI(TAG, "EVENT BUS");
    ESP_LOGI(TAG, "---------");

    for (int i = 0; i < subscriber_count; i++)
    {
        event_subscriber_t *sub = &subscribers[i];

        ESP_LOGI(TAG,
                 "%s rx=%lu drop=%lu",
                 sub->name,
                 (unsigned long)sub->received,
                 (unsigned long)sub->dropped);
    }
    ESP_LOGI(TAG, "---------");
    
    xSemaphoreGiveRecursive(bus_mutex);
}
