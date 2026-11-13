idf_component_register(SRCS "camera_node.c"
                            "camera_service.c"
                            "ble_service.c"
                            "board_io.c"
                            "daynight.c"
                            "als.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_timer esp_rom esp_driver_gpio esp_driver_i2c)
