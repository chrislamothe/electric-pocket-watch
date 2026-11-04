#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"

/*
 * QMI8658 (I2C 0x6B). INT1 is wired to GPIO21 (1.75C schematic); INT2 is not connected.
 * Verified on hardware (v0.0.8 log): wake-on-motion toggles GPIO21 high, and the IMU keeps
 * power across ESP32 resets so init always soft-resets it.
 *
 * Wake strategy: the sleeping watch arms wake-on-motion (low-power 128 Hz). Any motion that
 * exceeds the threshold raises INT1; the controller then switches the IMU to 250 Hz normal mode
 * and checks in software that a second tap follows. WoM mode reports zero acceleration data,
 * so tap detection is only possible in normal mode.
 */
#define PIN_IMU_INT1 GPIO_NUM_21

bool imu_init(i2c_master_bus_handle_t bus);

/* Accelerometer off (screen-on state). */
void imu_off(void);

/* Arm wake-on-motion with threshold in mg (1..255). Clears latched status. Returns false if
 * the CTRL9 handshake failed (WoM is then not armed). */
bool imu_arm_wom(uint8_t threshold_mg);

/* Wait (max_ms) for the watch to be still (|a| within quiet_mg of 1 g) for still_ms. Starts and
 * leaves the IMU in normal mode. Returns true if the watch settled in time. */
bool imu_wait_still(int still_ms, int max_ms, int quiet_mg);

/* After a WoM wake: 250 Hz normal mode, then look for a second tap within window_ms.
 * Returns true on a confirmed double tap. Leaves the IMU in normal mode. */
bool imu_confirm_double_tap(int window_ms, int tap_threshold_mg);
