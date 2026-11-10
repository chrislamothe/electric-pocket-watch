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
    if (rd(0x24, &v) && (v & 0x07) != 4) {
        i2c_reg_write(s_axp, 0x24, (uint8_t)((v & 0xF8) | 4)); /* power cut at 3.0 V (2600 + 100*4) */
    }
    vTaskDelay(pdMS_TO_TICKS(20));                          /* first ADC conversion */
    ESP_LOGI(TAG, "battery %d mV (%d%% by voltage), gauge %d%%, charge state %d, charge target %d mV, power cut %d mV",
             axp_battery_mv(), axp_battery_percent(), axp_gauge_percent(), axp_charge_state(),
             axp_charge_target_mv(), axp_shutdown_mv());
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

/* 0x34 (high 5 bits) and 0x35 (low 8 bits) are separate registers. Read on their own, the high byte
 * of one conversion can pair with the low byte of the next: a value crossing a 256 mV boundary
 * (3.584, 3.840, 4.096 V) then reads up to 256 mV off, several indicator segments. Read
 * high-low-high and redo the low byte if the high byte moved. (Not a burst read: register
 * auto-increment is not documented for this PMIC.) */
static int vbat_once(void)
{
    uint8_t h1 = 0, l = 0, h2 = 0;
    if (!rd(0x34, &h1) || !rd(0x35, &l) || !rd(0x34, &h2)) {
        return -1;
    }
    if ((h1 & 0x1F) != (h2 & 0x1F) && !rd(0x35, &l)) {
        return -1;
    }
    return ((h2 & 0x1F) << 8) | l;
}

int axp_battery_mv(void)
{
    if (!battery_present()) {
        return -1;
    }
    int v[3];
    for (int i = 0; i < 3; i++) {                           /* median of three: drops one outlier */
        v[i] = vbat_once();
        if (v[i] < 0) {
            return -1;
        }
    }
    int lo = v[0] < v[1] ? v[0] : v[1], hi = v[0] < v[1] ? v[1] : v[0];
    return v[2] < lo ? lo : (v[2] > hi ? hi : v[2]);
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

int axp_shutdown_mv(void)
{
    uint8_t v = 0;
    return rd(0x24, &v) ? 2600 + 100 * (v & 0x07) : -1;
}

/* Screen-off (light-load) voltage vs charge, measured on this watch's cell (v0.8.3 battery log,
 * 2026-11-09): a full always-on discharge at constant load gave charge vs time, and the screen-off
 * readings of an earlier run matched that curve 35 mV higher (AXP_SCREEN_SAG_MV) to within 2 mV.
 * 0 % is where the watch stopped under display load (3.29 V loaded). */
int axp_mv_to_percent(int mv)
{
    static const int MV[]  = {3320, 3490, 3574, 3632, 3692, 3776, 3858, 3928, 4000, 4068, 4150};
    static const int PCT[] = {   0,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100};
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

static int s_rest_mv = -1;

void axp_rest_update(void)
{
    int mv = axp_battery_mv();
    if (mv > 0) {
        s_rest_mv = mv;
    }
}

void axp_rest_set_loaded(int mv)
{
    if (mv > 0) {
        s_rest_mv = mv + AXP_SCREEN_SAG_MV;
    }
}

int axp_rest_mv(void)
{
    return s_rest_mv > 0 ? s_rest_mv : axp_battery_mv();
}

int axp_rest_percent(void)
{
    int mv = axp_rest_mv();
    return mv > 0 ? axp_mv_to_percent(mv) : -1;
}

int axp_power_on_source(void)
{
    uint8_t v = 0;
    return rd(0x20, &v) ? v : -1;
}

int axp_power_off_source(void)
{
    uint8_t v = 0;
    return rd(0x21, &v) ? v : -1;
}

void axp_power_off(void)
{
    uint8_t v = 0;
    if (rd(0x10, &v)) {
        i2c_reg_write(s_axp, 0x10, v | 0x01);              /* software power-off */
    }
}
