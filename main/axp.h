#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"

/*
 * AXP2101 PMIC (I2C 0x34). Its IRQ line is not routed to the ESP32 on the 1.75C, so it is polled.
 * Register map per XPowersLib (XPowersAXP2101.hpp):
 *   0x00 STATUS1: bit5 VBUS good, bit3 battery present
 *   0x01 STATUS2: bits[7:5] 0 standby, 1 charging, 2 discharging
 *   0x30 ADC channel enable: bit0 battery voltage
 *   0x34/0x35 battery voltage, 13 bits ((0x34 & 0x1F) << 8 | 0x35), mV
 *   0x64 charge target voltage: bits[2:0] 1=4.0 2=4.1 3=4.2 4=4.35 5=4.4 V
 *   0x68 bit0 battery detection enable
 *   0xA4 fuel-gauge percentage (depends on battery parameters loaded in the PMIC)
 */
typedef enum { AXP_CHG_STANDBY = 0, AXP_CHG_CHARGING = 1, AXP_CHG_DISCHARGING = 2, AXP_CHG_UNKNOWN = 3 } axp_charge_t;

bool axp_init(i2c_master_bus_handle_t bus);
bool axp_vbus_good(void);

/* PWR long press = PMIC power-off after 6 s (0x22 bit1 on, bit0 off; 0x27 bits[3:2]). */
void axp_configure_power_key(void);

int axp_battery_mv(void);            /* battery voltage in mV, -1 if no battery / read failed */
int axp_gauge_percent(void);         /* PMIC fuel-gauge %, -1 if unavailable */
axp_charge_t axp_charge_state(void);
int axp_charge_target_mv(void);      /* configured charge termination voltage, -1 if unknown */
int axp_shutdown_mv(void);           /* VOFF: system powers off below this (0x24 bits[2:0], 2600 + 100*n) */

/* Battery % used by the watch: from the measured voltage on a single-cell LiPo curve. The PMIC
 * gauge is only reported alongside, because its estimate depends on battery parameters that are
 * not known to match this cell. -1 when there is no battery. */
int axp_battery_percent(void);

/* "At rest" reading: taken while the display is dark (on wake before the screen lights, and by the
 * battery log while asleep). The AMOLED's current makes the cell voltage dip by an amount that
 * depends on how bright the face is, so live readings differ between faces; the indicator uses
 * this instead. Falls back to a live reading until the first rest sample exists. */
void axp_rest_update(void);
int axp_rest_mv(void);
int axp_rest_percent(void);
int axp_mv_to_percent(int mv);
