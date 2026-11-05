#include "axp.h"
#include "i2c_dev.h"
#include "esp_log.h"

static const char *TAG = "axp";

#define AXP_ADDR        0x34
#define AXP_REG_STATUS1 0x00
#define AXP_REG_CHIPID  0x03
#define AXP_REG_BATPCT  0xA4

static i2c_master_dev_handle_t s_axp;

bool axp_init(i2c_master_bus_handle_t bus)
{
    s_axp = i2c_dev_add(bus, AXP_ADDR);
    if (!s_axp) {
        return false;
    }
    uint8_t id = 0;
    if (i2c_reg_read(s_axp, AXP_REG_CHIPID, &id, 1) != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not answering");
        return false;
    }
    ESP_LOGI(TAG, "AXP2101 chip id reg = 0x%02X (expect 0x4A)", id);
    return true;
}

bool axp_vbus_good(void)
{
    uint8_t st = 0;
    if (!s_axp || i2c_reg_read(s_axp, AXP_REG_STATUS1, &st, 1) != ESP_OK) {
        return false;
    }
    return (st & 0x20) != 0;
}

int axp_battery_percent(void)
{
    uint8_t st = 0, pct = 0;
    if (!s_axp || i2c_reg_read(s_axp, AXP_REG_STATUS1, &st, 1) != ESP_OK || !(st & 0x08)) {
        return -1;
    }
    if (i2c_reg_read(s_axp, AXP_REG_BATPCT, &pct, 1) != ESP_OK) {
        return -1;
    }
    return pct > 100 ? 100 : pct;
}

void axp_configure_power_key(void)
{
    uint8_t en = 0, lvl = 0;
    if (!s_axp || i2c_reg_read(s_axp, 0x22, &en, 1) != ESP_OK || i2c_reg_read(s_axp, 0x27, &lvl, 1) != ESP_OK) {
        return;
    }
    uint8_t en_new = (uint8_t)((en | 0x02) & ~0x01);        /* long press shuts down, not restart */
    uint8_t lvl_new = (uint8_t)((lvl & 0xF3) | (1 << 2));   /* 6 s */
    if (en_new != en) {
        i2c_reg_write(s_axp, 0x22, en_new);
    }
    if (lvl_new != lvl) {
        i2c_reg_write(s_axp, 0x27, lvl_new);
    }
    i2c_reg_read(s_axp, 0x22, &en, 1);
    i2c_reg_read(s_axp, 0x27, &lvl, 1);
    ESP_LOGI(TAG, "power key: 0x22=%02X 0x27=%02X (hold PWR %d s to power off)", en, lvl, 4 + 2 * ((lvl >> 2) & 3));
}
