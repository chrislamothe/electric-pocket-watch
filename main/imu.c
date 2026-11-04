#include "imu.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2c_dev.h"

static const char *TAG = "imu";

#define IMU_ADDR        0x6B
#define REG_WHO_AM_I    0x00
#define REG_CTRL1       0x02
#define REG_CTRL2       0x03
#define REG_CTRL7       0x08
#define REG_CTRL8       0x09
#define REG_CTRL9       0x0A
#define REG_CAL1_L      0x0B
#define REG_CAL1_H      0x0C
#define REG_STATUSINT   0x2D
#define REG_RESET       0x60
#define REG_ACCEL_X_L   0x35

#define CTRL1_BASE      0x40    /* bit6: register address auto-increment */
#define CTRL1_INT1_EN   0x08    /* bit3: INT1 output enable */
#define CTRL2_250HZ_4G  0x15    /* +/-4 g, 250 Hz normal mode */
#define CTRL2_LP_128HZ  0x1C    /* +/-4 g, low-power 128 Hz (used by wake-on-motion) */
#define WOM_BLANKING    0x20    /* CAL1_H: INT1, initial level low, 32 blanking samples */
#define MG_PER_LSB      (4000.0f / 32768.0f)

static i2c_master_dev_handle_t s_imu;

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool ctrl9(uint8_t cmd)
{
    uint8_t st = 0;
    if (i2c_reg_write(s_imu, REG_CTRL9, cmd) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < 100; i++) {             /* STATUSINT bit7 set = command done */
        if (i2c_reg_read(s_imu, REG_STATUSINT, &st, 1) == ESP_OK && (st & 0x80)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    bool done = (st & 0x80) != 0;
    i2c_reg_write(s_imu, REG_CTRL9, 0x00);      /* ACK; never leave a command pending */
    for (int i = 0; i < 100 && done; i++) {     /* STATUSINT bit7 clears after the ACK */
        if (i2c_reg_read(s_imu, REG_STATUSINT, &st, 1) == ESP_OK && !(st & 0x80)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static void clear_status(void)
{
    uint8_t st[3];                              /* STATUSINT, STATUS0, STATUS1 (0x2D..0x2F) */
    i2c_reg_read(s_imu, REG_STATUSINT, st, sizeof(st));
}

static bool read_dev_mg(float *dev_mg)
{
    uint8_t a[6];
    if (i2c_reg_read(s_imu, REG_ACCEL_X_L, a, sizeof(a)) != ESP_OK) {
        return false;
    }
    float x = (int16_t)((a[1] << 8) | a[0]) * MG_PER_LSB;
    float y = (int16_t)((a[3] << 8) | a[2]) * MG_PER_LSB;
    float z = (int16_t)((a[5] << 8) | a[4]) * MG_PER_LSB;
    *dev_mg = fabsf(sqrtf(x * x + y * y + z * z) - 1000.0f);
    return true;
}

static void start_normal_mode(void)
{
    i2c_reg_write(s_imu, REG_CTRL7, 0x00);
    i2c_reg_write(s_imu, REG_CTRL1, CTRL1_BASE);            /* INT1 output off */
    i2c_reg_write(s_imu, REG_CTRL2, CTRL2_250HZ_4G);
    i2c_reg_write(s_imu, REG_CTRL7, 0x01);                  /* accelerometer on */
    vTaskDelay(pdMS_TO_TICKS(15));                          /* first samples after enable */
}

bool imu_init(i2c_master_bus_handle_t bus)
{
    s_imu = i2c_dev_add(bus, IMU_ADDR);
    if (!s_imu) {
        return false;
    }
    uint8_t who = 0;
    if (i2c_reg_read(s_imu, REG_WHO_AM_I, &who, 1) != ESP_OK || who != 0x05) {
        ESP_LOGE(TAG, "QMI8658 WHO_AM_I = 0x%02X (expect 0x05)", who);
        return false;
    }
    i2c_reg_write(s_imu, REG_RESET, 0xB0);                  /* it keeps power across ESP32 resets */
    vTaskDelay(pdMS_TO_TICKS(30));
    i2c_reg_write(s_imu, REG_CTRL8, 0x80);                  /* CTRL9 handshake via STATUSINT bit7 */
    i2c_reg_write(s_imu, REG_CTRL1, CTRL1_BASE);
    imu_off();

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_IMU_INT1,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,               /* INT1 is idle-low push-pull */
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "QMI8658 ready");
    return true;
}

void imu_off(void)
{
    if (!s_imu) {
        return;
    }
    i2c_reg_write(s_imu, REG_CTRL7, 0x00);
}

bool imu_arm_wom(uint8_t threshold_mg)
{
    if (!s_imu) {
        return false;
    }
    i2c_reg_write(s_imu, REG_CTRL7, 0x00);
    i2c_reg_write(s_imu, REG_CTRL1, CTRL1_BASE | CTRL1_INT1_EN);
    i2c_reg_write(s_imu, REG_CTRL2, CTRL2_LP_128HZ);
    i2c_reg_write(s_imu, REG_CAL1_L, threshold_mg);         /* threshold, 1 mg/LSB */
    i2c_reg_write(s_imu, REG_CAL1_H, WOM_BLANKING);
    bool ok = ctrl9(0x08);                                  /* CTRL_CMD_WRITE_WOM_SETTING */
    i2c_reg_write(s_imu, REG_CTRL7, 0x01);
    clear_status();
    if (!ok) {
        ESP_LOGW(TAG, "WoM CTRL9 handshake failed");
    }
    return ok;
}

bool imu_wait_still(int still_ms, int max_ms, int quiet_mg)
{
    if (!s_imu) {
        return false;
    }
    start_normal_mode();
    int64_t t0 = now_ms();
    int64_t still_since = t0;
    while (now_ms() - t0 < max_ms) {
        float dev;
        if (read_dev_mg(&dev)) {
            if (dev > quiet_mg) {
                still_since = now_ms();
            } else if (now_ms() - still_since >= still_ms) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    return false;
}

bool imu_confirm_double_tap(int window_ms, int tap_threshold_mg)
{
    if (!s_imu) {
        return false;
    }
    /* The WoM interrupt is tap 1 (t0). Tap 2 must be a separate burst of motion: it starts at
     * least TAP_MIN_GAP_MS after t0, is preceded by TAP_QUIET_MS without activity, and peaks
     * above tap_threshold_mg. "Activity" is anything above a third of that threshold. */
    const int TAP_MIN_GAP_MS = 100;
    const int TAP_QUIET_MS = 50;
    const int BURST_END_MS = 20;
    const float lo = tap_threshold_mg / 3.0f;

    start_normal_mode();
    int64_t t0 = now_ms();
    int64_t last_active = t0;
    int64_t burst_start = 0;
    bool in_burst = false, after_quiet = false;
    float burst_peak = 0, peak_all = 0;
    int bursts = 0;
    bool confirmed = false;

    while (now_ms() - t0 < window_ms && !confirmed) {
        float dev;
        int64_t t = now_ms();
        if (read_dev_mg(&dev)) {
            if (dev > peak_all) {
                peak_all = dev;
            }
            if (dev > lo) {
                if (!in_burst) {
                    in_burst = true;
                    burst_start = t;
                    burst_peak = 0;
                    after_quiet = (t - last_active) >= TAP_QUIET_MS;
                    bursts++;
                }
                if (dev > burst_peak) {
                    burst_peak = dev;
                }
                last_active = t;
            } else if (in_burst && (t - last_active) >= BURST_END_MS) {
                in_burst = false;
                if (after_quiet && burst_peak >= tap_threshold_mg && (burst_start - t0) >= TAP_MIN_GAP_MS) {
                    confirmed = true;
                    ESP_LOGI(TAG, "tap 2 at +%d ms, peak %.0f mg", (int)(burst_start - t0), burst_peak);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    ESP_LOGI(TAG, "double-tap check: %s (bursts %d, peak %.0f mg, threshold %d mg)",
             confirmed ? "CONFIRMED" : "rejected", bursts, peak_all, tap_threshold_mg);
    return confirmed;
}
