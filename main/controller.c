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
#include "net.h"
#include "settings.h"
#include "settings_ui.h"
#include "touch.h"
#include "clock.h"
#include "evlog.h"
#include "splash.h"
#include "lowpower.h"

static const char *TAG = "ctl";

typedef enum { WAKE_NONE = 0, WAKE_BOOT, WAKE_PWR } wake_src_t;

static bool s_screen_on = false;
static uint32_t s_awake_ms = CONFIG_WATCH_AWAKE_MS;
static bool s_lvgl_paused = false;

/* While charging (setting "chg_on", default on) the screen does not time out: after the normal
 * screen-on time it dims to CHARGE_DIM_PCT and stays on; a button press brings it back to full
 * brightness. Plugging in also turns the screen on. PWR still turns it off. */
#define CHARGE_DIM_PCT 20
static bool s_vbus;                  /* USB power, as last seen */
static bool s_chg_on = true;         /* setting, re-read periodically while the screen is on */
static bool s_dimmed;                /* dimmed by the charging rule */
static int s_bright = CONFIG_WATCH_BRIGHTNESS;

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
    /* Still running: the PMIC ignored the command. Sleep until a button rather than drain further. */
#if BOARD_PIN_PWR >= 0
    ESP_LOGE(TAG, "PMIC power-off failed; deep sleep until PWR");
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_PIN_PWR, 1);
#else
    ESP_LOGE(TAG, "PMIC power-off failed; deep sleep until BOOT");
    esp_sleep_enable_ext0_wakeup(PIN_BOOT, 0);
