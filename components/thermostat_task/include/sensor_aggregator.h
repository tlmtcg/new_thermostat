#ifndef SENSOR_AGGREGATOR_H
#define SENSOR_AGGREGATOR_H

#include <stdbool.h>

typedef struct {
    float dht_temp;
    float sht31_temp;
    bool dht_valid;
    bool sht31_valid;
} sensor_data_t;

// Retourne la température arbitrée et met à jour le nom de la source
float sensor_arbitrate(const sensor_data_t *data, const char **out_source);

#endif
