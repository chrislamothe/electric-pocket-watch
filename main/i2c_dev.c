#include "i2c_dev.h"
#include "esp_log.h"

static const char *TAG = "i2c";

#define I2C_TIMEOUT_MS 100

i2c_master_dev_handle_t i2c_dev_add(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "add_device 0x%02X failed", addr);
        return NULL;
    }
    return dev;
}

esp_err_t i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, I2C_TIMEOUT_MS);
}

esp_err_t i2c_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t tmp[2] = {reg, value};
    return i2c_master_transmit(dev, tmp, sizeof(tmp), I2C_TIMEOUT_MS);
}
