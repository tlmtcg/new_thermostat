#include "sensor_aggregator.h"
#include <math.h>
#include "thermostat_task.h"

/* =========================================================================
 * 1. SOUS-COMPOSANT : ARBITRAGE DES CAPTEURS
 * ========================================================================= */

// Définit la source de température dans le contexte
void temperature_set_source(thermostat_ctx_t *ctx, temperature_source_t source)
{
    if (ctx != NULL) {
        ctx->current_source = source;
    }
}

// Récupère la source de température actuelle depuis le contexte
temperature_source_t temperature_get_source(const thermostat_ctx_t *ctx)
{
    return (ctx != NULL) ? ctx->current_source : TEMP_SOURCE_NONE;
}

float sensor_arbitrate(const sensor_data_t *data, const char **out_source)
{
    // Priorité au SHT31 (Maître)
    if (data->sht31_valid && !isnan(data->sht31_temp))
    {
        *out_source = "SHT31 (Maître)";
        return data->sht31_temp;
    }
    
    // Sauvegarde par le DHT
    if (data->dht_valid && !isnan(data->dht_temp))
    {
        *out_source = "DHT (Sauvegarde)";
        return data->dht_temp;
    }

    // Aucun capteur valide
    *out_source = "AUCUN";
    return NAN;
}