#include "buttons.h"

#include "esp_timer.h"

#define DEBOUNCE_SAMPLES 3          /* 3 x 10 ms */
#define LONG_PRESS_MS    800

typedef struct {
    gpio_num_t pin;
    bool active_high;
    bool down;                      /* debounced state */
    int agree;                      /* consecutive samples disagreeing with 'down' */
    int64_t down_at_ms;
    bool long_fired;
    bool consumed;
} button_t;

static button_t s_boot = {.pin = PIN_BOOT, .active_high = false};
static button_t s_pwr  = {.pin = PIN_PWR,  .active_high = true};

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool raw_down(const button_t *b)
{
    return (gpio_get_level(b->pin) != 0) == b->active_high;
}

void buttons_init(void)
{
    gpio_config_t boot = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot);
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << PIN_PWR,
        .mode = GPIO_MODE_INPUT,    /* driven by the board's FET: low idle, high while pressed */
    };
    gpio_config(&pwr);
    buttons_sync_and_consume();
}

bool buttons_boot_down_raw(void) { return raw_down(&s_boot); }
bool buttons_pwr_down_raw(void)  { return raw_down(&s_pwr); }

static void sync_one(button_t *b)
{
    b->down = raw_down(b);
    b->agree = 0;
    b->down_at_ms = now_ms();
    b->long_fired = false;
    b->consumed = b->down;
}

void buttons_sync_and_consume(void)
{
    sync_one(&s_boot);
    sync_one(&s_pwr);
}

/* Returns SHORT/LONG for this button, or NONE. */
static int step(button_t *b)
{
    bool raw = raw_down(b);
    int64_t t = now_ms();
    if (raw != b->down) {
        if (++b->agree >= DEBOUNCE_SAMPLES) {
            b->agree = 0;
            b->down = raw;
            if (raw) {
                b->down_at_ms = t;
                b->long_fired = false;
                b->consumed = false;
            } else {
                bool short_press = !b->consumed && !b->long_fired;
                b->consumed = true;
                return short_press ? 1 : 0;
            }
        }
    } else {
        b->agree = 0;
    }
    if (b->down && !b->consumed && !b->long_fired && (t - b->down_at_ms) >= LONG_PRESS_MS) {
        b->long_fired = true;
        return 2;
    }
    return 0;
}

btn_evt_t buttons_poll(void)
{
    int r = step(&s_pwr);
    if (r == 1) {
        return BTN_PWR_SHORT;
    }
    if (r == 2) {
        return BTN_PWR_LONG;
    }
    r = step(&s_boot);
    if (r == 1) {
        return BTN_BOOT_SHORT;
    }
    if (r == 2) {
        return BTN_BOOT_LONG;
    }
    return BTN_EVT_NONE;
}
