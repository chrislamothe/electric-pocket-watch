#include "ext_rtc.h"

#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "rtc";

#define RTC_ADDR        0x51
#define REG_CTRL1       0x00
#define REG_SECONDS     0x04
#define CTRL1_STOP      0x20
#define CTRL1_12_24     0x02    /* 1 = 12-hour mode */
#define SECONDS_OS      0x80    /* oscillator stopped: time invalid */
#define VALID_FROM      1704067200LL    /* 2024-01-01 UTC */

static i2c_master_dev_handle_t s_dev;

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil). Avoids
 * mktime(), which depends on the TZ setting. */
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
        return false;
    }
    if (c1 & (CTRL1_STOP | CTRL1_12_24)) {          /* running, 24-hour mode; keep CAP_SEL etc. */
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

bool ext_rtc_restore_system_time(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    if (now.tv_sec >= VALID_FROM) {
        return false;            /* survived a reset in the RTC domain: finer than the RTC's 1 s */
    }
    uint8_t r[7];
    if (!rd(REG_SECONDS, r, sizeof(r))) {
        return false;
    }
    if (r[0] & SECONDS_OS) {
        ESP_LOGW(TAG, "RTC oscillator was stopped: time not valid until the next sync");
        return false;
    }
    int sec = bcd2bin(r[0] & 0x7F), min = bcd2bin(r[1] & 0x7F), hour = bcd2bin(r[2] & 0x3F);
    int day = bcd2bin(r[3] & 0x3F), mon = bcd2bin(r[5] & 0x1F), year = 2000 + bcd2bin(r[6]);
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        ESP_LOGW(TAG, "RTC holds an invalid date");
        return false;
    }
    long long t = days_from_civil(year, mon, day) * 86400LL + hour * 3600 + min * 60 + sec;
    if (t < VALID_FROM) {
        ESP_LOGW(TAG, "RTC time %04d-%02d-%02d is before 2024: not set", year, mon, day);
        return false;
    }
    struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 0};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "system time set from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC", year, mon, day, hour, min, sec);
    return true;
}

void ext_rtc_save_system_time(void)
{
    if (!s_dev) {
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < VALID_FROM) {
        return;
    }
    struct tm u;
    time_t s = tv.tv_sec;
    gmtime_r(&s, &u);
    uint8_t w[8] = {
        REG_SECONDS,
        bin2bcd(u.tm_sec),                              /* also clears the OS flag */
        bin2bcd(u.tm_min),
        bin2bcd(u.tm_hour),
        bin2bcd(u.tm_mday),
        (uint8_t)u.tm_wday,
        bin2bcd(u.tm_mon + 1),
        bin2bcd(u.tm_year % 100),
    };
    if (wr(w, sizeof(w))) {
        ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC", u.tm_year + 1900, u.tm_mon + 1,
                 u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
    } else {
        ESP_LOGW(TAG, "RTC write failed");
    }
}
