/*
 * Board package for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * Built from Waveshare's own sources, with the LVGL side matching their 1.75 / 1.75C packages:
 * - Panel bring-up (SH8601 driver, init sequence, 0x16 column gap) from the corrected board
 *   package in Waveshare's 2.06 factory firmware (examples/factory/components/
 *   waveshare__esp32_s3_touch_amoled_2_06). The registry package v2.0.0 registers this QSPI panel
 *   through esp_lvgl_port's RGB path, which the factory copy replaced; this file avoids both.
 * - Same bsp_display_new() / brightness / lock calls as Waveshare's 1.75 packages, so
 *   main/display.c brings LVGL up identically on every board. The touch controller (FT3168) is
 *   not used: main/parked.c puts it to sleep.
 */

#include "bsp/esp-bsp.h"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"

static const char *TAG = "bsp_206";

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

/* Waveshare factory firmware sequence. Brightness (0x51) stays 0 until the application has drawn
 * its first frame. */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
};

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = BSP_I2C_NUM,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return s_i2c;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness_percent < 0 || brightness_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t v = (uint8_t)(brightness_percent * 255 / 100);
    uint32_t cmd = (0x02u << 24) | (0x51u << 8);            /* QSPI write-command opcode, DCS 0x51 */
    return esp_lcd_panel_io_tx_param(s_io, (int)cmd, &v, 1);
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(config && config->max_transfer_sz > 0, ESP_ERR_INVALID_ARG, TAG, "config");
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1,
                                                              BSP_LCD_DATA2, BSP_LCD_DATA3,
                                                              config->max_transfer_sz);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_cfg, &s_io),
                        TAG, "panel IO");

    sh8601_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, BSP_LCD_X_GAP, 0), TAG, "panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");
    if (ret_panel) {
        *ret_panel = s_panel;
    }
    if (ret_io) {
        *ret_io = s_io;
    }
    return ESP_OK;
}

esp_err_t bsp_display_lock(uint32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
}
