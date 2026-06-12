# Enregistrement du composant Relay
idf_component_register(
    SRCS 
        "relay.c"
    INCLUDE_DIRS 
        "include"
    REQUIRES 

        esp_driver_gpio     # Nécessaire pour gpio.h
        esp_timer   # Nécessaire pour esp_timer_get_time()
        log         # Nécessaire pour ESP_LOGI
        freertos
        event_bus
        utils
)
