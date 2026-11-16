#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

/*
 * Waveshare ESP32-S3-Touch-AMOLED-2.06 (SKU 31957): rectangular 410x502 AMOLED, FT3168 touch,
 * AXP2101, QMI8658, PCF85063 RTC, vibration-motor driver. Pins from the V1.0 schematic.
 */
#define BOARD_ID              "206"
#define BOARD_NAME            "ESP32-S3-Touch-AMOLED-2.06"
#define BOARD_SHORT           "2.06"

#define BOARD_PIN_BOOT        0     /* active low, 10K pull-up */
#define BOARD_PIN_PWR         10    /* SYS_OUT: high while PWR is held */
#define BOARD_PWR_POLL_MS     0
#define BOARD_PIN_IMU_INT     21    /* QMI8658 INT1; unused, pulled down */
#define BOARD_PIN_TOUCH_RST   BSP_LCD_TOUCH_RST     /* GPIO9 */
#define BOARD_TOUCH_CHIP      3168  /* FT3168: asleep except in Settings (main/touch.c) */
#define BOARD_TOUCH_ADDR      0x38
#define BOARD_TOUCH_MIRROR    0     /* 1 = x and y both reversed relative to the panel */
#define BOARD_TOUCH_BOOT_MS   300   /* after the reset pulse, before it takes commands */
#define BOARD_PIN_SPEAKER_PA  BSP_POWER_AMP_IO      /* GPIO46 */
#define BOARD_PIN_MOTOR       18    /* MMBT3904 base: high = vibrate; kept low (main/parked.c) */

void board_init(i2c_master_bus_handle_t bus);
bool board_pwr_down(void);
