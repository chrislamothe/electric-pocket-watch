#pragma once

/*
 * Minimal board package for the Waveshare ESP32-S3-Touch-AMOLED-2.06, with the same API and
 * semantics as Waveshare's 1.75 / 1.75C packages (LVGL through esp_lvgl_adapter, lock returns
 * esp_err_t). Only what the watch firmware uses is provided.
 *
 * Hardware (Waveshare wiki and schematic V1.0): 2.06" AMOLED 410x502, CO5300 controller on QSPI
 * (driven with the SH8601 driver, as Waveshare does), FT3168 touch (FT5x06 protocol, I2C 0x38),
 * AXP2101, QMI8658, PCF85063, ES8311 + ES7210 + NS4150B audio, TF card on SPI.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "esp_lv_adapter.h"
#include "esp_lcd_types.h"

#define BSP_I2C_SCL           (GPIO_NUM_14)
#define BSP_I2C_SDA           (GPIO_NUM_15)
#define BSP_I2C_NUM           (I2C_NUM_1)
#define BSP_I2C_CLK_SPEED_HZ  (400000)

#define BSP_LCD_H_RES         (410)
#define BSP_LCD_V_RES         (502)
#define BSP_LCD_BITS_PER_PIXEL (16)
#define BSP_LCD_X_GAP         (0x16)    /* panel columns 22..431 */
#define BSP_LCD_SPI_NUM       (SPI2_HOST)
#define BSP_LCD_PCLK          (GPIO_NUM_11)
#define BSP_LCD_CS            (GPIO_NUM_12)
#define BSP_LCD_DATA0         (GPIO_NUM_4)
#define BSP_LCD_DATA1         (GPIO_NUM_5)
#define BSP_LCD_DATA2         (GPIO_NUM_6)
#define BSP_LCD_DATA3         (GPIO_NUM_7)
#define BSP_LCD_RST           (GPIO_NUM_8)
#define BSP_LCD_TE            (GPIO_NUM_13)
#define BSP_LCD_TOUCH_RST     (GPIO_NUM_9)
#define BSP_LCD_TOUCH_INT     (GPIO_NUM_38)
#define BSP_POWER_AMP_IO      (GPIO_NUM_46)

esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

typedef struct {
    int max_transfer_sz;    /* largest SPI transfer, bytes */
} bsp_display_config_t;

/* SPI bus and panel only (same call as Waveshare's 1.75 packages). The panel starts dark
 * (brightness 0) until bsp_display_brightness_set(). */
esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io);

esp_err_t bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);

/* 0..100 %, through the panel's 0x51 command (the AMOLED has no backlight). */
esp_err_t bsp_display_brightness_set(int brightness_percent);
