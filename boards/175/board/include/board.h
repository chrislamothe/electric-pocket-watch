#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.75 (SKU 31261), -1.75-B (31262, same board in a protective
 * case) and -1.75-G (31264, adds an LC76G GNSS module). Round 466x466 CO5300, CST9217 touch,
 * AXP2101, QMI8658, PCF85063 RTC, TCA9554 I/O expander, TF card.
 *
 * Differences from the 1.75C that matter here (schematic + Waveshare HARDWARE_REFERENCE.md):
 * - LCD reset GPIO39, touch reset GPIO40 (1.75C: GPIO1 / GPIO2); handled by the board package.
 * - The PWR state (SYS_OUT) is on TCA9554 P4, not on a GPIO, and the TCA9554 INT output is not
 *   connected to the ESP32. PWR therefore cannot wake light sleep directly: while the screen is
 *   off the watch wakes every BOARD_PWR_POLL_MS to read it. BOOT wakes it directly.
 * - QMI8658 INT1 goes to TCA9554 P6; INT2 goes straight to GPIO21 (the accelerometer is unused).
 * - PCF85063 RTC at 0x51, kept running from the AXP2101 RTC supply.
 */
#define BOARD_ID              "175"
#define BOARD_NAME            "ESP32-S3-Touch-AMOLED-1.75 / 1.75-B"
#define BOARD_SHORT           "1.75"

#define BOARD_PIN_BOOT        0     /* active low, 10K pull-up */
#define BOARD_PIN_PWR         -1    /* SYS_OUT on TCA9554 P4 */
#define BOARD_PWR_POLL_MS     200
#define BOARD_PIN_IMU_INT     21    /* QMI8658 INT2; unused, pulled down */
#define BOARD_PIN_TOUCH_RST   BSP_LCD_TOUCH_RST     /* GPIO40 */
#define BOARD_TOUCH_CHIP      9217  /* CST9217: unused, put to sleep at boot (main/parked.c) */
#define BOARD_TOUCH_ADDR      0x5A
#define BOARD_TOUCH_BOOT_MS   60
#define BOARD_PIN_SPEAKER_PA  BSP_POWER_AMP_IO      /* GPIO46 */
#define BOARD_PIN_MOTOR       -1

void board_init(i2c_master_bus_handle_t bus);
bool board_pwr_down(void);
