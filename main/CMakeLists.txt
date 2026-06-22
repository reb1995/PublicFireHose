idf_component_register(SRCS "PublicFireHose.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt esp_wifi nvs_flash esp_driver_gpio)
