/*
 * Electric Pocket Watch - bring-up firmware v0.0.1
 *
 * Purpose: verify the real wiring of the Waveshare ESP32-S3-Touch-AMOLED-1.75C
 * before the watch firmware is written. Shows on screen and logs over USB serial:
 *   - I2C bus scan (which devices answer, at which addresses)
 *   - PCF85063 RTC time (set to the firmware build time if the RTC is invalid)
 *   - QMI8658 IMU: WHO_AM_I, live acceleration, shake/tap spike detector
 *   - BOOT button (GPIO0) presses
 *   - Candidate IMU interrupt pin (GPIO21) edge counter
 *   - TCA9554 I/O expander input port, if present (PWR button / interrupt wiring)
 *   - AXP2101 raw ID and battery percentage registers (unverified register map)
 *   - Touch coordinates
 *
 * Register maps for QMI8658 and PCF85063 are from the vendor datasheets;
 * the AXP2101 and TCA9554 reads are best-effort and only logged raw.
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

#define ADDR_TCA9554      0x20
#define ADDR_AXP2101      0x34
#define ADDR_RTC          0x51
#define ADDR_IMU_A        0x6B
#define ADDR_IMU_B        0x6A

#define I2C_TIMEOUT_MS    100

/* Text shared between the monitor task and the UI loop. Tearing is harmless here. */
static char s_time[24]  = "--:--:--";
static char s_date[24]  = "";
static char s_bat[32]   = "";
static char s_scan[160] = "I2C scanning...";
static char s_imu[96]   = "IMU: ...";
static char s_btn[96]   = "BOOT: ...";
static char s_exio[64]  = "";
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

/* ---------------------------------------------------------------- RTC */

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

/* ---------------------------------------------------------------- monitor task */

static void monitor_task(void *arg)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    uint8_t found[128] = {0};

    /* --- I2C scan --- */
    size_t o = 0;
    o += snprintf(s_scan + o, sizeof(s_scan) - o, "I2C:");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            found[a] = 1;
            ESP_LOGI(TAG, "I2C device at 0x%02X", a);
            if (o < sizeof(s_scan) - 4) {
                o += snprintf(s_scan + o, sizeof(s_scan) - o, " %02X", a);
            }
        }
    }

    i2c_master_dev_handle_t rtc  = found[ADDR_RTC]     ? add_dev(bus, ADDR_RTC)     : NULL;
    i2c_master_dev_handle_t axp  = found[ADDR_AXP2101] ? add_dev(bus, ADDR_AXP2101) : NULL;
    i2c_master_dev_handle_t tca  = found[ADDR_TCA9554] ? add_dev(bus, ADDR_TCA9554) : NULL;
    uint8_t imu_addr = found[ADDR_IMU_A] ? ADDR_IMU_A : (found[ADDR_IMU_B] ? ADDR_IMU_B : 0);
    i2c_master_dev_handle_t imu  = imu_addr ? add_dev(bus, imu_addr) : NULL;

    ESP_LOGI(TAG, "present: rtc=%d axp2101=%d tca9554=%d imu=0x%02X",
             rtc != NULL, axp != NULL, tca != NULL, imu_addr);

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

    int boot_prev = gpio_get_level(PIN_BOOT);
    uint32_t boot_presses = 0;
    uint8_t exio_prev = 0xFF;
    bool exio_valid = false;

    float peak_dev_mg = 0;
    uint32_t spikes = 0;
    int64_t last_spike_ms = 0;
    int64_t window_start_ms = esp_timer_get_time() / 1000;
    float window_peak = 0;
    float last_window_peak = 0;
    int64_t last_slow_ms = 0;

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

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

        /* TCA9554 input port (PWR button / interrupt lines per the standard board) */
        if (tca) {
            uint8_t in = 0;
            if (reg_read(tca, 0x00, &in, 1) == ESP_OK) {
                if (!exio_valid || in != exio_prev) {
                    ESP_LOGI(TAG, "TCA9554 input port = 0x%02X (P4 = %d)", in, (in >> 4) & 1);
                    exio_prev = in;
                    exio_valid = true;
                }
                snprintf(s_exio, sizeof(s_exio), "EXIO=%02X P4=%d", in, (in >> 4) & 1);
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
static lv_obj_t *s_lbl_exio;
static lv_obj_t *s_lbl_touch;

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
    s_lbl_exio  = make_label(&lv_font_montserrat_14,  100, 300);
    s_lbl_touch = make_label(&lv_font_montserrat_14,  130, 300);
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
    lv_label_set_text(s_lbl_exio, s_exio);
    lv_label_set_text(s_lbl_touch, s_touch);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Electric Pocket Watch bring-up v0.0.1 (built %s %s)", __DATE__, __TIME__);

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
