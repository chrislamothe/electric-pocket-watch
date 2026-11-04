/*
 * Electric Pocket Watch - bring-up firmware v0.0.3
 *
 * Purpose: verify the real wiring of the Waveshare ESP32-S3-Touch-AMOLED-1.75C
 * before the watch firmware is written. Shows on screen and logs over USB serial:
 *   - I2C bus scan (repeated every 5 s, changes logged), plus a register dump of
 *     any unrecognised device
 *   - AXP2101 interrupt status: power-key IRQs enabled, status cleared after each
 *     read (write-1-to-clear), press/release/short counters, live GPIO3 level
 *     (hypothesis: GPIO3 is the PMIC IRQ line)
 *   - PCF85063 RTC time if one answers at 0x51 (set to build time if invalid)
 *   - QMI8658 IMU: WHO_AM_I, live acceleration, shake/tap spike detector
 *   - BOOT button (GPIO0) presses
 *   - Candidate IMU interrupt pin (GPIO21) edge counter
 *   - Scan of every free GPIO for level changes (finds the PWR button / interrupt pins)
 *   - AXP2101 raw ID, battery percentage and interrupt status registers
 *   - Touch coordinates
 *
 * Register maps for QMI8658 and PCF85063 are from the vendor datasheets;
 * AXP2101 reads are best-effort and only logged raw.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "pocketwatch";

#define PIN_BOOT          GPIO_NUM_0
#define PIN_IMU_INT2      GPIO_NUM_21   /* candidate, from the standard 1.75 reference */

#define ADDR_AXP2101      0x34
#define ADDR_RTC          0x51
#define ADDR_IMU_A        0x6B
#define ADDR_IMU_B        0x6A

#define I2C_TIMEOUT_MS    100

/*
 * GPIOs not claimed by flash/PSRAM (26-37), the display (4-7, 12, 38), I2C (14, 15),
 * touch INT (11), resets (1, 2), USB (19, 20) or the I2S/audio pins (8, 9, 10, 45, 46).
 * Their levels are watched for changes to locate buttons and interrupt lines.
 */
static const int8_t SCAN_PINS[] = {3, 13, 16, 17, 18, 39, 40, 41, 42, 43, 44, 47, 48};
#define SCAN_COUNT ((int)(sizeof(SCAN_PINS) / sizeof(SCAN_PINS[0])))

/* Text shared between the monitor task and the UI loop. Tearing is harmless here. */
static char s_time[24]  = "--:--:--";
static char s_date[24]  = "";
static char s_bat[32]   = "";
static char s_scan[160] = "I2C scanning...";
static char s_imu[96]   = "IMU: ...";
static char s_btn[96]   = "BOOT: ...";
static char s_gpio[96]  = "GPIO ev: -";
static char s_low[64]   = "";
static char s_axp[96]   = "";
static char s_touch[48] = "touch: -";

static volatile uint32_t s_int2_edges = 0;

static void IRAM_ATTR int2_isr(void *arg)
{
    s_int2_edges++;
}

/* ---------------------------------------------------------------- I2C helpers */

static i2c_master_dev_handle_t add_dev(i2c_master_bus_handle_t bus, uint8_t addr)
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

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tmp[16];
    if (n > sizeof(tmp) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    memcpy(tmp + 1, buf, n);
    return i2c_master_transmit(dev, tmp, n + 1, I2C_TIMEOUT_MS);
}

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* ---------------------------------------------------------------- RTC (PCF85063 at 0x51) */

static void build_time(uint8_t *h, uint8_t *m, uint8_t *s, uint8_t *d, uint8_t *mo, uint8_t *y)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int dd = 1, yy = 2026, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &dd, &yy);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *p = strstr(months, mon);
    *mo = p ? (uint8_t)((p - months) / 3 + 1) : 1;
    *d = (uint8_t)dd;
    *y = (uint8_t)(yy - 2000);
    *h = (uint8_t)hh;
    *m = (uint8_t)mm;
    *s = (uint8_t)ss;
}

