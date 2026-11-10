/*
 * Electric Pocket Watch
 *
 * Board: Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 * CONFIG_WATCH_STAGE 1: faces only, screen always on, BOOT short = next face, serial heartbeat.
 * CONFIG_WATCH_STAGE 2: full controller (screen off until BOOT, PWR or a double tap).
 *
 * The bring-up/diagnostic firmware is commit 986ce41 (v0.0.8).
 */

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "axp.h"
#include "battlog.h"
#include "buttons.h"
#include "controller.h"
#include "faces.h"
#include "imu.h"
#include "improv.h"
#include "net.h"
#include "settings.h"
#include "settings_ui.h"
#include "splash.h"
#include "version.h"
#include "watch_time.h"

static const char *TAG = "pocketwatch";

/* The 1.75C has no RTC chip, so the clock is lost on every power-off. Try one sync shortly after
 * boot (saved network, or an open one if allowed); the result shows under Settings > Wi-Fi. */
static void boot_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    net_result_t r = net_sync_time();
    ESP_LOGI(TAG, "boot time sync: %s", net_result_text(r));
    vTaskDelete(NULL);
}

static void stage1_loop(void)
{
    bsp_display_brightness_set(CONFIG_WATCH_BRIGHTNESS);
    ESP_LOGI(TAG, "stage 1: screen on at %d%%", CONFIG_WATCH_BRIGHTNESS);
    uint32_t n = 0;
    while (1) {
        if (buttons_poll() == BTN_BOOT_SHORT) {
            ESP_LOGI(TAG, "BOOT short: next face");
            faces_next();
        }
        if ((++n % 200) == 0) {                 /* every ~2 s */
            struct tm t;
            watch_time_now(&t, NULL);
            ESP_LOGI(TAG, "alive %02d:%02d:%02d  heap internal %u  psram %u  battery %d%%  VBUS %d",
                     t.tm_hour, t.tm_min, t.tm_sec,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     axp_battery_percent(), axp_vbus_good());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    improv_start();                             /* USB serial driver first: console goes through it */
    ESP_LOGI(TAG, "%s %s stage %d (built %s %s)", WATCH_NAME, WATCH_VERSION, CONFIG_WATCH_STAGE,
             __DATE__, __TIME__);
    settings_init();
    net_init();
    settings_ui_init();

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_LOGI(TAG, "step: display start");
    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }
    splash_show();                              /* name, version, reset reason */

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_LOGI(TAG, "step: PMIC");
    if (!axp_init(bus)) {
        ESP_LOGE(TAG, "PMIC not available");
    } else {
        axp_configure_power_key();
        battlog_init();
        battlog_sample("boot");
#if CONFIG_WATCH_STAGE == 2
        controller_check_empty_at_boot();
#endif
    }
    ESP_LOGI(TAG, "step: IMU");
    if (!imu_init(bus)) {
        ESP_LOGE(TAG, "IMU not available: double-tap wake disabled");
    }

    watch_time_init();
    buttons_init();

    ESP_LOGI(TAG, "step: faces");
    if (bsp_display_lock(1000) == ESP_OK) {
        faces_init();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock timeout");
    }
    splash_hide();
    ESP_LOGI(TAG, "step: faces built, heap internal %u psram %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

#if CONFIG_WATCH_STAGE == 1
    stage1_loop();
#else
    xTaskCreate(boot_sync_task, "boot_sync", 6144, NULL, 3, NULL);
    controller_run();
#endif
}
