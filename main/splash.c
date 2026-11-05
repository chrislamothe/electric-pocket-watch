#include "splash.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "settings.h"
#include "version.h"

static const char *TAG = "splash";

static lv_obj_t *s_splash;
static int64_t s_shown_us;

static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_SW:        return "Restarted";
    case ESP_RST_PANIC:     return "Restarted after a crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "Restarted after a freeze";
    case ESP_RST_BROWNOUT:  return "Restarted: low voltage";
    case ESP_RST_USB:
    case ESP_RST_JTAG:      return "Restarted over USB";
    case ESP_RST_DEEPSLEEP: return "Woke from deep sleep";
    case ESP_RST_EXT:       return "External reset";
    default:                return "Started";
    }
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, const lv_font_t *font, uint32_t color, int dy)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, t);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
    return l;
}

void splash_show(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason %d: %s", (int)r, reset_text(r));
    if (bsp_display_lock(1000) != ESP_OK) {
        return;
    }
    s_splash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_splash, lv_color_black(), 0);
    lv_obj_remove_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    text(s_splash, "Electric", &lv_font_montserrat_36, 0xC9A04A, -52);
    text(s_splash, "Pocket Watch", &lv_font_montserrat_36, 0xC9A04A, -10);
    char v[24];
    snprintf(v, sizeof(v), "v%s", WATCH_VERSION);
    text(s_splash, v, &lv_font_montserrat_20, 0xA89F88, 38);
    bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                     r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
    text(s_splash, reset_text(r), &lv_font_montserrat_20, abnormal ? 0xD9734E : 0x7FB069, 74);
    bsp_display_unlock();

    bsp_display_brightness_set(settings_get_int("bright", CONFIG_WATCH_BRIGHTNESS));
    s_shown_us = esp_timer_get_time();
}

void splash_hide(void)
{
    if (!s_splash) {
        return;
    }
    int64_t left_ms = SPLASH_MIN_MS - (esp_timer_get_time() - s_shown_us) / 1000;
    if (left_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(left_ms));
    }
    if (bsp_display_lock(1000) == ESP_OK) {
        lv_obj_delete(s_splash);
        s_splash = NULL;
        bsp_display_unlock();
    }
}
