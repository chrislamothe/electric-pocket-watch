#include "controller.h"

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "bsp/esp-bsp.h"
#include "axp.h"
#include "buttons.h"
#include "faces.h"
#include "imu.h"

static const char *TAG = "ctl";

typedef enum { WAKE_NONE = 0, WAKE_BOOT, WAKE_PWR, WAKE_IMU } wake_src_t;

static bool s_screen_on = false;

/* ---------------------------------------------------------------- screen */

static void touch_activity(void)
{
    if (bsp_display_lock(200) == ESP_OK) {
        lv_display_trigger_activity(NULL);
        bsp_display_unlock();
    }
}

static void screen_on(const char *why)
{
    int pct = axp_battery_percent();
    ESP_LOGI(TAG, "screen ON (%s), battery %d%%, VBUS %d", why, pct, axp_vbus_good());
    imu_off();
    faces_set_active(true);                          /* draws the current time first */
    vTaskDelay(pdMS_TO_TICKS(60));                   /* let that frame reach the panel */
    bsp_display_brightness_set(CONFIG_WATCH_BRIGHTNESS);
    touch_activity();
    s_screen_on = true;
}

static void screen_off(const char *why)
{
    ESP_LOGI(TAG, "screen OFF (%s)", why);
    bsp_display_brightness_set(0);
    faces_set_active(false);
    s_screen_on = false;
}

/* ---------------------------------------------------------------- screen-on loop */

static void run_screen_on(void)
{
    uint32_t n = 0;
    while (s_screen_on) {
        switch (buttons_poll()) {
        case BTN_BOOT_SHORT:
            ESP_LOGI(TAG, "BOOT short: next face");
            faces_next();
            touch_activity();
            break;
        case BTN_BOOT_LONG:
            ESP_LOGI(TAG, "BOOT long: reserved for WiFi time sync");
            touch_activity();
            break;
        case BTN_PWR_SHORT:
            screen_off("PWR");
            return;
        case BTN_PWR_LONG:
            ESP_LOGI(TAG, "PWR long: the PMIC powers the board off if it stays held");
            break;
        default:
            break;
        }
        if ((++n % 20) == 0) {                       /* every ~200 ms */
            uint32_t idle = 0;
            if (bsp_display_lock(100) == ESP_OK) {
                idle = lv_display_get_inactive_time(NULL);
                bsp_display_unlock();
            }
            if (idle >= CONFIG_WATCH_AWAKE_MS) {
                screen_off("timeout");
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------------------------------------------------------------- screen-off cycle */

static wake_src_t wake_pins(void)
{
    if (buttons_boot_down_raw()) {
        return WAKE_BOOT;
    }
    if (buttons_pwr_down_raw()) {
        return WAKE_PWR;
    }
    if (gpio_get_level(PIN_IMU_INT1)) {
        return WAKE_IMU;
    }
    return WAKE_NONE;
}

static void wait_buttons_released(int max_ms)
{
    for (int t = 0; t < max_ms && (buttons_boot_down_raw() || buttons_pwr_down_raw()); t += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Settle, arm wake-on-motion, and make sure INT1 is low so the level wake does not fire at once. */
static void arm_for_sleep(void)
{
    wait_buttons_released(3000);
    if (!imu_wait_still(300, 2000, 150)) {
        ESP_LOGI(TAG, "not still after 2 s; arming wake-on-motion anyway");
    }
    imu_arm_wom(CONFIG_WATCH_WOM_THRESHOLD_MG);
    for (int i = 0; i < 20 && gpio_get_level(PIN_IMU_INT1); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* Light sleep until one of BOOT (low), PWR (high) or IMU INT1 (high) fires. Per-pin wake levels
 * are supported by gpio_wakeup_enable() in light sleep (ESP-IDF sleep_modes docs), which is what
 * allows the mixed polarity. While USB power is present the watch only idles, so the serial log
 * survives, unless CONFIG_WATCH_SLEEP_ON_USB is set. */
static wake_src_t wait_for_wake(void)
{
#ifdef CONFIG_WATCH_SLEEP_ON_USB
    const bool light = true;
#else
    const bool light = !axp_vbus_good();
#endif
    if (light) {
        gpio_wakeup_enable(PIN_BOOT, GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable(PIN_PWR, GPIO_INTR_HIGH_LEVEL);
        gpio_wakeup_enable(PIN_IMU_INT1, GPIO_INTR_HIGH_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        esp_err_t r = esp_light_sleep_start();
        gpio_wakeup_disable(PIN_BOOT);
        gpio_wakeup_disable(PIN_PWR);
        gpio_wakeup_disable(PIN_IMU_INT1);
        if (r != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));           /* rejected: a wake level was already active */
        }
    } else {
        for (int i = 0; i < 20 && wake_pins() == WAKE_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return wake_pins();
}

static void run_screen_off(void)
{
    arm_for_sleep();
    while (!s_screen_on) {
        wake_src_t w = wait_for_wake();
        switch (w) {
        case WAKE_BOOT:
        case WAKE_PWR:
            buttons_sync_and_consume();              /* this press only wakes the screen */
            screen_on(w == WAKE_BOOT ? "BOOT" : "PWR");
            break;
        case WAKE_IMU:
            ESP_LOGI(TAG, "motion wake");
            if (imu_confirm_double_tap(CONFIG_WATCH_TAP_WINDOW_MS, CONFIG_WATCH_TAP_THRESHOLD_MG)) {
                buttons_sync_and_consume();
                screen_on("double tap");
            } else {
                arm_for_sleep();
            }
            break;
        default:
            break;
        }
    }
}

void controller_run(void)
{
    screen_on("boot");
    while (1) {
        if (s_screen_on) {
            run_screen_on();
        } else {
            run_screen_off();
        }
    }
}
