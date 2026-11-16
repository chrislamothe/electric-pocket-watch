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
#define BOARD_PIN_IMU_INT     21    /* QMI8658 INT1 (INT2 only to a test pad) */
#define BOARD_IMU_INT         1
#define BOARD_PIN_TOUCH_RST   BSP_LCD_TOUCH_RST     /* GPIO9 */
/* The FT3168 driver writes its configuration at start-up only; after a hardware reset that
 * configuration would be lost, so the touch controller is not held in reset while dark. */
#define BOARD_TOUCH_SLEEP_RESET 0
#define BOARD_PIN_SPEAKER_PA  BSP_POWER_AMP_IO      /* GPIO46 */
#define BOARD_PIN_MOTOR       18    /* MMBT3904 base: high = vibrate; kept low */

void board_init(i2c_master_bus_handle_t bus);
bool board_pwr_down(void);
