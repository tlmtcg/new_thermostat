idf_component_register(SRCS "heating_program.c" "time_utils.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES nvs_flash cjson time_manager
                    )
                    