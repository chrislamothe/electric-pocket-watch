idf_component_register(SRCS "main.c" "i2c_dev.c" "axp.c" "imu.c" "buttons.c" "watch_time.c"
                            "faces.c" "face_analog.c" "face_digital.c" "controller.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer esp_hw_support esp_system)
