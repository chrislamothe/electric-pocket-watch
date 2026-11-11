#include "display_dma.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"

static const char *TAG = "display_dma";

static uint8_t *s_bounce;
static size_t s_bounce_size;
static uint32_t s_too_big;
static esp_lcd_panel_handle_t s_panel;       /* the board package keeps it private; seen here */

esp_lcd_panel_handle_t display_dma_panel(void)
{
    return s_panel;
}

static esp_err_t draw_cb(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x_start, int y_start,
                         int x_end, int y_end, const void *color_map, void *user_ctx)
{
    (void)user_ctx;
    s_panel = panel;
    size_t bytes = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) *
                   lv_color_format_get_size(lv_display_get_color_format(disp));
    if (bytes > s_bounce_size) {
        /* Larger than a draw strip: cannot happen in partial mode, but fall back to the driver's
         * own copy rather than drop the area. */
        if (s_too_big++ == 0) {
            ESP_LOGW(TAG, "area of %u bytes exceeds the %u byte buffer", (unsigned)bytes, (unsigned)s_bounce_size);
        }
        return esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_map);
    }
    memcpy(s_bounce, color_map, bytes);
    return esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, s_bounce);
}

void display_dma_init(lv_display_t *disp)
{
    if (!disp || bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGE(TAG, "not installed (no display or LVGL lock timeout)");
        return;
    }
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    size_t size = buf ? buf->data_size : 0;
    if (size == 0) {
        size = (size_t)lv_display_get_horizontal_resolution(disp) * 50 * 2;
    }
    s_bounce = heap_caps_aligned_alloc(4, size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bounce) {
        ESP_LOGE(TAG, "no %u byte internal DMA buffer: keeping the driver's per-strip copies", (unsigned)size);
        bsp_display_unlock();
        return;
    }
    s_bounce_size = size;
    const esp_lv_adapter_draw_bitmap_callbacks_t cbs = {.custom_draw_bitmap = draw_cb};
    esp_err_t e = esp_lv_adapter_set_draw_bitmap_callbacks(disp, &cbs, NULL);
    bsp_display_unlock();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "draw callback not installed: %s", esp_err_to_name(e));
        heap_caps_free(s_bounce);
        s_bounce = NULL;
        s_bounce_size = 0;
        return;
    }
    ESP_LOGI(TAG, "%u byte internal DMA strip buffer installed; internal DMA free %u (largest %u)",
             (unsigned)size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}
