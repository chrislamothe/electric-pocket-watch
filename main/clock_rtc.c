#include "clock_rtc.h"

#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rtc";

#define RTC_ADDR        0x51
#define REG_CTRL1       0x00
#define REG_SECONDS     0x04
#define CTRL1_STOP      0x20
#define CTRL1_12_24     0x02
#define SECONDS_OS      0x80
#define XFER_MS         50
#define MIN_UTC         1704067200LL    /* 2024-01-01 */
#define MAX_UTC         4102444799LL    /* 2099-12-31 23:59:59 */

static i2c_master_dev_handle_t s_dev;

const char *pcf_status_text(pcf_status_t s)
{
    switch (s) {
    case PCF_OK:       return "ok";
    case PCF_ABSENT:   return "absent";
    case PCF_BUS:      return "I2C error";
    case PCF_STOPPED:  return "stopped";
    case PCF_OSC_LOST: return "oscillator stopped (time lost)";
    case PCF_BAD_DATA: return "invalid data";
    case PCF_NO_TICK:  return "not advancing";
    case PCF_VERIFY:   return "write not verified";
    }
    return "?";
}

/* Days since 1970-01-01 (H. Hinnant). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool bcd(uint8_t v, uint8_t mask, int *out)
{
    v &= mask;
    if ((v & 0x0F) > 9 || (v >> 4) > 9) {
        return false;
    }
    *out = (v >> 4) * 10 + (v & 0x0F);
    return true;
}

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return s_dev && i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, XFER_MS) == ESP_OK;
}

static bool wr(const uint8_t *buf, size_t n)
{
    return s_dev && i2c_master_transmit(s_dev, buf, n, XFER_MS) == ESP_OK;
}

static pcf_status_t decode(const uint8_t r[7], int64_t *utc_s)
{
    if (r[0] & SECONDS_OS) {
        return PCF_OSC_LOST;
    }
    int sec, min, hour, day, mon, yy;
    if (!bcd(r[0], 0x7F, &sec) || !bcd(r[1], 0x7F, &min) || !bcd(r[2], 0x3F, &hour) ||
        !bcd(r[3], 0x3F, &day) || !bcd(r[5], 0x1F, &mon) || !bcd(r[6], 0xFF, &yy)) {
        return PCF_BAD_DATA;
    }
    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int year = 2000 + yy;
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (mon < 1 || mon > 12 || day < 1 || day > mdays[mon - 1] + (mon == 2 && leap) || hour > 23 ||
        min > 59 || sec > 59) {
        return PCF_BAD_DATA;
    }
    int64_t t = days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
    if (t < MIN_UTC || t > MAX_UTC) {
        return PCF_BAD_DATA;
    }
    *utc_s = t;
    return PCF_OK;
}

bool pcf_init(i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, RTC_ADDR, XFER_MS) != ESP_OK) {
        ESP_LOGI(TAG, "no PCF85063 on this board");
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }
    uint8_t c[3] = {0};                              /* Control_1, Control_2, Offset */
    if (!rd(REG_CTRL1, c, sizeof(c))) {
        ESP_LOGW(TAG, "PCF85063 not answering");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }
    ESP_LOGI(TAG, "PCF85063: control1 0x%02X control2 0x%02X offset 0x%02X", c[0], c[1], c[2]);
    if (c[0] & CTRL1_12_24) {                        /* 24-hour mode; STOP is left for clock.c to judge */
        const uint8_t w[2] = {REG_CTRL1, (uint8_t)(c[0] & ~CTRL1_12_24)};
        wr(w, sizeof(w));
    }
    return true;
}

bool pcf_present(void)
{
    return s_dev != NULL;
}

static pcf_status_t ctrl_running(void)
{
    uint8_t c1 = 0;
    if (!rd(REG_CTRL1, &c1, 1)) {
        return PCF_BUS;
    }
    return (c1 & CTRL1_STOP) ? PCF_STOPPED : PCF_OK;
}

pcf_status_t pcf_read(int64_t *utc_s)
{
    if (!s_dev) {
        return PCF_ABSENT;
    }
    pcf_status_t st = ctrl_running();
    if (st != PCF_OK) {
        return st;
    }
    uint8_t r[7];
    if (!rd(REG_SECONDS, r, sizeof(r))) {
        return PCF_BUS;
    }
    return decode(r, utc_s);
}

