#include "ext_rtc.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "rtc";

#define RTC_ADDR        0x51
#define REG_CTRL1       0x00
#define REG_SECONDS     0x04
#define CTRL1_STOP      0x20
#define CTRL1_12_24     0x02    /* 1 = 12-hour mode */
#define SECONDS_OS      0x80    /* oscillator stopped: time invalid */
#define VALID_FROM      1704067200LL    /* 2024-01-01 UTC */

static i2c_master_dev_handle_t s_dev;
static bool s_stale;                /* found stopped at boot: frozen time, invalid until written */

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Days since 1970-01-01 (H. Hinnant's days_from_civil); independent of TZ. */
static long long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static bool rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return s_dev && i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK;
}

static bool wr(const uint8_t *buf, size_t n)
{
    return s_dev && i2c_master_transmit(s_dev, buf, n, 100) == ESP_OK;
}

static bool regs_to_utc(const uint8_t r[7], long long *out)
{
    if (r[0] & SECONDS_OS) {
        return false;
    }
    int sec = bcd2bin(r[0] & 0x7F), min = bcd2bin(r[1] & 0x7F), hour = bcd2bin(r[2] & 0x3F);
    int day = bcd2bin(r[3] & 0x3F), mon = bcd2bin(r[5] & 0x1F), year = 2000 + bcd2bin(r[6]);
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        return false;
    }
    *out = days_from_civil(year, mon, day) * 86400LL + hour * 3600 + min * 60 + sec;
    return *out >= VALID_FROM;
}

bool ext_rtc_init(i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, RTC_ADDR, 50) != ESP_OK) {
        ESP_LOGI(TAG, "no PCF85063 on this board");
        return false;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }
    uint8_t c1 = 0;
    if (!rd(REG_CTRL1, &c1, 1)) {
        ESP_LOGW(TAG, "PCF85063 not answering");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }
    if (c1 & CTRL1_STOP) {
        s_stale = true;                              /* e.g. a write interrupted mid-sequence */
        ESP_LOGW(TAG, "PCF85063 was stopped: its time is stale until the next sync");
    }
    if (c1 & (CTRL1_STOP | CTRL1_12_24)) {           /* run, 24-hour mode; keep CAP_SEL etc. */
        uint8_t w[2] = {REG_CTRL1, (uint8_t)(c1 & ~(CTRL1_STOP | CTRL1_12_24))};
        wr(w, sizeof(w));
    }
    ESP_LOGI(TAG, "PCF85063 ready (control 0x%02X)", c1);
    return true;
}

bool ext_rtc_present(void)
{
    return s_dev != NULL;
}

bool ext_rtc_read(long long *utc_s)
{
    uint8_t r[7];
    return !s_stale && rd(REG_SECONDS, r, sizeof(r)) && regs_to_utc(r, utc_s);
}

bool ext_rtc_read_edge(long long *utc_s, int64_t *mono_us)
{
    uint8_t first[7], r[7];
    if (s_stale || !rd(REG_SECONDS, first, sizeof(first))) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 1200000) {
        vTaskDelay(pdMS_TO_TICKS(2));
        int64_t before = esp_timer_get_time();
        if (!rd(REG_SECONDS, r, sizeof(r))) {
            return false;
        }
        int64_t after = esp_timer_get_time();
        if (r[0] != first[0]) {
            /* The change happened between the previous read and this one (~2 ms apart). */
            *mono_us = (before + after) / 2 - 1000;
            return regs_to_utc(r, utc_s);
        }
    }
    ESP_LOGW(TAG, "seconds register did not advance in 1.2 s");
    return false;
}

static bool release(uint8_t c1)
{
    const uint8_t go[2] = {REG_CTRL1, c1};
    for (int i = 0; i < 3; i++) {
        if (wr(go, sizeof(go))) {
            return true;
        }
    }
    return false;
}

bool ext_rtc_write_aligned(int64_t (*utc_now_us)(void))
{
    if (!s_dev) {
        return false;
    }
    uint8_t c1 = 0;
    if (!rd(REG_CTRL1, &c1, 1)) {
        return false;
    }
    c1 &= ~(CTRL1_STOP | CTRL1_12_24);
    int64_t t = utc_now_us();
    long long s = t / 1000000;
    if (t % 1000000 > 442000) {                      /* release point must be >= 50 ms ahead */
        s++;
    }
    const uint8_t stop[2] = {REG_CTRL1, (uint8_t)(c1 | CTRL1_STOP)};
    if (!wr(stop, sizeof(stop))) {
        return false;                                /* not stopped: old time keeps running */
    }
    struct tm u;
    time_t st = (time_t)s;
    gmtime_r(&st, &u);
    const uint8_t w[8] = {
        REG_SECONDS,
        bin2bcd(u.tm_sec),                           /* also clears the OS flag */
        bin2bcd(u.tm_min),
        bin2bcd(u.tm_hour),
        bin2bcd(u.tm_mday),
        (uint8_t)u.tm_wday,
        bin2bcd(u.tm_mon + 1),
        bin2bcd(u.tm_year % 100),
    };
    bool loaded = wr(w, sizeof(w));
    const int64_t rel = s * 1000000 + 492130;
    int64_t left;
    while ((left = rel - utc_now_us()) > 3000) {
        vTaskDelay(pdMS_TO_TICKS(left / 1000 - 2 > 0 ? left / 1000 - 2 : 1));
    }
    while (utc_now_us() < rel) {
    }
    if (!release(c1)) {
        ESP_LOGE(TAG, "could not restart the RTC: it is stopped");
        s_stale = true;
        return false;
    }
    if (!loaded) {
        ESP_LOGE(TAG, "time registers not written: RTC keeps its old time");
        return false;
    }
    /* Verify: ~0.6 s after the release it must read S+1. */
    vTaskDelay(pdMS_TO_TICKS(620));
    long long check = 0;
    s_stale = false;
    if (!ext_rtc_read(&check) || check != s + 1) {
        ESP_LOGE(TAG, "RTC check failed after write (read %lld, expected %lld)", check, s + 1);
        s_stale = true;
        return false;
    }
    ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC, verified", u.tm_year + 1900, u.tm_mon + 1,
             u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
    return true;
}
