#pragma once

#include "lvgl.h"

/*
 * Display transfer path without per-strip memory allocation.
 *
 * The board package renders into two PSRAM strips (466 x 50 px) and hands each one to the SPI
 * driver. The SPI DMA cannot read PSRAM here, so the driver allocated a ~46 KB internal-RAM copy
 * for every strip and freed it afterwards. When internal RAM was short or fragmented (Wi-Fi,
 * building a face) the allocation failed, the strip was dropped, and the panel kept whatever it
 * showed before in that band: ghost hands, stale battery segments, half-old faces. The v0.8.3 log
 * shows exactly that (setup_dma_priv_buffer: Failed to alloc / Draw bitmap failed: ESP_ERR_NO_MEM).
 *
 * display_dma_init() allocates one strip-sized internal DMA buffer at boot, while memory is
 * still contiguous, and registers an esp_lvgl_adapter draw-bitmap callback that copies each strip
 * into it before sending. LVGL waits for the previous flush to finish before the next one
 * (double-buffered partial mode), so one buffer is enough. Call right after bsp_display_start().
 */
void display_dma_init(lv_display_t *disp);

/* The CO5300 panel handle, known after the first frame has been drawn (NULL before). */
#include "esp_lcd_types.h"
esp_lcd_panel_handle_t display_dma_panel(void);
