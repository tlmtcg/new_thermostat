idf_component_register(SRCS "main.c" "blink_test.c"
                       INCLUDE_DIRS "." "include"
                       REQUIRES esp_driver_gpio esp_timer 
                       system_manager
                       event_bus
                       nvs_flash thermostat_task
                       )
