idf_component_register(SRCS "dht.c" "dht_task.c"
                      INCLUDE_DIRS "include"
                      REQUIRES 
                        esp_timer 
                        esp_driver_gpio
                        driver
                        event_bus
                        utils
                        )
