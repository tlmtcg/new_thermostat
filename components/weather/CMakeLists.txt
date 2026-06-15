idf_component_register(
    SRCS
        "weather.c"
        "weather_task.c"
        "weather_store.c"
        "weather_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        esp_http_client 
        esp_netif 
        cjson 
        esp-tls      
        freertos 
        event_bus
        
    # Inclure le certificat dans le binaire
    EMBED_FILES "certs/isrgrootx1.pem"
)