static void rtc_set_to_build_time(i2c_master_dev_handle_t rtc)
{
    uint8_t h, m, s, d, mo, y;
    build_time(&h, &m, &s, &d, &mo, &y);
    uint8_t buf[7] = {
        bin2bcd(s),   /* bit7 (OS) cleared */
        bin2bcd(m),
        bin2bcd(h),   /* 24-hour mode (Control_1 bit1 = 0 by default) */
        bin2bcd(d),
        0,            /* weekday unused for now */
        bin2bcd(mo),
        bin2bcd(y),
    };
    esp_err_t err = reg_write(rtc, 0x04, buf, sizeof(buf));
    ESP_LOGW(TAG, "RTC set to build time (UTC from CI) %02u:%02u:%02u: %s",
             h, m, s, esp_err_to_name(err));
}

static void rtc_poll(i2c_master_dev_handle_t rtc)
{
    uint8_t r[7];
    if (reg_read(rtc, 0x04, r, sizeof(r)) != ESP_OK) {
        snprintf(s_time, sizeof(s_time), "RTC err");
        return;
    }
    bool osc_stopped = (r[0] & 0x80) != 0;
    uint8_t sec = bcd2bin(r[0] & 0x7F);
    uint8_t min = bcd2bin(r[1] & 0x7F);
    uint8_t hr  = bcd2bin(r[2] & 0x3F);
    uint8_t day = bcd2bin(r[3] & 0x3F);
    uint8_t mon = bcd2bin(r[5] & 0x1F);
    uint8_t yr  = bcd2bin(r[6]);

    if (osc_stopped || yr < 25 || mon == 0 || mon > 12 || day == 0) {
        rtc_set_to_build_time(rtc);
        return;
    }
    snprintf(s_time, sizeof(s_time), "%02u:%02u:%02u", hr, min, sec);
    snprintf(s_date, sizeof(s_date), "20%02u-%02u-%02u", yr, mon, day);
}

/* ---------------------------------------------------------------- I2C scanning */

static bool is_known_addr(uint8_t a)
{
    /* ES8311, AXP2101, ES7210, RTC, touch, IMU */
    return a == 0x18 || a == 0x34 || a == 0x40 || a == 0x51 || a == 0x5A || a == 0x6A || a == 0x6B;
}

static void scan_bus(i2c_master_bus_handle_t bus, uint8_t *found)
{
    for (uint8_t a = 0x08; a < 0x78; a++) {
        found[a] = (i2c_master_probe(bus, a, 20) == ESP_OK) ? 1 : 0;
    }
}

static void dump_unknown(i2c_master_bus_handle_t bus, uint8_t a)
{
    i2c_master_dev_handle_t d = add_dev(bus, a);
    if (!d) {
        return;
    }
    uint8_t b[16] = {0};
    esp_err_t e = reg_read(d, 0x00, b, sizeof(b));
    ESP_LOGI(TAG, "unknown device 0x%02X, read regs 0x00..0x0F: %s", a, esp_err_to_name(e));
    ESP_LOG_BUFFER_HEX(TAG, b, sizeof(b));
    i2c_master_bus_rm_device(d);
}

static void format_scan(const uint8_t *now, const uint8_t *ever)
{
    size_t o = (size_t)snprintf(s_scan, sizeof(s_scan), "I2C:");
    for (uint8_t a = 0x08; a < 0x78 && o < sizeof(s_scan) - 4; a++) {
        if (now[a]) {
            o += (size_t)snprintf(s_scan + o, sizeof(s_scan) - o, " %02X", a);
        }
    }
    bool first = true;
    for (uint8_t a = 0x08; a < 0x78 && o < sizeof(s_scan) - 12; a++) {
        if (ever[a] && !now[a]) {
            o += (size_t)snprintf(s_scan + o, sizeof(s_scan) - o, "%s %02X", first ? "  gone:" : "", a);
            first = false;
        }
    }
}

