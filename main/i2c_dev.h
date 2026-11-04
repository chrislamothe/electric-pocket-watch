#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Thin register helpers on top of the BSP's shared I2C master bus (SCL 14, SDA 15). */

i2c_master_dev_handle_t i2c_dev_add(i2c_master_bus_handle_t bus, uint8_t addr);
esp_err_t i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n);
esp_err_t i2c_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value);