pcf_status_t pcf_read_edge(int64_t *utc_s, int64_t *mono_us, int64_t *window_us)
{
    if (!s_dev) {
        return PCF_ABSENT;
    }
    pcf_status_t st = ctrl_running();
    if (st != PCF_OK) {
        return st;
    }
    uint8_t first[7], r[7];
    int64_t prev_start = esp_timer_get_time();
    if (!rd(REG_SECONDS, first, sizeof(first))) {
        return PCF_BUS;
    }
    int64_t t_first = 0;
    if ((st = decode(first, &t_first)) != PCF_OK) {
        return st;
    }
    const int64_t deadline = prev_start + 1300000;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(1);
        int64_t start = esp_timer_get_time();
        if (!rd(REG_SECONDS, r, sizeof(r))) {
            return PCF_BUS;
        }
        int64_t end = esp_timer_get_time();
        if (r[0] != first[0]) {
            int64_t t = 0;
            if ((st = decode(r, &t)) != PCF_OK) {
                return st;
            }
            if (t != t_first + 1) {
                return PCF_BAD_DATA;                 /* skipped or jumped: not a clean rollover */
            }
            /* The rollover lies between the start of the last old read and the end of this one. */
            *utc_s = t;
            *mono_us = (prev_start + end) / 2;
            *window_us = end - prev_start;
            return PCF_OK;
        }
        prev_start = start;
    }
    return PCF_NO_TICK;
}

pcf_status_t pcf_write(int64_t (*now_us)(void))
{
    if (!s_dev) {
        return PCF_ABSENT;
    }
    uint8_t c1 = 0;
    if (!rd(REG_CTRL1, &c1, 1)) {
        return PCF_BUS;
    }
    c1 &= ~(CTRL1_STOP | CTRL1_12_24);
    int64_t t = now_us();
    int64_t s = t / 1000000;
    if (t % 1000000 > 442000) {                      /* release point at least 50 ms ahead */
        s++;
    }
    if (s < MIN_UTC || s > MAX_UTC) {
        return PCF_BAD_DATA;
    }
    const uint8_t stop[2] = {REG_CTRL1, (uint8_t)(c1 | CTRL1_STOP)};
    if (!wr(stop, sizeof(stop))) {
        return PCF_BUS;                              /* never stopped: nothing to undo */
    }
    struct tm u;
    time_t st = (time_t)s;
    gmtime_r(&st, &u);
    const uint8_t w[8] = {
        REG_SECONDS, to_bcd(u.tm_sec),               /* writing seconds clears OS */
        to_bcd(u.tm_min), to_bcd(u.tm_hour), to_bcd(u.tm_mday), (uint8_t)u.tm_wday,
        to_bcd(u.tm_mon + 1), to_bcd(u.tm_year % 100),
    };
    bool loaded = wr(w, sizeof(w));
    const int64_t rel = s * 1000000 + 492130;
    for (int64_t left; loaded && (left = rel - now_us()) > 3000;) {
        vTaskDelay(pdMS_TO_TICKS(left / 1000 - 2 > 0 ? left / 1000 - 2 : 1));
    }
    while (loaded && now_us() < rel) {
    }
    /* Restart on every path. */
    const uint8_t go[2] = {REG_CTRL1, c1};
    bool running = false;
    for (int i = 0; i < 3 && !running; i++) {
        running = wr(go, sizeof(go)) && ctrl_running() == PCF_OK;
    }
    if (!running) {
        ESP_LOGE(TAG, "RTC could not be restarted");
        return PCF_STOPPED;
    }
    if (!loaded) {
        return PCF_BUS;
    }
    int64_t r = 0, m = 0, win = 0;
    pcf_status_t v = pcf_read_edge(&r, &m, &win);
    if (v != PCF_OK) {
        return v;
    }
    int64_t expect = (now_us() - (esp_timer_get_time() - m) + 500000) / 1000000;
    if (r != expect) {
        ESP_LOGE(TAG, "RTC read back %lld, expected %lld", (long long)r, (long long)expect);
        return PCF_VERIFY;
    }
    ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC, running and verified", u.tm_year + 1900,
             u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
    return PCF_OK;
}
