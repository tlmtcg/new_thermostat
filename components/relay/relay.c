#include "relay.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "sdkconfig.h"
#include "utils.h"
#include "esp_task_wdt.h"

static const char *TAG = "RELAY";

#ifdef CONFIG_RELAY_INVERTED
#define RELAY_DEFAULT_INVERTED true
#else
#define RELAY_DEFAULT_INVERTED false
#endif

static relay_config_t g_relay_config = {
    .gpio_pin = CONFIG_RELAY_GPIO_PIN,
    .inverted = RELAY_DEFAULT_INVERTED,
    .min_delay_s = CONFIG_RELAY_MIN_DELAY_SEC,
};

static relay_runtime_t g_relay_runtime = {
    .last_error = "",
    .remaining_s = 0,
    .state = false,
};

static void relay_update_remaining_s(void)
{
    uint32_t now = get_ms();
    uint32_t elapsed_s = (now - g_relay_runtime.last_change_ms) / 1000;

    if (elapsed_s < g_relay_config.min_delay_s)
        g_relay_runtime.remaining_s = g_relay_config.min_delay_s - elapsed_s;
    else
        g_relay_runtime.remaining_s = 0;
}

static void relay_configure_gpio(int gpio)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << gpio),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };

    gpio_config(&io_conf);
}

static int relay_level_for_state(bool state)
{
    if (state)
        return g_relay_config.inverted ? 0 : 1;

    return g_relay_config.inverted ? 1 : 0;
}

static void relay_update_runtime_levels(void)
{
    g_relay_runtime.heating = g_relay_runtime.state;
    g_relay_runtime.gpio_level = relay_level_for_state(g_relay_runtime.state);
}

static uint32_t relay_current_heating_s(uint32_t now)
{
    if (!g_relay_runtime.state)
        return 0;

    return (now - g_relay_runtime.last_change_ms) / 1000;
}

static void relay_update_heating_s(void)
{
    uint32_t now = get_ms();

    g_relay_runtime.current_heating_s = relay_current_heating_s(now);
    g_relay_runtime.total_heating_live_s =
        g_relay_runtime.total_heating_s + g_relay_runtime.current_heating_s;

    snprintf(g_relay_runtime.duration_str,
             sizeof(g_relay_runtime.duration_str),
             "%lu s",
             (unsigned long)g_relay_runtime.total_heating_live_s);
}

static bool is_change_allowed(void)
{
    uint32_t now = get_ms();
    uint32_t elapsed_s = (now - g_relay_runtime.last_change_ms) / 1000;

    if (elapsed_s < g_relay_config.min_delay_s)
    {
        g_relay_runtime.remaining_s = g_relay_config.min_delay_s - elapsed_s;
    

        snprintf(g_relay_runtime.last_error,
                 sizeof(g_relay_runtime.last_error),
                 "Delai trop court : %us restantes",
                 (unsigned)g_relay_runtime.remaining_s);

        ESP_LOGW(TAG, "%s", g_relay_runtime.last_error);
        return false;
    }

    g_relay_runtime.last_error[0] = '\0';
    g_relay_runtime.remaining_s = 0;
    return true;
}

void relay_init(void)
{

    relay_configure_gpio(g_relay_config.gpio_pin);
    gpio_set_level(g_relay_config.gpio_pin, relay_level_for_state(false));

    g_relay_runtime.state = false;
    g_relay_runtime.last_change_ms = get_ms();
    relay_update_runtime_levels();
    relay_update_heating_s();

    ESP_LOGI(TAG, "Relais init (GPIO %d, min_delay=%us, inverted=%d, heating_total=%lu s)",
             g_relay_config.gpio_pin,
             (unsigned)g_relay_config.min_delay_s,
             g_relay_config.inverted,
             (unsigned long)g_relay_runtime.total_heating_s);
}

static void relay_on(void)
{
    if (!g_relay_runtime.state)
    {
        if (!is_change_allowed())
            return;

        gpio_set_level(g_relay_config.gpio_pin, relay_level_for_state(true));

        g_relay_runtime.state = true;
        g_relay_runtime.cycle_count++;
        g_relay_runtime.last_change_ms = get_ms();
        relay_update_runtime_levels();
        relay_update_heating_s();

        ESP_LOGI(TAG,
                 "Relais allume (gpio_level=%d, inverted=%d)",
                 g_relay_runtime.gpio_level,
                 g_relay_config.inverted);
    }
}

static void relay_off(void)
{
    if (g_relay_runtime.state)
    {
        if (!is_change_allowed())
            return;

        uint32_t now = get_ms();
        g_relay_runtime.total_heating_s +=
            (now - g_relay_runtime.last_change_ms) / 1000;

        gpio_set_level(g_relay_config.gpio_pin, relay_level_for_state(false));

        g_relay_runtime.state = false;
        g_relay_runtime.last_change_ms = now;
        relay_update_runtime_levels();
        relay_update_heating_s();

        ESP_LOGI(TAG,
                 "Relais eteint (chauffe_totale=%lu s, gpio_level=%d, inverted=%d)",
                 (unsigned long)g_relay_runtime.total_heating_s,
                 g_relay_runtime.gpio_level,
                 g_relay_config.inverted);
    }
}

