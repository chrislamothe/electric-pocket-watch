#pragma once

#include "driver/i2c_master.h"

/* Clock lab firmware (CONFIG_WATCH_CLOCK_LAB): light-sleep timing experiments. Never returns. */
void clocklab_run(i2c_master_bus_handle_t bus);
