#include "touch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "board.h"

static const char *TAG = "touch";

#define POLL_MS 20

static i2c_master_dev_handle_t s_dev;
static TaskHandle_t s_task;
static volatile bool s_want;        /* requested state */
static volatile bool s_awake;       /* chip state: scanning */
static volatile bool s_need_sleep;  /* sleep command not yet confirmed (boot, or a failed attempt) */
static uint32_t s_sleep_failures;
static volatile bool s_pressed;
static volatile int16_t s_x, s_y;

static void reset_pulse(void)
{
    const gpio_num_t rst = (gpio_num_t)BOARD_PIN_TOUCH_RST;
    gpio_set_level(rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(BOARD_TOUCH_BOOT_MS));
}

/* The chip's own deep sleep; only a reset pulse brings it back. */
static esp_err_t chip_sleep(void)
{
#if BOARD_TOUCH_CHIP == 9217
    /* CST92xx: command mode (0xD101), then sleep (0xD105), as SensorLib TouchDrvCST92xx::sleep(). */
    const uint8_t cmd_mode[2] = {0xD1, 0x01}, sleep[2] = {0xD1, 0x05};
    esp_err_t e = i2c_master_transmit(s_dev, cmd_mode, sizeof(cmd_mode), 100);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (e == ESP_OK) {
        e = i2c_master_transmit(s_dev, sleep, sizeof(sleep), 100);
    }
    return e;
#else
    /* FT3168 (FT5x06 register set): power mode register 0xA5 = 3, hibernate. */
    const uint8_t hib[2] = {0xA5, 0x03};
    return i2c_master_transmit(s_dev, hib, sizeof(hib), 100);
#endif
}

/* One reading. Returns false when the chip did not answer or the data is not valid. */
static bool chip_read(bool *pressed, int *x, int *y)
{
#if BOARD_TOUCH_CHIP == 9217
    /* Register 0xD000, 10 bytes; byte 6 = 0xAB when valid; first point: status 0x06 = finger down,
     * 12-bit x and y. (esp_lcd_touch_cst9217) */
    const uint8_t reg[2] = {0xD0, 0x00};
    uint8_t d[10] = {0};
    if (i2c_master_transmit(s_dev, reg, sizeof(reg), 50) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    if (i2c_master_receive(s_dev, d, sizeof(d), 50) != ESP_OK || d[6] != 0xAB) {
        return false;
    }
    *pressed = (d[5] & 0x7F) > 0 && (d[0] & 0x0F) == 0x06;
    *x = (d[1] << 4) | (d[3] >> 4);
    *y = (d[2] << 4) | (d[3] & 0x0F);
#else
    /* FT5x06 layout: 0x02 = number of points, 0x03..0x06 = first point x (12 bit), y (12 bit). */
    const uint8_t reg = 0x02;
    uint8_t d[5] = {0};
    if (i2c_master_transmit_receive(s_dev, &reg, 1, d, sizeof(d), 50) != ESP_OK) {
        return false;
    }
    int n = d[0] & 0x0F;
    *pressed = n > 0 && n <= 5;
    *x = ((d[1] & 0x0F) << 8) | d[2];
    *y = ((d[3] & 0x0F) << 8) | d[4];
#endif
#if BOARD_TOUCH_MIRROR
    *x = BSP_LCD_H_RES - 1 - *x;
    *y = BSP_LCD_V_RES - 1 - *y;
#endif
    return true;
}

/* Reset pulse (the chip ignores I2C while asleep) and sleep command, up to 3 attempts. */
static bool put_to_sleep(void)
{
    for (int i = 0; i < 3; i++) {
        reset_pulse();
        esp_err_t e = chip_sleep();
        if (e == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "sleep command failed (attempt %d): %s", i + 1, esp_err_to_name(e));
    }
    s_sleep_failures++;
    return false;
}

static void touch_task(void *arg)
{
    (void)arg;
    int misses = 0;
    for (;;) {
        if (s_want && !s_awake) {
            reset_pulse();
            misses = 0;
            s_need_sleep = false;
            s_awake = true;
            ESP_LOGI(TAG, "touch on");
        } else if (!s_want && (s_awake || s_need_sleep)) {
            s_pressed = false;
            s_awake = false;
            s_need_sleep = !put_to_sleep();          /* retried at the next screen off if it failed */
            if (!s_need_sleep) {
                ESP_LOGI(TAG, "touch controller asleep");
            } else {
                ESP_LOGE(TAG, "touch controller may still be awake (%lu failed sleeps)",
                         (unsigned long)s_sleep_failures);
            }
        }
        if (!s_awake) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    /* nothing to do until asked */
            continue;
        }
        bool pressed = false;
        int x = 0, y = 0;
        if (chip_read(&pressed, &x, &y)) {
            misses = 0;
            if (pressed && x >= 0 && y >= 0 && x < BSP_LCD_H_RES && y < BSP_LCD_V_RES) {
                s_x = (int16_t)x;
                s_y = (int16_t)y;
                s_pressed = true;
            } else {
                s_pressed = false;
            }
        } else if (++misses == 50) {                    /* 1 s without a valid answer */
            s_pressed = false;
            ESP_LOGW(TAG, "touch controller not answering; BOOT still works the menu");
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
    }
}

static void indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_x;
    data->point.y = s_y;
    data->state = (s_awake && s_pressed) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void touch_init(i2c_master_bus_handle_t bus)
{
    const gpio_num_t rst = (gpio_num_t)BOARD_PIN_TOUCH_RST;
    gpio_hold_dis(rst);                                 /* latch left by v0.9.0 / v0.10.0 */
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_sleep_sel_dis(rst);                            /* stays high (released) in light sleep */

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "I2C device not added: no touch");
        return;
    }
    /* The chip may be awake (power-on) or asleep (ESP32 reset): the task resets it and sends the
     * sleep command, verified and retried, before anything else. */
    s_need_sleep = true;

    if (bsp_display_lock(1000) == ESP_OK) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, indev_read);
        bsp_display_unlock();
    }
    xTaskCreate(touch_task, "touch", 3072, NULL, 4, &s_task);
}

void touch_set_active(bool on)
{
    s_want = on;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void touch_wait_settled(int max_ms)
{
    if (s_task && !s_want) {
        xTaskNotifyGive(s_task);                        /* also retries a failed sleep */
    }
    for (int t = 0; s_task && (s_awake != s_want || (!s_want && s_need_sleep && s_sleep_failures == 0)) &&
                    t < max_ms; t += 5) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

uint32_t touch_sleep_failures(void)
{
    return s_sleep_failures;
}

void touch_sleep_only(i2c_master_bus_handle_t bus)
{
    const gpio_num_t rst = (gpio_num_t)BOARD_PIN_TOUCH_RST;
    gpio_hold_dis(rst);
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_sleep_sel_dis(rst);
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    if (!s_dev && i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "touch controller %s", put_to_sleep() ? "asleep" : "did not take the sleep command");
}
