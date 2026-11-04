#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"

/*
 * AXP2101 PMIC (I2C 0x34). Only reads are used: the PWR key long-press power-off is
 * handled by the PMIC itself, and its IRQ line is not routed to the ESP32 on the 1.75C.
 * STATUS1 (0x00): bit5 VBUS good, bit3 battery present (XPowersLib XPowersAXP2101.hpp).
 * Battery percentage: register 0xA4.
 */
bool axp_init(i2c_master_bus_handle_t bus);
bool axp_vbus_good(void);
int axp_battery_percent(void);   /* 0..100, or -1 when no battery / read failed */
