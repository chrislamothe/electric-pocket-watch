#include "battlog.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "axp.h"

static const char *TAG = "battlog";

#define MAX_SAMPLES 96                      /* 24 h at 15 min */
#define PERIOD_US   ((int64_t)BATTLOG_PERIOD_MIN * 60 * 1000000)

typedef struct {
    uint32_t t_s;                           /* uptime, seconds */
    int16_t mv;
    int8_t gauge;
    uint8_t charge;                         /* axp_charge_t; 0xFF if on USB */
} sample_t;

static sample_t s_log[MAX_SAMPLES];
static int s_count, s_head;                 /* ring buffer */
static int64_t s_last_us = -PERIOD_US;
static int s_base = -1;                     /* index of the first sample of the current discharge */
static uint32_t s_wakes[3];

void battlog_note_wake(battlog_wake_t w)
{
    s_wakes[w]++;
}

bool battlog_due(void)
{
    return esp_timer_get_time() - s_last_us >= PERIOD_US;
}

uint64_t battlog_us_until_due(void)
{
    int64_t left = PERIOD_US - (esp_timer_get_time() - s_last_us);
    return left > 1000000 ? (uint64_t)left : 1000000;
}

void battlog_sample(const char *why)
{
    int64_t now = esp_timer_get_time();
    s_last_us = now;
    bool usb = axp_vbus_good();
    sample_t s = {
        .t_s = (uint32_t)(now / 1000000),
        .mv = (int16_t)axp_battery_mv(),
        .gauge = (int8_t)axp_gauge_percent(),
        .charge = usb ? 0xFF : (uint8_t)axp_charge_state(),
    };
    int idx = s_head;
    s_log[idx] = s;
    s_head = (s_head + 1) % MAX_SAMPLES;
    if (s_count < MAX_SAMPLES) {
        s_count++;
    } else if (s_base == idx) {
        s_base = s_head;                    /* oldest discharge sample was overwritten */
    }
    if (usb) {
        s_base = -1;                        /* a new discharge starts after unplugging */
    } else if (s_base < 0) {
        s_base = idx;
    }
    ESP_LOGI(TAG, "[%s] t=%lus battery %d mV (%d%%), gauge %d%%, %s, wakes: buttons %lu taps %lu motion-rejected %lu",
             why, (unsigned long)s.t_s, s.mv, axp_mv_to_percent(s.mv), s.gauge, usb ? "USB" : "battery",
             (unsigned long)s_wakes[0], (unsigned long)s_wakes[1], (unsigned long)s_wakes[2]);
}

void battlog_summary(char *buf, size_t len)
{
    int mv = axp_battery_mv();
    int g = axp_gauge_percent();
    bool usb = axp_vbus_good();
    static const char *const CHG[] = {"idle", "charging", "discharging", "?"};
    size_t o = 0;
    if (mv > 0) {
        o += snprintf(buf + o, len - o, "Battery %d.%02d V = %d%%, gauge %d%%\n%s, %s\n", mv / 1000,
                      (mv % 1000) / 10, axp_mv_to_percent(mv), g, usb ? "USB power" : "on battery",
                      CHG[axp_charge_state() & 3]);
    } else {
        o += snprintf(buf + o, len - o, "No battery reading\n");
    }
    if (s_base >= 0 && !usb) {
        const sample_t *first = &s_log[s_base];
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
        float hours = (now_s - first->t_s) / 3600.0f;
        int d = axp_mv_to_percent(first->mv) - axp_mv_to_percent(mv);
        if (hours >= 0.25f) {
            o += snprintf(buf + o, len - o, "Unplugged %.1f h: %d.%02d -> %d.%02d V\n%d%% used, %.1f%%/h\n", hours,
                          first->mv / 1000, (first->mv % 1000) / 10, mv / 1000, (mv % 1000) / 10, d, d / hours);
        } else {
            o += snprintf(buf + o, len - o, "Unplugged %.0f min, measuring\n", hours * 60);
        }
    }
    snprintf(buf + o, len - o, "Wakes: %lu button, %lu tap, %lu motion only", (unsigned long)s_wakes[0],
             (unsigned long)s_wakes[1], (unsigned long)s_wakes[2]);
}
