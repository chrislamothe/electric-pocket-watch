#include "lowpower.h"

#include <stdbool.h>
#include <stdio.h>
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "display.h"
#include "touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "board.h"

static const char *TAG = "lowpower";

/* totals since boot, and the current screen-off period */
static int64_t s_off_start_us, s_sleep_start_us;
static int64_t s_off_us, s_sleep_us;                 /* current period */
static int64_t s_tot_off_us, s_tot_sleep_us;
static uint32_t s_sleeps, s_rejected, s_tot_sleeps, s_tot_rejected;

/* ---------------------------------------------------------------- panel deep standby */

/* CO5300 over QSPI: command byte in bits 15..8 of a 32-bit word, opcode 0x02 = write. */
static esp_err_t panel_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_lcd_panel_io_handle_t io = display_panel_io();
    if (!io) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_io_tx_param(io, (0x02 << 24) | ((uint32_t)cmd << 8), data, len);
}

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[4];
    uint16_t delay_ms;
} panel_init_t;

/*
 * Register set-up after a hardware reset: the board package's own sequence (Waveshare BSP for
 * the 1.75 / 1.75C, main bsp_206.c for the 2.06) with three changes, so the wake takes ~0.2 s
 * instead of ~1.4 s and never shows stale or random panel memory:
 *   - brightness (0x51) 0 instead of 0xFF; the controller raises it after the first frame
 *   - no DISPON (0x29) here; lowpower_screen_show() sends it once a full frame is in panel memory
 *   - 120 ms after SLPOUT (the CO5300 driver's own default is 60 ms) instead of 2 x 600 ms
 * MADCTL and COLMOD are written explicitly (normally sent by the panel driver's init).
 */
static const panel_init_t s_init[] = {
#if defined(BOARD_PANEL_410x502)
    {0x11, 0, {0}, 120},                             /* SLPOUT */
    {0xC4, 1, {0x80}, 0},
    {0x44, 2, {0x01, 0xD1}, 0},
    {0x35, 1, {0x00}, 0},
    {0x53, 1, {0x20}, 0},
    {0x63, 1, {0xFF}, 0},
    {0x51, 1, {0x00}, 0},
    {0x36, 1, {0x00}, 0},
    {0x3A, 1, {0x55}, 0},
    {0x2A, 4, {0x00, 0x16, 0x01, 0xAF}, 0},
    {0x2B, 4, {0x00, 0x00, 0x01, 0xF5}, 0},
#else
    {0xFE, 1, {0x20}, 0},
    {0x19, 1, {0x10}, 0},
    {0x1C, 1, {0xA0}, 0},
    {0xFE, 1, {0x00}, 0},
    {0xC4, 1, {0x80}, 0},
    {0x36, 1, {0x00}, 0},
    {0x3A, 1, {0x55}, 0},
    {0x35, 1, {0x00}, 0},
    {0x53, 1, {0x20}, 0},
    {0x51, 1, {0x00}, 0},
    {0x63, 1, {0xFF}, 0},
    {0x2A, 4, {0x00, 0x06, 0x01, 0xD7}, 0},
    {0x2B, 4, {0x00, 0x00, 0x01, 0xD1}, 0},
    {0x11, 0, {0}, 120},                             /* SLPOUT */
#endif
};

static bool s_standby;              /* panel in deep standby: memory lost, needs reset + init */
static int64_t s_wake_us;

static uint32_t s_standby_failures, s_wake_failures;

static esp_err_t enter_standby(void)
{
    const uint8_t dstb = 0x01;
    esp_err_t e = panel_cmd(0x28, NULL, 0);                  /* DISPOFF */
    if (e == ESP_OK) {
        e = panel_cmd(0x10, NULL, 0);                        /* SLPIN */
    }
    if (e == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(120));                      /* SLPIN settle before DSTB */
        e = panel_cmd(0x4F, &dstb, 1);                       /* deep standby */
    }
    return e;
}

static esp_err_t reset_and_init(void)
{
    gpio_set_level(BSP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BSP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    esp_err_t e = ESP_OK;
    for (size_t i = 0; i < sizeof(s_init) / sizeof(s_init[0]) && e == ESP_OK; i++) {
        e = panel_cmd(s_init[i].cmd, s_init[i].len ? s_init[i].data : NULL, s_init[i].len);
        if (s_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_init[i].delay_ms));
        }
    }
    return e;
}

void lowpower_screen_off(void)
{
    gpio_sleep_sel_dis(BSP_LCD_RST);                         /* reset line stays driven in light sleep */
    if (bsp_display_lock(1000) != ESP_OK) {
        s_standby_failures++;
        ESP_LOGE(TAG, "display lock timeout: panel not in standby (%lu failures)", (unsigned long)s_standby_failures);
        return;
    }
    esp_err_t e = enter_standby();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "panel standby failed (%s): retrying", esp_err_to_name(e));
        e = enter_standby();
    }
    bsp_display_unlock();
    /* Even a partly failed sequence may have reached SLPIN/DSTB: always re-initialise on wake. */
    s_standby = true;
    if (e != ESP_OK) {
        s_standby_failures++;
        ESP_LOGE(TAG, "panel standby failed: %s (%lu failures)", esp_err_to_name(e), (unsigned long)s_standby_failures);
    }
}

void lowpower_screen_on(void)
{
    s_wake_us = esp_timer_get_time();
    if (!s_standby) {
        return;
    }
    if (bsp_display_lock(1000) != ESP_OK) {
        s_wake_failures++;
        ESP_LOGE(TAG, "display lock timeout: panel wake skipped (stays flagged for the next wake)");
        return;
    }
    esp_err_t e = reset_and_init();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "panel wake failed (%s): retrying", esp_err_to_name(e));
        e = reset_and_init();
    }
    bsp_display_unlock();
    if (e == ESP_OK) {
        s_standby = false;
    } else {
        s_wake_failures++;                                   /* s_standby stays set: full init next time */
        ESP_LOGE(TAG, "panel wake failed: %s (%lu failures)", esp_err_to_name(e), (unsigned long)s_wake_failures);
    }
}

void lowpower_screen_show(void)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "display lock timeout: DISPON skipped");
        return;
    }
    /* The panel memory was lost in deep standby: redraw everything before the display goes on. */
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(20));                           /* last strip leaves the DMA */
    if (bsp_display_lock(1000) == ESP_OK) {
        panel_cmd(0x29, NULL, 0);                            /* DISPON */
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "panel on %d ms after wake", (int)((esp_timer_get_time() - s_wake_us) / 1000));
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
    int n = snprintf(buf, len, "Light sleep: %.1f%% of %.1f h off\n%lu sleeps, %lu rejected",
                     100.0 * s_tot_sleep_us / s_tot_off_us, s_tot_off_us / 3.6e9, (unsigned long)s_tot_sleeps,
                     (unsigned long)s_tot_rejected);
    uint32_t tf = touch_sleep_failures();
    if ((s_standby_failures || s_wake_failures || tf) && n > 0 && (size_t)n < len) {
        snprintf(buf + n, len - n, "\nFailures: panel off %lu, panel on %lu, touch sleep %lu",
                 (unsigned long)s_standby_failures, (unsigned long)s_wake_failures, (unsigned long)tf);
    }
}
