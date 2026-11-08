#include "axp.h"
#include "i2c_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "axp";

#define AXP_ADDR        0x34
#define AXP_REG_STATUS1 0x00
#define AXP_REG_CHIPID  0x03
#define AXP_REG_BATPCT  0xA4

static i2c_master_dev_handle_t s_axp;

static bool rd(uint8_t reg, uint8_t *v);

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
    uint8_t v = 0;
    if (rd(0x30, &v) && !(v & 0x01)) {
        i2c_reg_write(s_axp, 0x30, v | 0x01);              /* battery voltage ADC on */
    }
    if (rd(0x68, &v) && !(v & 0x01)) {
        i2c_reg_write(s_axp, 0x68, v | 0x01);              /* battery detection on */
    }
    vTaskDelay(pdMS_TO_TICKS(20));                          /* first ADC conversion */
    ESP_LOGI(TAG, "battery %d mV (%d%% by voltage), gauge %d%%, charge state %d, charge target %d mV",
             axp_battery_mv(), axp_battery_percent(), axp_gauge_percent(), axp_charge_state(),
             axp_charge_target_mv());
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

static bool rd(uint8_t reg, uint8_t *v)
{
    return s_axp && i2c_reg_read(s_axp, reg, v, 1) == ESP_OK;
}

static bool battery_present(void)
{
    uint8_t st = 0;
    return rd(AXP_REG_STATUS1, &st) && (st & 0x08);
}

int axp_battery_mv(void)
{
    uint8_t h = 0, l = 0;
    if (!battery_present() || !rd(0x34, &h) || !rd(0x35, &l)) {
        return -1;
    }
    return ((h & 0x1F) << 8) | l;
}

int axp_gauge_percent(void)
{
    uint8_t pct = 0;
    if (!battery_present() || !rd(AXP_REG_BATPCT, &pct)) {
        return -1;
    }
    return pct > 100 ? 100 : pct;
}

axp_charge_t axp_charge_state(void)
{
    uint8_t st = 0;
    if (!rd(0x01, &st)) {
        return AXP_CHG_UNKNOWN;
    }
    uint8_t v = st >> 5;
    return v <= 2 ? (axp_charge_t)v : AXP_CHG_UNKNOWN;
}

int axp_charge_target_mv(void)
{
    static const int MV[] = {-1, 4000, 4100, 4200, 4350, 4400, -1, -1};
    uint8_t v = 0;
    return rd(0x64, &v) ? MV[v & 0x07] : -1;
}

/* Typical single-cell LiPo resting voltage vs state of charge (4.2 V full). Light-load readings,
 * which is what the watch sees most of the time; under load the voltage sags a little. */
int axp_mv_to_percent(int mv)
{
    static const int MV[]  = {3300, 3500, 3610, 3670, 3710, 3750, 3800, 3850, 3920, 4000, 4100, 4180};
    static const int PCT[] = {   0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100};
    const int n = sizeof(MV) / sizeof(MV[0]);
    if (mv <= MV[0]) {
        return 0;
    }
    if (mv >= MV[n - 1]) {
        return 100;
    }
    for (int i = 1; i < n; i++) {
        if (mv <= MV[i]) {
            return PCT[i - 1] + (PCT[i] - PCT[i - 1]) * (mv - MV[i - 1]) / (MV[i] - MV[i - 1]);
        }
    }
    return 100;
}

int axp_battery_percent(void)
{
    int mv = axp_battery_mv();
    return mv > 0 ? axp_mv_to_percent(mv) : -1;
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
