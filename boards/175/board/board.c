#include "board.h"
#include "esp_timer.h"
/* evlog lives in main, which this component cannot depend on: weak reference, checked before use */
extern void evlog_add(const char *what, const char *tag, int32_t a, int32_t b) __attribute__((weak));

#include "esp_log.h"

static const char *TAG = "board";

#define TCA9554_ADDR      0x20      /* A2..A0 = 000 */
#define TCA9554_INPUT     0x00
#define TCA9554_CONFIG    0x03      /* 1 = input (power-on default 0xFF) */
#define EXIO_SYS_OUT      (1u << 4) /* P4: PWR held = high */

static i2c_master_dev_handle_t s_exp;

void board_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_exp) != ESP_OK) {
        s_exp = NULL;
        ESP_LOGE(TAG, "TCA9554 not added: PWR button unavailable");
        return;
    }
    uint8_t reg = TCA9554_CONFIG, dir = 0;
    if (i2c_master_transmit_receive(s_exp, &reg, 1, &dir, 1, 100) != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 not answering: PWR button unavailable");
        return;
    }
    if (!(dir & EXIO_SYS_OUT)) {
        uint8_t w[2] = {TCA9554_CONFIG, (uint8_t)(dir | EXIO_SYS_OUT)};   /* P4 must be an input */
        i2c_master_transmit(s_exp, w, sizeof(w), 100);
    }
    ESP_LOGI(TAG, "TCA9554 ready (direction 0x%02X); PWR read through P4", dir);
}

bool board_pwr_down(void)
{
    uint8_t reg = TCA9554_INPUT, in = 0;
    if (!s_exp) {
        return false;
    }
    esp_err_t e = i2c_master_transmit_receive(s_exp, &reg, 1, &in, 1, 20);
    if (e != ESP_OK) {
        static int64_t s_last_ms;
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_last_ms > 1000) {                /* at most one entry per second */
            s_last_ms = now;
            if (evlog_add) {
                evlog_add("i2c_err", "TCA9554", e, 0);
            }
        }
        return false;
    }
    return (in & EXIO_SYS_OUT) != 0;
}
