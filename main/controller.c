#include "controller.h"

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "axp.h"
#include "battlog.h"
#include "buttons.h"
#include "faces.h"
#include "imu.h"
#include "net.h"
#include "settings.h"
#include "settings_ui.h"
#include "splash.h"

static const char *TAG = "ctl";

typedef enum { WAKE_NONE = 0, WAKE_BOOT, WAKE_PWR, WAKE_IMU } wake_src_t;

static bool s_screen_on = false;
static uint32_t s_awake_ms = CONFIG_WATCH_AWAKE_MS;
static bool s_lvgl_paused = false;
static int s_imu_idle_level = 0;     /* INT1 level after arming; a change from it = motion */

static void set_brightness(int pct);

/* ---------------------------------------------------------------- empty battery */

static void power_off_empty(int rest_mv)
{
    ESP_LOGW(TAG, "battery empty (%d mV at rest): powering off", rest_mv);
    battlog_sample("empty");
    if (s_lvgl_paused) {
        esp_lv_adapter_resume();
        s_lvgl_paused = false;
    }
    splash_message("Battery empty", "Charge to turn on");
    vTaskDelay(pdMS_TO_TICKS(3000));
    set_brightness(0);
    axp_power_off();
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* Still running: the PMIC ignored the command. Sleep until PWR rather than drain further. */
    ESP_LOGE(TAG, "PMIC power-off failed; deep sleep until PWR");
    esp_sleep_enable_ext0_wakeup(PIN_PWR, 1);
    esp_deep_sleep_start();
}

/* rest_mv: a screen-off reading, or a screen-on one corrected by AXP_SCREEN_SAG_MV. */
static void check_empty(int rest_mv)
{
    static int low;
    if (rest_mv <= 0 || rest_mv > BATTLOG_EMPTY_MV || axp_vbus_good()) {
        low = 0;
        return;
    }
    if (++low >= 2) {
        power_off_empty(rest_mv);
    }
}