/* ---------------------------------------------------------------- monitor task */

static void monitor_task(void *arg)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    uint8_t found[128] = {0};
    uint8_t ever[128] = {0};

    /* --- I2C scan --- */
    scan_bus(bus, found);
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (found[a]) {
            ever[a] = 1;
            ESP_LOGI(TAG, "I2C device at 0x%02X", a);
        }
    }
    format_scan(found, ever);

    /* --- Register dump of devices we do not recognise --- */
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (found[a] && !is_known_addr(a)) {
            dump_unknown(bus, a);
        }
    }

    i2c_master_dev_handle_t rtc = found[ADDR_RTC]     ? add_dev(bus, ADDR_RTC)     : NULL;
    i2c_master_dev_handle_t axp = found[ADDR_AXP2101] ? add_dev(bus, ADDR_AXP2101) : NULL;
    uint8_t imu_addr = found[ADDR_IMU_A] ? ADDR_IMU_A : (found[ADDR_IMU_B] ? ADDR_IMU_B : 0);
    i2c_master_dev_handle_t imu  = imu_addr ? add_dev(bus, imu_addr) : NULL;

    ESP_LOGI(TAG, "present: rtc@51=%d axp2101=%d imu=0x%02X", rtc != NULL, axp != NULL, imu_addr);
    if (!rtc) {
        snprintf(s_time, sizeof(s_time), "no RTC@51");
    }

    /* --- AXP2101: log status and interrupt-enable registers once --- */
    if (axp) {
        uint8_t st[2] = {0}, en[3] = {0};
        reg_read(axp, 0x00, st, sizeof(st));
        reg_read(axp, 0x40, en, sizeof(en));
        ESP_LOGI(TAG, "AXP2101 status 0x00/0x01 = %02X %02X, INTEN 0x40..0x42 = %02X %02X %02X",
                 st[0], st[1], en[0], en[1], en[2]);

        /* Enable the power-key IRQs so the PMIC IRQ line (if it reaches the ESP32)
         * asserts on a PWR press: 0x41 bit0 = PWR_ON positive edge, bit1 = negative
         * edge (Zephyr mfd_axp2101), bit3 = short press (docs.rs axp2101-embedded). */
        uint8_t en1 = (uint8_t)(en[1] | 0x0B);
        esp_err_t e1 = reg_write(axp, 0x41, &en1, 1);
        ESP_LOGI(TAG, "AXP2101 INTEN1 0x41 -> %02X: %s", en1, esp_err_to_name(e1));

        /* Clear any latched status (write 1 to clear) so the IRQ line starts released. */
        uint8_t pre[3] = {0};
        reg_read(axp, 0x48, pre, sizeof(pre));
        ESP_LOGI(TAG, "AXP2101 INTSTS1..3 at start = %02X %02X %02X (clearing)", pre[0], pre[1], pre[2]);
        for (uint8_t r = 0; r < 3; r++) {
            uint8_t ff = 0xFF;
            reg_write(axp, (uint8_t)(0x48 + r), &ff, 1);
        }
        reg_read(axp, 0x48, pre, sizeof(pre));
        ESP_LOGI(TAG, "AXP2101 INTSTS1..3 after clear = %02X %02X %02X", pre[0], pre[1], pre[2]);
    }

    /* --- IMU init: accel only, +/-4g, 250 Hz normal mode --- */
    if (imu) {
        uint8_t who = 0;
        reg_read(imu, 0x00, &who, 1);
        ESP_LOGI(TAG, "QMI8658 WHO_AM_I = 0x%02X (expect 0x05)", who);
        uint8_t v = 0x40;               /* CTRL1: address auto-increment */
        reg_write(imu, 0x02, &v, 1);
        v = 0x15;                       /* CTRL2: aFS=+/-4g (001), aODR=250Hz (0101) */
        reg_write(imu, 0x03, &v, 1);
        v = 0x01;                       /* CTRL7: accel enable */
        reg_write(imu, 0x08, &v, 1);
    }

    /* --- GPIOs --- */
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_cfg);

    gpio_config_t int2_cfg = {
        .pin_bit_mask = 1ULL << PIN_IMU_INT2,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&int2_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_IMU_INT2, int2_isr, NULL);

    uint64_t scan_mask = 0;
    for (int i = 0; i < SCAN_COUNT; i++) {
        scan_mask |= 1ULL << SCAN_PINS[i];
    }
    gpio_config_t scan_cfg = {
        .pin_bit_mask = scan_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&scan_cfg);

    int scan_level[SCAN_COUNT];
    int scan_cand[SCAN_COUNT];
    int scan_cnt[SCAN_COUNT];
    char boot_levels[160];
    size_t bo = 0;
    bo += snprintf(boot_levels, sizeof(boot_levels), "GPIO levels at start:");
    for (int i = 0; i < SCAN_COUNT; i++) {
        scan_level[i] = gpio_get_level(SCAN_PINS[i]);
        scan_cand[i] = scan_level[i];
        scan_cnt[i] = 0;
        bo += snprintf(boot_levels + bo, sizeof(boot_levels) - bo, " %d=%d", SCAN_PINS[i], scan_level[i]);
    }
    ESP_LOGI(TAG, "%s", boot_levels);

    int boot_prev = gpio_get_level(PIN_BOOT);
    uint32_t boot_presses = 0;

    uint32_t pk_dn = 0, pk_up = 0, pk_short = 0, pk_b2 = 0;
    char axp_last[32] = "AXP irq: none yet";
    int64_t last_scan_ms = 0;

    float peak_dev_mg = 0;
    uint32_t spikes = 0;
    int64_t last_spike_ms = 0;
    int64_t window_start_ms = esp_timer_get_time() / 1000;
    float window_peak = 0;
    float last_window_peak = 0;
    int64_t last_slow_ms = 0;
    uint32_t loop_n = 0;

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        loop_n++;

        /* BOOT button */
        int lvl = gpio_get_level(PIN_BOOT);
        if (lvl != boot_prev) {
            boot_prev = lvl;
            if (lvl == 0) {
                boot_presses++;
            }
            ESP_LOGI(TAG, "BOOT %s (presses=%" PRIu32 ")", lvl ? "released" : "pressed", boot_presses);
        }
        snprintf(s_btn, sizeof(s_btn), "BOOT %s n=%" PRIu32 "  INT2 edges=%" PRIu32,
                 lvl ? "up" : "DOWN", boot_presses, s_int2_edges);

        /* Free-GPIO scan: accept a change after 3 consecutive differing samples */
        for (int i = 0; i < SCAN_COUNT; i++) {
            int l = gpio_get_level(SCAN_PINS[i]);
            if (l == scan_level[i]) {
                scan_cnt[i] = 0;
                continue;
            }
            if (l == scan_cand[i]) {
                scan_cnt[i]++;
            } else {
                scan_cand[i] = l;
                scan_cnt[i] = 1;
            }
            if (scan_cnt[i] >= 3) {
                scan_level[i] = l;
                scan_cnt[i] = 0;
                ESP_LOGI(TAG, "GPIO%d -> %d at %" PRId64 " ms", SCAN_PINS[i], l, now_ms);
                snprintf(s_gpio, sizeof(s_gpio), "GPIO%d -> %d @%" PRId64 "s", SCAN_PINS[i], l, now_ms / 1000);
            }
        }

        /* Levels of the free GPIOs that are low right now (raw, not debounced), ~50 ms */
        if ((loop_n % 10) == 0) {
            size_t lo = (size_t)snprintf(s_low, sizeof(s_low), "low now:");
            bool any = false;
            for (int i = 0; i < SCAN_COUNT; i++) {
                if (gpio_get_level(SCAN_PINS[i]) == 0 && lo < sizeof(s_low) - 6) {
                    lo += (size_t)snprintf(s_low + lo, sizeof(s_low) - lo, " %d", SCAN_PINS[i]);
                    any = true;
                }
            }
            if (!any) {
                snprintf(s_low + lo, sizeof(s_low) - lo, " none");
            }
        }

        /* AXP2101 interrupt status: read, count, log, then clear (write 1 to clear) */
        if (axp && (loop_n % 10) == 5) {
            uint8_t irq[3] = {0};
            if (reg_read(axp, 0x48, irq, sizeof(irq)) == ESP_OK && (irq[0] | irq[1] | irq[2])) {
                ESP_LOGI(TAG, "AXP2101 INTSTS1..3 = %02X %02X %02X at %" PRId64 " ms, GPIO3=%d",
                         irq[0], irq[1], irq[2], now_ms, gpio_get_level(GPIO_NUM_3));
                pk_up    += (irq[1] & 0x01) ? 1 : 0;   /* PWR_ON positive edge (release) */
                pk_dn    += (irq[1] & 0x02) ? 1 : 0;   /* PWR_ON negative edge (press)   */
                pk_b2    += (irq[1] & 0x04) ? 1 : 0;   /* unverified: probably long press */
                pk_short += (irq[1] & 0x08) ? 1 : 0;   /* short press */
                snprintf(axp_last, sizeof(axp_last), "last irq %02X %02X %02X", irq[0], irq[1], irq[2]);
                for (uint8_t r = 0; r < 3; r++) {
                    if (irq[r]) {
                        reg_write(axp, (uint8_t)(0x48 + r), &irq[r], 1);
                    }
                }
            }
            snprintf(s_axp, sizeof(s_axp), "%s\nPWR dn=%" PRIu32 " up=%" PRIu32 " short=%" PRIu32 " b2=%" PRIu32,
                     axp_last, pk_dn, pk_up, pk_short, pk_b2);
        }

        /* Re-scan the I2C bus every 5 s and report changes (0x3E/0x57 came and went) */
        if (now_ms - last_scan_ms >= 5000) {
            last_scan_ms = now_ms;
            uint8_t cur[128] = {0};
            scan_bus(bus, cur);
            bool changed = false;
            for (uint8_t a = 0x08; a < 0x78; a++) {
                if (cur[a] != found[a]) {
                    changed = true;
                    ESP_LOGW(TAG, "I2C 0x%02X %s at %" PRId64 " ms", a, cur[a] ? "APPEARED" : "DISAPPEARED", now_ms);
                    if (cur[a] && !ever[a] && !is_known_addr(a)) {
                        dump_unknown(bus, a);
                    }
                }
                if (cur[a]) {
                    ever[a] = 1;
                }
            }
            if (changed) {
                memcpy(found, cur, sizeof(found));
                format_scan(found, ever);
            }
        }

        /* IMU */
        if (imu) {
            uint8_t a[6];
            if (reg_read(imu, 0x35, a, sizeof(a)) == ESP_OK) {
                int16_t rx = (int16_t)((a[1] << 8) | a[0]);
                int16_t ry = (int16_t)((a[3] << 8) | a[2]);
                int16_t rz = (int16_t)((a[5] << 8) | a[4]);
                const float mg_per_lsb = 4000.0f / 32768.0f;
                float x = rx * mg_per_lsb, y = ry * mg_per_lsb, z = rz * mg_per_lsb;
                float mag = sqrtf(x * x + y * y + z * z);
                float dev = fabsf(mag - 1000.0f);
                if (dev > window_peak) {
                    window_peak = dev;
                }
                if (dev > peak_dev_mg) {
                    peak_dev_mg = dev;
                }
                if (dev > 600.0f && (now_ms - last_spike_ms) > 40) {
                    spikes++;
                    ESP_LOGI(TAG, "IMU spike #%" PRIu32 " dev=%.0f mg at %" PRId64 " ms",
                             spikes, dev, now_ms);
                    last_spike_ms = now_ms;
                }
                if (now_ms - window_start_ms >= 1000) {
                    last_window_peak = window_peak;
                    window_peak = 0;
                    window_start_ms = now_ms;
                }
                snprintf(s_imu, sizeof(s_imu), "IMU %.0f %.0f %.0f mg\npk1s=%.0f spikes=%" PRIu32,
                         x, y, z, last_window_peak, spikes);
            }
        }

        /* Slow poll once per second: RTC and PMIC */
        if (now_ms - last_slow_ms >= 1000) {
            last_slow_ms = now_ms;
            if (rtc) {
                rtc_poll(rtc);
            }
            if (axp) {
                uint8_t id = 0, pct = 0;
                reg_read(axp, 0x03, &id, 1);
                reg_read(axp, 0xA4, &pct, 1);
                ESP_LOGI(TAG, "AXP2101 reg0x03=0x%02X batt%%reg0xA4=%u", id, pct);
                snprintf(s_bat, sizeof(s_bat), "bat %u%% id %02X", pct, id);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ---------------------------------------------------------------- UI */

static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_date;
static lv_obj_t *s_lbl_scan;
static lv_obj_t *s_lbl_imu;
static lv_obj_t *s_lbl_btn;
static lv_obj_t *s_lbl_gpio;
static lv_obj_t *s_lbl_touch;
static lv_obj_t *s_lbl_axp;

static lv_obj_t *make_label(const lv_font_t *font, int y_offset, int width)
{
    lv_obj_t *l = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, width);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y_offset);
    lv_label_set_text(l, "");
    return l;
}

