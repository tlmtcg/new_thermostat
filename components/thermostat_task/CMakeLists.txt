idf_component_register(
    SRCS 
        "thermostat_task.c" "sensor_aggregator.c"
    INCLUDE_DIRS 
        "include"
    REQUIRES
        dht
        event_bus
        heating_program
        nvs_flash
        relay
        sht31
        utils
        weather
        thermostat_task
)              