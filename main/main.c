/*
 * Electric Pocket Watch
 *
 * Boards: Waveshare ESP32-S3-Touch-AMOLED-1.75C, -1.75 / -1.75-B, -2.06 (boards/, selected at
 * build time with -D BOARD=...). The screen stays off until BOOT or PWR is pressed; the watch is
 * operated with those two buttons only (no touch, accelerometer or audio: main/parked.c).
 *
 * The bring-up/diagnostic firmware is commit 986ce41 (v0.0.8).
 */

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "board.h"
#include "axp.h"
#include "battlog.h"
#include "buttons.h"
#include "controller.h"
#include "display.h"
#include "display_dma.h"
#include "lowpower.h"
#include "faces.h"
#include "parked.h"
#include "touch.h"
#include "improv.h"
#include "net.h"
#include "settings.h"
#include "settings_ui.h"
#include "splash.h"
#include "version.h"
#include "clock.h"
#include "clocklab.h"

static const char *TAG = "pocketwatch";

/* Try one sync shortly after boot (saved network, or an open one if allowed); the result shows
 * under Settings > Wi-Fi. Without an RTC chip (1.75C) this is what sets the clock after a
 * power-off; with one it corrects the RTC. Later syncs are scheduled by clock.c. */
static void boot_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    net_result_t r = net_sync_time();
    ESP_LOGI(TAG, "boot time sync: %s", net_result_text(r));
    vTaskDelete(NULL);
}

void app_main(void)
{
    improv_start();                             /* USB serial driver first: console goes through it */
    ESP_LOGI(TAG, "%s %s for %s (built %s %s)", WATCH_NAME, WATCH_VERSION, BOARD_NAME, __DATE__, __TIME__);
    settings_init();
    net_init();
    settings_ui_init();

    ESP_ERROR_CHECK(bsp_i2c_init());
#if CONFIG_WATCH_CLOCK_LAB
    clocklab_run(bsp_i2c_get_handle());         /* lab firmware: never returns, no display */
#endif
    ESP_LOGI(TAG, "step: display start");
    lv_display_t *disp = display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }
    display_dma_init(disp);                     /* before anything else takes internal RAM */
    splash_show();                              /* name, version, reset reason */

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    board_init(bus);
    parked_init(bus);                           /* accelerometer, audio: off for good */
    touch_init(bus);                            /* asleep except in the Settings menu */
    clock_init(bus);                            /* time source decided before the first log record */
    ESP_LOGI(TAG, "step: PMIC");
    if (!axp_init(bus)) {
        ESP_LOGE(TAG, "PMIC not available");
    } else {
        axp_configure_power_key();
#if BOARD_PWR_POLL_MS > 0
        controller_set_pwr_latch(axp_pkey_latch_enable());   /* PWR not on a GPIO: PMIC latches presses */
#endif
        battlog_init();
        battlog_sample("boot");
        controller_check_empty_at_boot();
    }
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

    xTaskCreate(boot_sync_task, "boot_sync", 6144, NULL, 3, NULL);
    controller_run();
}
