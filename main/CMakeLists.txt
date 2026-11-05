idf_component_register(SRCS "main.c" "i2c_dev.c" "axp.c" "imu.c" "buttons.c" "watch_time.c"
                            "faces.c" "face_analog.c" "face_calendar.c" "face_railroad.c"
                            "fonts/font_railroad_64.c" "fonts/font_railroad_20.c" "controller.c" "settings.c" "net.c" "improv.c" "settings_ui.c" "splash.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer esp_hw_support esp_system heap nvs_flash esp_wifi
                                esp_netif esp_event esp_driver_usb_serial_jtag)
