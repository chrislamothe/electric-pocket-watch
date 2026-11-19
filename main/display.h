#pragma once

#include "lvgl.h"

/*
 * Display bring-up without a touch input device: SPI bus and panel through the board package's
 * bsp_display_new(), then LVGL through esp_lvgl_adapter (50-line PSRAM strips, double buffered,
 * even-aligned areas for the CO5300), as Waveshare's own bsp_display_start() does. That call is
 * not used because it also starts the touch controller and aborts the boot when it does not answer.
 *
 * Call after bsp_i2c_init(). Returns NULL on failure. The LVGL worker is running on return.
 */
lv_display_t *display_start(void);

/* Panel IO handle (QSPI command channel), NULL before display_start(). */
#include "esp_lcd_types.h"
esp_lcd_panel_io_handle_t display_panel_io(void);