#endif
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
    /* After an empty-battery shutdown the cell recovers a little; booting on that just browns out
     * minutes later. Require BATTLOG_RESTART_MV then, BATTLOG_EMPTY_MV otherwise. */
    int need = battlog_last_run_ended_empty() ? BATTLOG_RESTART_MV : BATTLOG_EMPTY_MV;
    for (int i = 0; i < 2; i++) {                    /* splash is lit: screen-on readings */
        int mv = axp_battery_mv();
        if (axp_vbus_good() || mv <= 0 || mv + AXP_SCREEN_SAG_MV > need) {
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
    evlog_add("scr_on", why, pct, axp_vbus_good());
    ESP_LOGI(TAG, "screen ON (%s), battery %d%%, VBUS %d, internal DMA free %u (largest %u)", why, pct,
             axp_vbus_good(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    lowpower_off_end();
    lowpower_screen_on();                            /* panel out of deep standby, still dark */
    if (s_lvgl_paused) {
        esp_lv_adapter_resume();                     /* restart the LVGL worker and its tick */
        s_lvgl_paused = false;
    }
    clock_display_wake();                            /* anchor to the RTC before any time is drawn */
    faces_set_active(true);                          /* draws the current time first */
    lowpower_screen_show();                          /* full frame in panel memory, then DISPON */
    s_bright = settings_get_int("bright", CONFIG_WATCH_BRIGHTNESS);
    set_brightness(s_bright);
    s_dimmed = false;
    s_chg_on = settings_get_int("chg_on", 1) != 0;
    s_awake_ms = (uint32_t)settings_get_int("awake_s", CONFIG_WATCH_AWAKE_MS / 1000) * 1000;
    touch_activity();
    s_screen_on = true;
}

static void screen_off(const char *why)
{
    ESP_LOGI(TAG, "screen OFF (%s)", why);
    evlog_add("scr_off", why, (int32_t)s_awake_ms, 0);
    settings_ui_close();
    touch_set_active(false);
    touch_wait_settled(300);                         /* touch controller asleep before light sleep */
    set_brightness(0);
    faces_set_active(false);
    /* Stop the LVGL worker (waits for any flush in progress) before light sleep, as the
     * esp_lvgl_adapter README prescribes for custom sleep flows. */
    if (esp_lv_adapter_pause(500) == ESP_OK) {
        s_lvgl_paused = true;
    } else {
        ESP_LOGW(TAG, "LVGL pause timed out");
    }
    lowpower_screen_off();                           /* panel deep standby */
    lowpower_off_begin();
    s_screen_on = false;
}

/* ---------------------------------------------------------------- USB power */

/* Returns true when USB power has just been connected. */
static bool vbus_poll(void)
{
    bool usb = axp_vbus_good();
    if (usb == s_vbus) {
        return false;
    }
    s_vbus = usb;
    ESP_LOGI(TAG, "USB power %s", usb ? "connected" : "removed");
    if (!usb) {
        /* The last reading was the elevated charging voltage: replace it now. */
        if (s_screen_on && s_bright > 0) {
            axp_rest_set_loaded(axp_battery_mv());
        } else {
            axp_rest_update();
        }
    }
    if (s_screen_on) {
        faces_battery_refresh();
    }
    return usb;
}

static void restore_from_dim(void)
{
    if (s_dimmed) {
        set_brightness(s_bright);
        s_dimmed = false;
    }
}

/* ---------------------------------------------------------------- PWR hold = power off */

#define POWER_OFF_HOLD_MS 3000

/* Called at the long-press report (0.8 s). The board's own PMIC hold-to-power-off never worked on
 * this watch, so the firmware does it: keep holding to 3 s and the PMIC is told to power off
 * (0x10 bit0, the same path as the empty-battery shutdown). PWR or USB turns it on again. */
static void power_off_if_held(void)
{
    faces_show_status("Hold to power off", 0);
    int held = 800;
    while (buttons_pwr_down_raw() && held < POWER_OFF_HOLD_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        held += 20;
    }
    if (held < POWER_OFF_HOLD_MS) {
        faces_show_status("", 1);                    /* released early: cancel */
        buttons_sync_and_consume();
        touch_activity();
        return;
    }
    ESP_LOGW(TAG, "PWR held %d ms: powering off", POWER_OFF_HOLD_MS);
    faces_show_status("Powering off", 0);
    battlog_sample("off");
    vTaskDelay(pdMS_TO_TICKS(1000));
    set_brightness(0);
    axp_power_off();
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGE(TAG, "PMIC power-off failed");
    faces_show_status("Power-off failed", 3000);
    set_brightness(s_bright);
    buttons_sync_and_consume();
}

/* ---------------------------------------------------------------- screen-on loop */

static void run_screen_on(void)
{
    uint32_t n = 0;
    while (s_screen_on) {
        switch (buttons_poll()) {
        case BTN_BOOT_SHORT:
            if (s_dimmed) {
                ESP_LOGI(TAG, "BOOT short: undim");      /* first press only brightens */
                restore_from_dim();
            } else if (settings_ui_is_open()) {
                settings_ui_next();                      /* move the highlight */
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
            restore_from_dim();
            if (settings_ui_is_open()) {
                settings_ui_select();                    /* activate the highlighted item */
                buttons_sync_and_consume();
                touch_activity();
                break;
            }
            ESP_LOGI(TAG, "BOOT long: Wi-Fi time sync");
            evlog_add("sync", "user", 0, 0);
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
            power_off_if_held();
            break;
        default:
            break;
        }
        if (settings_ui_busy()) {
            touch_activity();                        /* keep the screen on during a Settings job */
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
            if (((n / 20) % 25) == 0) {
                s_chg_on = settings_get_int("chg_on", 1) != 0;   /* every ~5 s: follows the settings */
                if (!s_dimmed) {
                    s_bright = settings_get_int("bright", CONFIG_WATCH_BRIGHTNESS);
                }
            }
            bool was_usb = s_vbus;
            vbus_poll();
            if (s_vbus != was_usb) {
                evlog_add("usb", s_vbus ? "in" : "out", axp_battery_mv(), 0);
                touch_activity();                    /* plugged or unplugged: restart the timer */
                restore_from_dim();
                idle = 0;
            }
            uint32_t limit = settings_ui_is_open() ? 60000 : s_awake_ms;   /* longer while in Settings */
            bool timed_out = s_awake_ms > 0 && idle >= limit;
            if (s_vbus && s_chg_on) {
                if (timed_out && !s_dimmed) {
                    ESP_LOGI(TAG, "charging: screen dimmed");
                    set_brightness(s_bright < CHARGE_DIM_PCT ? s_bright : CHARGE_DIM_PCT);
                    s_dimmed = true;
                } else if (!timed_out && s_dimmed) {
                    restore_from_dim();              /* button pressed */
                }
            } else if (timed_out) {
                evlog_add("idle", settings_ui_is_open() ? "settings" : "face", (int32_t)idle, (int32_t)limit);
                restore_from_dim();
                screen_off("timeout");
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------------------------------------------------------------- screen-off cycle */

/* A button that reads pressed for 3 s when the screen goes off is treated as stuck (or leaning on
 * something in a pocket): it is ignored as a wake source until it reads released again, instead of
 * waking the screen over and over. */
static bool s_ignore_boot, s_ignore_pwr;
static uint32_t s_false_wakes;
static bool s_pwr_latched;           /* PMIC saw a PWR press since the last poll */
static bool s_pwr_latch_ok;          /* latch confirmed at boot (1.75 / 1.75-B) */

void controller_set_pwr_latch(bool ok)
{
    s_pwr_latch_ok = ok;
    if (!ok) {
        ESP_LOGW(TAG, "PWR latch unavailable: polling PWR every 200 ms");
    }
}

static wake_src_t wake_pins(void)
{
    bool boot = buttons_boot_down_raw(), pwr = buttons_pwr_down_raw();
    if (s_ignore_boot && !boot) {
        s_ignore_boot = false;
        ESP_LOGI(TAG, "BOOT released: wakes the screen again");
    }
    if (s_ignore_pwr && !pwr) {
        s_ignore_pwr = false;
        ESP_LOGI(TAG, "PWR released: wakes the screen again");
    }
    if (boot && !s_ignore_boot) {
        return WAKE_BOOT;
    }
    if (pwr && !s_ignore_pwr) {
        return WAKE_PWR;
    }
    return WAKE_NONE;
}

/* A press must hold for 50 ms to count: a glitch on a button pin does not light the screen. */
static bool wake_confirmed(wake_src_t w)
{
    if (w == WAKE_PWR && s_pwr_latched) {
        /* Pressed and already released: the level can no longer be checked. Whether contact bounce
         * or noise alone can set the latch is not verified on hardware; false PWR wakes show in
         * the wake counts on Settings > System. */
        s_pwr_latched = false;
        return true;
    }
    for (int i = 0; i < 5; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (wake_pins() != w) {
            s_false_wakes++;
            evlog_add("glitch", w == WAKE_BOOT ? "BOOT" : "PWR", (int32_t)s_false_wakes, 0);
            ESP_LOGW(TAG, "%s glitch ignored (%lu so far)", w == WAKE_BOOT ? "BOOT" : "PWR",
                     (unsigned long)s_false_wakes);
            return false;
        }
    }
    return true;
}

static void wait_buttons_released(int max_ms)
{
    for (int t = 0; t < max_ms && (buttons_boot_down_raw() || buttons_pwr_down_raw()); t += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Light sleep until BOOT (low) or PWR (high) is pressed. Per-pin wake levels
 * are supported by gpio_wakeup_enable() in light sleep (ESP-IDF sleep_modes docs), which is what
 * allows the mixed polarity. Where PWR is not on a GPIO (1.75 / 1.75-B: I/O expander without a
 * routed interrupt), the timer wakes the chip every BOARD_PWR_POLL_MS to read it. While USB power
 * is present the watch only idles, so the serial log survives, unless CONFIG_WATCH_SLEEP_ON_USB
 * is set. */
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
    if (light && !clock_sleep_prepare()) {
        light = false;                               /* RTC or network time transaction running */
    }
    if (light) {
        if (!s_ignore_boot) {
            gpio_wakeup_enable(PIN_BOOT, GPIO_INTR_LOW_LEVEL);
        }
#if BOARD_PIN_PWR >= 0
        if (!s_ignore_pwr) {
            gpio_wakeup_enable((gpio_num_t)BOARD_PIN_PWR, GPIO_INTR_HIGH_LEVEL);
        }
#endif
        esp_sleep_enable_gpio_wakeup();
        uint64_t wake_us = battlog_us_until_due();              /* battery log sample */
        uint64_t time_us = clock_us_until_due();                /* clock work (RTC refine, auto sync) */
        if (time_us < wake_us) {
            wake_us = time_us < 1000 ? 1000 : time_us;
        }
        if ((s_ignore_boot || s_ignore_pwr) && wake_us > 1000000) {
            wake_us = 1000000;                                  /* look for the stuck button's release */
        }
#if BOARD_PWR_POLL_MS > 0
        const uint64_t poll_us = (uint64_t)(s_pwr_latch_ok ? BOARD_PWR_POLL_MS : 200) * 1000;
        if (wake_us > poll_us) {
            wake_us = poll_us;                                   /* read PWR (not a wake pin) */
        }
#endif
        esp_sleep_enable_timer_wakeup(wake_us);
        lowpower_sleep_begin();
        esp_err_t r = esp_light_sleep_start();
        clock_sleep_resume(r == ESP_OK);             /* sleep-clock error removed from the bridge */
        lowpower_sleep_end(r == ESP_OK);
        gpio_wakeup_disable(PIN_BOOT);
#if BOARD_PIN_PWR >= 0
        gpio_wakeup_disable((gpio_num_t)BOARD_PIN_PWR);
#endif
        if (r != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));           /* rejected: a wake level was already active */
        }
    } else {
        for (int i = 0; i < 20 && wake_pins() == WAKE_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    wake_src_t w = wake_pins();
#if BOARD_PWR_POLL_MS > 0
    if (w == WAKE_NONE && s_pwr_latch_ok && axp_pkey_pressed_latched()) {
        s_pwr_latched = true;                        /* pressed and released between polls */
        w = WAKE_PWR;
    }
#endif
    if (w == WAKE_NONE && battlog_due()) {
        battlog_sample(light ? "sleep" : "idle");
        check_empty(axp_rest_mv());
    }
    return w;
}

static void run_screen_off(void)
{
    wait_buttons_released(3000);                     /* a held button would reject every sleep */
    if (buttons_boot_down_raw()) {
        s_ignore_boot = true;
        evlog_add("stuck", "BOOT", 0, 0);
        ESP_LOGW(TAG, "BOOT reads pressed 3 s after screen off: ignored until released");
    }
    if (buttons_pwr_down_raw()) {
        s_ignore_pwr = true;
        evlog_add("stuck", "PWR", 0, 0);
        ESP_LOGW(TAG, "PWR reads pressed 3 s after screen off: ignored until released");
    }
#if BOARD_PWR_POLL_MS > 0
    vTaskDelay(pdMS_TO_TICKS(50));                   /* let release bounce settle first */
    axp_pkey_pressed_latched();                      /* drop presses made while the screen was on */
    s_pwr_latched = false;
#endif
    while (!s_screen_on) {
        wake_src_t w = wait_for_wake();
        if (vbus_poll() && settings_get_int("chg_on", 1)) {
            buttons_sync_and_consume();
            battlog_note_wake(BATTLOG_WAKE_USB);
            screen_on("USB connected");
            break;
        }
        if (w != WAKE_NONE && wake_confirmed(w)) {
            buttons_sync_and_consume();              /* this press only wakes the screen */
            battlog_note_wake(w == WAKE_BOOT ? BATTLOG_WAKE_BOOT : BATTLOG_WAKE_PWR);
            screen_on(w == WAKE_BOOT ? "BOOT" : "PWR");
            s_ignore_boot = s_ignore_pwr = false;
        }
    }
}

void controller_run(void)
{
    s_vbus = axp_vbus_good();
    screen_on("boot");
    while (1) {
        if (s_screen_on) {
            run_screen_on();
        } else {
            run_screen_off();
        }
    }
}