static void relay_set(bool target_state)
{
    if (target_state)
        relay_on();
    else
        relay_off();
}

esp_err_t relay_get_config(relay_config_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;

    *out = g_relay_config;
    return ESP_OK;
}

esp_err_t relay_set_config(const relay_config_t *config)
{
    if (!config)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = relay_set_gpio(config->gpio_pin);
    if (err != ESP_OK)
        return err;

    err = relay_set_inverted(config->inverted);
    if (err != ESP_OK)
        return err;

    g_relay_config.min_delay_s = config->min_delay_s;

    ESP_LOGI(TAG, "Config relais appliquee (GPIO %d, min_delay=%us, inverted=%d)",
             g_relay_config.gpio_pin,
             (unsigned)g_relay_config.min_delay_s,
             g_relay_config.inverted);

    return ESP_OK;
}

const relay_runtime_t *relay_get_runtime(void)
{
    relay_update_remaining_s();
    relay_update_runtime_levels();
    relay_update_heating_s();
    return &g_relay_runtime;
}

esp_err_t relay_set_gpio(int new_gpio)
{
    if (new_gpio < 0 || new_gpio > 48)
        return ESP_ERR_INVALID_ARG;

    g_relay_config.gpio_pin = new_gpio;

    relay_configure_gpio(new_gpio);
    gpio_set_level(new_gpio, relay_level_for_state(g_relay_runtime.state));
    relay_update_runtime_levels();

    ESP_LOGI(TAG, "GPIO du relais change en %d", new_gpio);
    return ESP_OK;
}

esp_err_t relay_set_inverted(bool inverted)
{
    g_relay_config.inverted = inverted;

    gpio_set_level(g_relay_config.gpio_pin,
                   relay_level_for_state(g_relay_runtime.state));
    relay_update_runtime_levels();

    ESP_LOGI(TAG, "Inversion modifiee : %s", inverted ? "true" : "false");
    return ESP_OK;
}

// --- Fonction principale convertie ---
char *relay_get_json_status(void)
{
    // Mises à jour des états
    relay_update_remaining_s();
    relay_update_runtime_levels();
    relay_update_heating_s();

    // Définition du gabarit JSON (remplace la structure cJSON)
    const char *json_format = 
        "{\"runtime\":{"
            "\"state\":%s,"
            "\"heating\":%s,"
            "\"gpio_level\":%d,"
            "\"cycles\":%u,"
            "\"total_s\":%lu,"
            "\"total_live_s\":%lu,"
            "\"current_s\":%lu,"
            "\"duration\":\"%s\","
            "\"last_error\":\"%s\","
            "\"remaining_s\":%ld"
        "},"
        "\"config\":{"
            "\"gpio\":%d,"
            "\"inverted\":%s,"
            "\"min_delay_s\":%u"
        "}}";

    // Envoi des arguments à la fonction de formatage et d'allocation dynamique
    return format_json_alloc(json_format,
        g_relay_runtime.state ? "true" : "false",
        g_relay_runtime.heating ? "true" : "false",
        g_relay_runtime.gpio_level,
        g_relay_runtime.cycle_count,
        (unsigned long)g_relay_runtime.total_heating_s,
        (unsigned long)g_relay_runtime.total_heating_live_s,
        (unsigned long)g_relay_runtime.current_heating_s,
        g_relay_runtime.duration_str,
        g_relay_runtime.last_error,
        (long)g_relay_runtime.remaining_s,
        g_relay_config.gpio_pin,
        g_relay_config.inverted ? "true" : "false",
        g_relay_config.min_delay_s
    );
}

esp_err_t relay_apply_config(bool force_state, uint32_t new_min_delay)
{
    if (new_min_delay > 0)
        g_relay_config.min_delay_s = new_min_delay;

    relay_set(force_state);

    return ESP_OK;
}

bool get_relay_state(void)
{
    return g_relay_runtime.state;
}

void relay_task(void *arg)
{
    esp_task_wdt_add(NULL);

    // Le filtre écoute désormais uniquement EVENT_RELAY_SET
    static const event_type_t filter[] = {EVENT_RELAY_SET};

    // Inscription au bus d'événements
    QueueHandle_t q = event_bus_subscribe("relay", filter, 1);

    if (!q)
    {
        ESP_LOGE(TAG, "Relay Subscription failed");
        vTaskDelete(NULL);
        return;
    }

    event_t evt;

    while (1)
    {
        esp_task_wdt_reset();  // ✅ Réinitialise le WDT régulièrement
        // Réception bloquante rythmée par la file
        if (event_bus_receive(q, &evt, pdMS_TO_TICKS(200)))
        {
            if (evt.type == EVENT_RELAY_SET)
            {
                esp_task_wdt_reset();
                ESP_LOGI(TAG, "RELAY %s", evt.net.bool_value ? "ON" : "OFF");
                relay_set(evt.net.bool_value);
            }   
        }
    }
}