static void touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    snprintf(s_touch, sizeof(s_touch), "touch %d,%d", (int)p.x, (int)p.y);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSING, NULL);

    s_lbl_time  = make_label(&lv_font_montserrat_48, -120, 320);
    s_lbl_date  = make_label(&lv_font_montserrat_20,  -66, 320);
    s_lbl_scan  = make_label(&lv_font_montserrat_14,  -30, 300);
    s_lbl_imu   = make_label(&lv_font_montserrat_14,   20, 300);
    s_lbl_btn   = make_label(&lv_font_montserrat_14,   70, 300);
    s_lbl_gpio  = make_label(&lv_font_montserrat_14,  104, 300);
    s_lbl_touch = make_label(&lv_font_montserrat_14,  138, 300);
    s_lbl_axp   = make_label(&lv_font_montserrat_14,  172, 300);
}

static void update_ui(void)
{
    lv_label_set_text(s_lbl_time, s_time);
    char date_line[64];
    snprintf(date_line, sizeof(date_line), "%s  %s", s_date, s_bat);
    lv_label_set_text(s_lbl_date, date_line);
    lv_label_set_text(s_lbl_scan, s_scan);
    lv_label_set_text(s_lbl_imu, s_imu);
    lv_label_set_text(s_lbl_btn, s_btn);
    char gpio_text[176];
    snprintf(gpio_text, sizeof(gpio_text), "%s\n%s", s_gpio, s_low);
    lv_label_set_text(s_lbl_gpio, gpio_text);
    lv_label_set_text(s_lbl_touch, s_touch);
    lv_label_set_text(s_lbl_axp, s_axp);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Electric Pocket Watch bring-up v0.0.3 (built %s %s)", __DATE__, __TIME__);

    ESP_ERROR_CHECK(bsp_i2c_init());

    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }
    bsp_display_brightness_set(60);

    if (bsp_display_lock(1000) == ESP_OK) {
        build_ui();
        bsp_display_unlock();
    }

    xTaskCreate(monitor_task, "monitor", 6144, NULL, 5, NULL);

    while (1) {
        if (bsp_display_lock(100) == ESP_OK) {
            update_ui();
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
