#include "board.h"

#include "driver/gpio.h"

void board_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PWR,
        .mode = GPIO_MODE_INPUT,            /* driven by the board's FET: low idle, high while held */
    };
    gpio_config(&pwr);
}

bool board_pwr_down(void)
{
    return gpio_get_level(BOARD_PIN_PWR) != 0;
}
