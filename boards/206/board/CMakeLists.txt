idf_component_register(SRCS "board.c" "bsp_206.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd)
