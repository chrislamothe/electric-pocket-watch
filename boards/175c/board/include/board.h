#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C (round 466x466 CO5300, CST9217 touch, AXP2101,
 * QMI8658). Verified on hardware since v0.0.8.
 */
#define BOARD_ID              "175c"
#define BOARD_NAME            "ESP32-S3-Touch-AMOLED-1.75C"
#define BOARD_SHORT           "1.75C"

#define BOARD_PIN_BOOT        0     /* active low, 10K pull-up */
#define BOARD_PIN_PWR         3     /* SYS_OUT: high while PWR is held; -1 = not on a GPIO */
#define BOARD_PWR_POLL_MS     0     /* PWR wakes light sleep through its GPIO */
#define BOARD_PIN_IMU_INT     21    /* QMI8658 INT1; unused, pulled down */
#define BOARD_PIN_TOUCH_RST   BSP_LCD_TOUCH_RST     /* GPIO2 */
#define BOARD_TOUCH_CHIP      9217  /* CST9217: asleep except in Settings (main/touch.c) */
#define BOARD_TOUCH_ADDR      0x5A
#define BOARD_TOUCH_MIRROR    1     /* 1 = x and y both reversed relative to the panel */
#define BOARD_TOUCH_BOOT_MS   60    /* after the reset pulse, before it takes commands */
#define BOARD_PIN_SPEAKER_PA  BSP_POWER_AMP_IO      /* GPIO46 */
#define BOARD_PIN_MOTOR       -1

/* After bsp_i2c_init(): board-specific setup (pins the board package does not configure). */
void board_init(i2c_master_bus_handle_t bus);

/* PWR button level now (true = held). */
bool board_pwr_down(void);
