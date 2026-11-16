#include "parked.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "board.h"
#include "i2c_dev.h"

static const char *TAG = "parked";

/* ESP-IDF isolates (floats) every GPIO during light sleep unless told otherwise
 * ("sleep_gpio: Configure to isolate all GPIO pins in sleep state" in the boot log). The pins
 * driven here must keep their level while asleep: a floating reset or enable line could wake the
 * chip behind it. gpio_sleep_sel_dis() keeps the pin's normal configuration in sleep; unlike
 * gpio_hold it does not outlive a reset. */
static void out_low(int pin)
{
    if (pin < 0) {
        return;
    }
    gpio_hold_dis((gpio_num_t)pin);                     /* latch left by v0.9.0 / v0.10.0 */
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)pin, 0);
    gpio_sleep_sel_dis((gpio_num_t)pin);
}

/* ---------------------------------------------------------------- accelerometer */

/* QMI8658 (0x6B). It keeps power across ESP32 resets and may still be in wake-on-motion from an
 * older firmware: soft reset, both sensors off (CTRL7 = 0), internal oscillator off (CTRL1 bit0,
 * with bit6 = address auto-increment as after reset). */
static void imu_power_down(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev = i2c_dev_add(bus, 0x6B);
    uint8_t who = 0;
    if (!dev || i2c_reg_read(dev, 0x00, &who, 1) != ESP_OK) {
        ESP_LOGW(TAG, "accelerometer not answering");
        return;
    }
    i2c_reg_write(dev, 0x60, 0xB0);                     /* soft reset; NACKs for ~2 ms */
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t e = i2c_reg_write(dev, 0x08, 0x00);
    if (e == ESP_OK) {
        e = i2c_reg_write(dev, 0x02, 0x41);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(BOARD_PIN_IMU_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,           /* interrupt pin unused: keep it defined */
    };
    gpio_config(&cfg);
    gpio_sleep_sel_dis((gpio_num_t)BOARD_PIN_IMU_INT);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "accelerometer powered down (id 0x%02X)", who);
    } else {
        ESP_LOGW(TAG, "accelerometer power-down failed: %s", esp_err_to_name(e));
    }
}

/* ---------------------------------------------------------------- audio */

typedef struct {
    uint8_t reg, val;
} regval_t;

static void write_seq(i2c_master_bus_handle_t bus, uint8_t addr, const char *name, const regval_t *seq, int n)
{
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
        ESP_LOGW(TAG, "%s not answering", name);
        return;
    }
    i2c_master_dev_handle_t dev = i2c_dev_add(bus, addr);
    if (!dev) {
        return;
    }
    esp_err_t e = ESP_OK;
    for (int i = 0; i < n && e == ESP_OK; i++) {
        e = i2c_reg_write(dev, seq[i].reg, seq[i].val);
    }
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "%s powered down", name);
    } else {
        ESP_LOGW(TAG, "%s power-down failed: %s", name, esp_err_to_name(e));
    }
}

/* Register sequences from Espressif's esp_codec_dev: es7210_stop() and es8311_suspend(). */
static void audio_power_down(i2c_master_bus_handle_t bus)
{
    static const regval_t es7210[] = {
        {0x47, 0xFF}, {0x48, 0xFF}, {0x49, 0xFF}, {0x4A, 0xFF},     /* MIC1..4 off */
        {0x4B, 0xFF}, {0x4C, 0xFF},                                 /* bias, ADC, PGA off */
        {0x40, 0xC0},                                               /* analog off */
        {0x01, 0x7F},                                               /* clocks off */
        {0x06, 0x07},                                               /* power down */
    };
    static const regval_t es8311[] = {
        {0x32, 0x00}, {0x17, 0x00}, {0x0E, 0xFF}, {0x12, 0x02}, {0x14, 0x00}, {0x0D, 0xFA},
        {0x15, 0x00}, {0x02, 0x10}, {0x00, 0x00}, {0x00, 0x1F}, {0x01, 0x30}, {0x01, 0x00},
        {0x45, 0x00}, {0x0D, 0xFC}, {0x02, 0x00},
    };
    write_seq(bus, 0x40, "microphone ADC (ES7210)", es7210, sizeof(es7210) / sizeof(es7210[0]));
    write_seq(bus, 0x18, "audio codec (ES8311)", es8311, sizeof(es8311) / sizeof(es8311[0]));
}

void parked_init(i2c_master_bus_handle_t bus)
{
    out_low(BOARD_PIN_SPEAKER_PA);                      /* amplifier enable */
    out_low(BOARD_PIN_MOTOR);
    imu_power_down(bus);
    audio_power_down(bus);
}
