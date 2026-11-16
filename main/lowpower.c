#include "lowpower.h"

#include <stdbool.h>
#include <stdio.h>
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "display_dma.h"

static const char *TAG = "lowpower";

/* totals since boot, and the current screen-off period */
static int64_t s_off_start_us, s_sleep_start_us;
static int64_t s_off_us, s_sleep_us;                 /* current period */
static int64_t s_tot_off_us, s_tot_sleep_us;
static uint32_t s_sleeps, s_rejected, s_tot_sleeps, s_tot_rejected;

static void panel_on_off(bool on)
{
    esp_lcd_panel_handle_t panel = display_dma_panel();
    if (!panel) {
        return;
    }
    /* Panel commands share the QSPI link with pixel data: send them under the display lock. */
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "display lock timeout: panel %s skipped", on ? "on" : "off");
        return;
    }
    esp_err_t e = esp_lcd_panel_disp_on_off(panel, on);
    bsp_display_unlock();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "panel %s failed: %s", on ? "on" : "off", esp_err_to_name(e));
    }
}

void lowpower_screen_off(void)
{
    panel_on_off(false);
}

void lowpower_screen_on(void)
{
    panel_on_off(true);
}

void lowpower_off_begin(void)
{
    s_off_start_us = esp_timer_get_time();
    s_off_us = s_sleep_us = 0;
    s_sleeps = s_rejected = 0;
}

void lowpower_sleep_begin(void)
{
    s_sleep_start_us = esp_timer_get_time();
}

void lowpower_sleep_end(bool slept)
{
    if (slept) {
        s_sleep_us += esp_timer_get_time() - s_sleep_start_us;
        s_sleeps++;
    } else {
        s_rejected++;
    }
}

void lowpower_off_end(void)
{
    if (s_off_start_us == 0) {
        return;
    }
    s_off_us = esp_timer_get_time() - s_off_start_us;
    s_off_start_us = 0;
    s_tot_off_us += s_off_us;
    s_tot_sleep_us += s_sleep_us;
    s_tot_sleeps += s_sleeps;
    s_tot_rejected += s_rejected;
    ESP_LOGI(TAG, "screen off %.1f s: asleep %.1f%% (%lu sleeps, %lu rejected)", s_off_us / 1e6,
             s_off_us > 0 ? 100.0 * s_sleep_us / s_off_us : 0.0, (unsigned long)s_sleeps,
             (unsigned long)s_rejected);
}

void lowpower_summary(char *buf, size_t len)
{
    if (s_tot_off_us <= 0) {
        snprintf(buf, len, "Light sleep: no screen-off time yet");
        return;
    }
    snprintf(buf, len, "Light sleep: %.1f%% of %.1f h off\n%lu sleeps, %lu rejected",
             100.0 * s_tot_sleep_us / s_tot_off_us, s_tot_off_us / 3.6e9, (unsigned long)s_tot_sleeps,
             (unsigned long)s_tot_rejected);
}
