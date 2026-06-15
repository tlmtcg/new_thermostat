#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // ===================== TYPES =====================

    typedef enum
    {
        EVENT_NONE = 0,
        // Capteurs
        EVENT_SENSOR_DHT,
        EVENT_SENSOR_ERROR_DHT,
        EVENT_SENSOR_SHT31,
        EVENT_SENSOR_ERROR_SHT31,

        // Thermostat et relais
        EVENT_THERMOSTAT_SET,
        EVENT_THERMOSTAT_MODE,
        EVENT_RELAY_SET,

        // Météo
        EVENT_WEATHER_UPDATE,
        EVENT_WEATHER_HOURLY,

        // WEB SERVER & SERIAL
        EVENT_MODE_CHANGE_REQUEST,
        EVENT_MANUAL_SETPOINT_REQUEST,

        // NETWORK & TIME
        EVENT_WIFI_STATUS,
        EVENT_NET_TIME_SYNCED,
        EVENT_NET_TIME_ERROR,

        // STORAGE & FTP
        EVENT_SD_CRITICAL_ERROR,
        EVENT_LOG_ROTATE_TRIGGER,

        // VISUAL & LEDSTRIP
        EVENT_LED_COMMAND,

        // SYSTEM
        EVENT_SYSTEM_REBOOT
    } event_type_t;

    typedef enum
    {
        EVENT_PRIO_LOW = 0,
        EVENT_PRIO_NORMAL,
        EVENT_PRIO_HIGH
    } event_priority_t;

    // ===================== EVENT =====================

    typedef struct
    {
        event_type_t type;
        event_priority_t priority;
        uint32_t timestamp_ms;

        union
        {
            // Bloc 1 : Capteurs Environnementaux
            struct
            {
                float temperature;
                float humidity;
            } sensor;

            // Bloc 2 : Réseau & Commandes (Partage la mémoire avec le Bloc 1)
            struct
            {
                int error_code;
                bool bool_value;
                uint8_t retry_count;
            } net;

            // Bloc 3 : Payload pour pointeurs (Web, SD, FTP)
            struct
            {
                void *payload_ptr;
                size_t payload_len;
            } payload;

            // Bloc pour les données météo horaires
            struct
            {
                float temperature; // Température en °C
                float humidity;    // Humidité en %
                int weather_code;  // Code météo (ex. 0 = ciel clair)
            } weather_hourly;
        };
    } event_t;

    // ===================== SUBSCRIBER =====================

#define EVENT_BUS_MAX_FILTER 24
#define EVENT_BUS_MAX_SUBSCRIBERS 16

    typedef struct
    {
        QueueHandle_t queue;

        event_type_t filter[EVENT_BUS_MAX_FILTER];
        uint8_t filter_count;
        char name[32];
        uint32_t received;
        uint32_t dropped;
    } event_subscriber_t;

    // ===================== STATS =====================

    typedef struct
    {
        uint32_t sent;
        uint32_t delivered;
        uint32_t dropped;
        uint32_t filtered;
    } event_bus_stats_t;

    // ===================== API =====================

    void event_bus_init(void);

    QueueHandle_t event_bus_subscribe(const char *name, const event_type_t *filter, uint8_t filter_count);

    bool event_bus_publish(const event_t *event);

    bool event_bus_receive(QueueHandle_t q, event_t *evt, TickType_t timeout);

    void event_bus_get_stats(event_bus_stats_t *out);

    void event_bus_print_stats(void);

    void event_bus_print_subscribers(void);

    char *event_bus_get_json_status(void);

#ifdef __cplusplus
}
#endif