void controller_check_empty_at_boot(void)
{
    for (int i = 0; i < 2; i++) {                    /* splash is lit: screen-on readings */
        int mv = axp_battery_mv();
        if (axp_vbus_good() || mv <= 0 || mv + AXP_SCREEN_SAG_MV > BATTLOG_EMPTY_MV) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    power_off_empty(axp_battery_mv() + AXP_SCREEN_SAG_MV);
}

/* ---------------------------------------------------------------- screen */

/* The brightness DCS command goes over the same QSPI link as the pixel data; send it while
 * holding the LVGL lock so it never lands in the middle of a render/flush cycle. */
static void set_brightness(int pct)
{
    bool locked = bsp_display_lock(1000) == ESP_OK;
    bsp_display_brightness_set(pct);
    if (locked) {
        bsp_display_unlock();
    }
}

static void touch_activity(void)
{
    if (bsp_display_lock(500) == ESP_OK) {
        lv_display_trigger_activity(NULL);
        bsp_display_unlock();
    }
}

static void screen_on(const char *why)
{
    static bool s_booted;
    if (s_booted) {
        axp_rest_update();                            /* display still dark: light-load reading */
    } else {
        axp_rest_set_loaded(axp_battery_mv());        /* boot: splash already lit */
        s_booted = true;
    }
    check_empty(axp_rest_mv());
    int pct = axp_rest_percent();
    ESP_LOGI(TAG, "battery %d mV, gauge %d%%, charge state %d", axp_battery_mv(), axp_gauge_percent(),
             axp_charge_state());
    ESP_LOGI(TAG, "screen ON (%s), battery %d%%, VBUS %d, internal DMA free %u (largest %u)", why, pct,
             axp_vbus_good(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    imu_off();
    if (s_lvgl_paused) {
        esp_lv_adapter_resume();                     /* restart the LVGL worker and its tick */
        s_lvgl_paused = false;
    }
    faces_set_active(true);                          /* draws the current time first */
    vTaskDelay(pdMS_TO_TICKS(60));                   /* let that frame reach the panel */
    set_brightness(settings_get_int("bright", CONFIG_WATCH_BRIGHTNESS));
    s_awake_ms = (uint32_t)settings_get_int("awake_s", CONFIG_WATCH_AWAKE_MS / 1000) * 1000;
    touch_activity();
    s_screen_on = true;
}

static void screen_off(const char *why)
{
    ESP_LOGI(TAG, "screen OFF (%s)", why);
    settings_ui_close();
    set_brightness(0);
    faces_set_active(false);
    /* Stop the LVGL worker (waits for any flush in progress) before light sleep, as the
     * esp_lvgl_adapter README prescribes for custom sleep flows. */
    if (esp_lv_adapter_pause(500) == ESP_OK) {
        s_lvgl_paused = true;
    } else {
        ESP_LOGW(TAG, "LVGL pause timed out");
    }
    s_screen_on = false;
}

/* ---------------------------------------------------------------- screen-on loop */

static void run_screen_on(void)
{
    uint32_t n = 0;
    while (s_screen_on) {
        switch (buttons_poll()) {
        case BTN_BOOT_SHORT:
            if (settings_ui_is_open()) {
                ESP_LOGI(TAG, "BOOT short: close settings");
                settings_ui_close();
            } else {
                ESP_LOGI(TAG, "BOOT short: next face");
                faces_next();
            }
            touch_activity();
            break;
        case BTN_BOOT_DOUBLE:
            ESP_LOGI(TAG, "BOOT double: settings");
            settings_ui_toggle();
            touch_activity();
            break;
        case BTN_BOOT_LONG:
        {
            ESP_LOGI(TAG, "BOOT long: Wi-Fi time sync");
            faces_show_status("Syncing time...", 0);
            net_result_t r = net_sync_time();
            ESP_LOGI(TAG, "time sync: %s", net_result_text(r));
            faces_show_status(net_result_text(r), 3000);
            buttons_sync_and_consume();
            touch_activity();
            break;
        }
        case BTN_PWR_SHORT:
            screen_off("PWR");
            return;
        case BTN_PWR_LONG:
            ESP_LOGI(TAG, "PWR long: the PMIC powers the board off if it stays held");
            break;
        default:
            break;
        }
        if (settings_ui_busy() || net_busy()) {
            touch_activity();                        /* keep the screen on during Wi-Fi work */
        }
        if ((++n % 20) == 0) {                       /* every ~200 ms */
            uint32_t idle = 0;
            if (bsp_display_lock(1000) == ESP_OK) {
                idle = lv_display_get_inactive_time(NULL);
                bsp_display_unlock();
            }
            if (battlog_due()) {
                battlog_sample("awake");             /* keeps logging with the screen always on */
                if (s_awake_ms == 0) {
                    axp_rest_set_loaded(axp_battery_mv());   /* always on: no dark reading exists */
                    check_empty(axp_rest_mv());
                }
            }
            uint32_t limit = settings_ui_is_open() ? 60000 : s_awake_ms;   /* longer while in Settings */
            if (s_awake_ms > 0 && idle >= limit) {
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
    if (gpio_get_level(PIN_IMU_INT1) != s_imu_idle_level) {
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

/* Settle, arm wake-on-motion, then record INT1's idle level. The v0.0.8 log showed INT1 at both
 * levels across WoM events, so it may toggle rather than pulse: the wake is armed on the level
 * opposite to the recorded one, which catches either behaviour. */
static void arm_for_sleep(void)
{
    wait_buttons_released(3000);
    if (!imu_wait_still(300, 2000, 150)) {
        ESP_LOGI(TAG, "not still after 2 s; arming wake-on-motion anyway");
    }
    imu_arm_wom(CONFIG_WATCH_WOM_THRESHOLD_MG);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_imu_idle_level = gpio_get_level(PIN_IMU_INT1);
    ESP_LOGI(TAG, "wake-on-motion armed, INT1 idle level %d", s_imu_idle_level);
}

/* Light sleep until one of BOOT (low), PWR (high) or IMU INT1 (high) fires. Per-pin wake levels
 * are supported by gpio_wakeup_enable() in light sleep (ESP-IDF sleep_modes docs), which is what
 * allows the mixed polarity. While USB power is present the watch only idles, so the serial log
 * survives, unless CONFIG_WATCH_SLEEP_ON_USB is set. */
static wake_src_t wait_for_wake(void)
{
#ifdef CONFIG_WATCH_SLEEP_ON_USB
    bool light = true;
#else
    bool light = !axp_vbus_good();
#endif
    if (net_busy()) {
        light = false;                               /* Wi-Fi is in use: idle instead of sleeping */
    }
    if (light) {
        gpio_wakeup_enable(PIN_BOOT, GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable(PIN_PWR, GPIO_INTR_HIGH_LEVEL);
        gpio_wakeup_enable(PIN_IMU_INT1, s_imu_idle_level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        esp_sleep_enable_timer_wakeup(battlog_us_until_due());    /* battery log sample */
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
    wake_src_t w = wake_pins();
    if (w == WAKE_NONE && battlog_due()) {
        battlog_sample(light ? "sleep" : "idle");
        check_empty(axp_rest_mv());
    }
    return w;
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
            battlog_note_wake(BATTLOG_WAKE_BUTTON);
            screen_on(w == WAKE_BOOT ? "BOOT" : "PWR");
            break;
        case WAKE_IMU:
            ESP_LOGI(TAG, "motion wake");
            if (imu_confirm_double_tap(CONFIG_WATCH_TAP_WINDOW_MS, CONFIG_WATCH_TAP_THRESHOLD_MG)) {
                buttons_sync_and_consume();
                battlog_note_wake(BATTLOG_WAKE_TAP);
                screen_on("double tap");
            } else {
                battlog_note_wake(BATTLOG_WAKE_MOTION_REJECTED);
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
