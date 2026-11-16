#include "board.h"

#include "driver/gpio.h"

void board_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PWR,
        .mode = GPIO_MODE_INPUT,            /* SYS_OUT through a FET, 10K pull-up: high while held */
    };
    gpio_config(&pwr);

    gpio_config_t motor = {
        .pin_bit_mask = 1ULL << BOARD_PIN_MOTOR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&motor);
    gpio_set_level(BOARD_PIN_MOTOR, 0);
    gpio_hold_en(BOARD_PIN_MOTOR);          /* stays off through light sleep */
}

bool board_pwr_down(void)
{
    return gpio_get_level(BOARD_PIN_PWR) != 0;
}
