idf_component_register(
    SRCS
    
        "ssd1306_driver.c" "oled_service.c" "font5x7.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        ssd1306_driver
        esp_driver_i2c 
        esp_common
        log
        i2c_shared
        event_bus
        relay
        weather
)
