#include "display.h"

#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;

esp_lcd_panel_io_handle_t display_panel_io(void)
{
    return s_io;
}

/* CO5300: window start on even, end on odd coordinates. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

lv_display_t *display_start(void)
{
    const esp_lv_adapter_config_t lv_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    if (esp_lv_adapter_init(&lv_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter init failed");
        return NULL;
    }
    const bsp_display_config_t cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (bsp_display_new(&cfg, &panel, &io) != ESP_OK || !panel || !io) {
        ESP_LOGE(TAG, "panel start failed");
        return NULL;
    }
    s_io = io;
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = 50,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "display registration failed");
        return NULL;
    }
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter start failed");
        return NULL;
    }
    ESP_LOGI(TAG, "display %dx%d ready (no touch input)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return disp;
}
