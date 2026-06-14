idf_component_register(
    SRCS "time_manager_storage.c" "time_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_netif lwip nvs_flash utils event_bus
)