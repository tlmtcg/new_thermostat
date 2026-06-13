idf_component_register(
    SRCS 
        "system_manager.c" "sensor_service.c"
    INCLUDE_DIRS 
        "include"
    PRIV_REQUIRES
        tasks
        relay
        driver
        ssd1306_driver
        i2c_shared
        dht
        thermostat_task
        wifi_app
        time_manager
)              