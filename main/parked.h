#pragma once

#include "driver/i2c_master.h"

/*
 * Hardware the watch does not use, put into its lowest-power state once at boot: the touch
 * controller, the accelerometer, the audio codecs and speaker amplifier, the vibration motor.
 * The watch is operated with the BOOT and PWR buttons only.
 *
 * Nothing here is required for the watch to work: a chip that does not answer is logged and
 * skipped. No pin is latched with gpio_hold (a latch survives resets and reflashing; v0.9.0 and
 * v0.10.0 latched the touch reset low, which stopped older firmware from starting). Latches left
 * by those versions are released here.
 *
 * Call after the display is up (the touch reset pulse follows the panel reset, as it did when the
 * touch driver ran) and before light sleep is first used.
 */
void parked_init(i2c_master_bus_handle_t bus);
