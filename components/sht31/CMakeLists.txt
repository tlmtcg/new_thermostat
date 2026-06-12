idf_component_register(
    SRCS
        "sht31.c"
        "sht31_task.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        esp_driver_i2c 
        freertos 
        event_bus
        utils
       i2c_shared
)
