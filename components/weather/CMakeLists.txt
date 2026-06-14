idf_component_register(
    SRCS
        "weather.c"
        "weather_task.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_netif cjson esp-tls alert_manager freertos thermostat config_runtime time_utils
    # Inclure le certificat dans le binaire
    EMBED_FILES "certs/isrgrootx1.pem"
)
