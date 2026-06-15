idf_component_register(
    SRCS
        "serial_manager.c"

    INCLUDE_DIRS
        "include"

    PRIV_REQUIRES
        cjson
        esp_driver_uart
        freertos
        relay
        sht31
        thermostat
)
