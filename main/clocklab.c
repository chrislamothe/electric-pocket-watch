#include "clocklab.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "axp.h"
#include "board.h"
#include "clock_rtc.h"
#include "net.h"
#include "parked.h"
#include "touch.h"
#include "version.h"

/*
 * CLOCK LAB - measures what light sleep does to the ESP32's time, against network time.
 *
 * Runs a fixed cycle of experiment blocks, each bracketed by an SNTP measurement (UTC and the
 * esp_timer instant it arrived) with the radio off in between. Within a block only one kind of
 * activity happens, so the error can be separated into:
 *   - awake rate        (block AWAKE: no sleep at all)
 *   - sleep rate        (few long sleeps)
 *   - per-sleep error   (many short sleeps, same total time)
 * On boards with the PCF85063 the RTC is read (on its seconds rollover) at both ends of each block,
 * giving the RTC's own rate against network time.
 *
 * Every esp_light_sleep_start() call is timed with esp_timer immediately before and after; S is
 * the sum of those windows for successful sleeps, N their count. Requested durations are never used.
 *
 * Results go to NVS (namespace "clocklab"), survive resets, and are printed over USB every 20 s
 * whenever USB power is present (experiments pause on USB: the watch does not light-sleep there).
 * When the run is complete (or the battery is low) the watch powers itself off: press PWR with
 * USB connected to read the results (USB cannot enumerate while the chip is in light sleep).
 * Hold BOOT for 5 s while on USB to erase the results and start a new run.
 *
 * Slow-clock source is a build option (CONFIG_RTC_CLK_SRC_*): the variant is part of every record.
 */

static const char *TAG = "clocklab";

#define LAB_FORMAT      2
#define CYCLES          2
#define REF_TRIES       3
#define LOW_MV          3500
#define PIN_BOOT        GPIO_NUM_0

#if CONFIG_RTC_CLK_SRC_INT_8MD256
#define VARIANT 1
#define VARIANT_NAME "RC_FAST/256"
#elif CONFIG_RTC_CLK_SRC_INT_RC
#define VARIANT 2
#define VARIANT_NAME "RC_SLOW"
#else
#define VARIANT 0
#define VARIANT_NAME "other"
#endif

typedef struct {
    const char *name;
    uint32_t count;          /* sleeps (0 = stay awake for awake_s) */
    uint32_t sleep_ms;       /* requested per sleep */
    uint32_t awake_s;
} block_t;

/* Format 2: format 1 showed no per-sleep error (0.2 s to 60 s sleeps all ~-7500 ppm) and a rate
 * that eases with sleep length (15 min ~-6700, 30 min ~-5200 ppm). This run maps long sleeps. */
static const block_t BLOCKS[] = {
    {"AWAKE", 0, 0, 1800},              /* 30 min CPU awake, no sleep */
    {"SEC", 1800, 1000, 0},             /* 1800 x 1 s */
    {"PROD", 4, 900000, 0},             /* 4 x 15 min (the watch's screen-off pattern) */
    {"LONG", 2, 1800000, 0},            /* 2 x 30 min */
    {"HOUR", 1, 3600000, 0},            /* 1 x 1 h */
    {"3HOUR", 1, 10800000, 0},          /* 1 x 3 h */
};
#define NBLOCKS ((int)(sizeof(BLOCKS) / sizeof(BLOCKS[0])))

typedef struct {
    uint8_t format, variant, cycle, block;
    uint32_t n_ok, n_rej;
    int64_t s_us;                       /* summed successful sleep-call windows */
    int64_t ref0_utc, ref0_mono, ref1_utc, ref1_mono;
    int64_t rtc0_s, rtc0_mono, rtc1_s, rtc1_mono;   /* 0 when no RTC / read failed */
    int16_t mv0, mv1;
    uint32_t req_ms;
} rec_t;

static nvs_handle_t s_nvs;
static bool s_rtc;

/* ---------------------------------------------------------------- storage */

static int rec_count(void)
{
    int32_t n = 0;
    nvs_get_i32(s_nvs, "n", &n);
    return (int)n;
}

static bool rec_get(int i, rec_t *r)
{
    char key[12];
    snprintf(key, sizeof(key), "r%d", i);
    size_t len = sizeof(*r);
    return nvs_get_blob(s_nvs, key, r, &len) == ESP_OK && len == sizeof(*r);
}

static void rec_add(const rec_t *r)
{
    int n = rec_count();
    char key[12];
    snprintf(key, sizeof(key), "r%d", n);
    nvs_set_blob(s_nvs, key, r, sizeof(*r));
    nvs_set_i32(s_nvs, "n", n + 1);
    nvs_commit(s_nvs);
}

static void state_get(int *cycle, int *block)
{
    int32_t c = 0, b = 0;
    nvs_get_i32(s_nvs, "cycle", &c);
    nvs_get_i32(s_nvs, "block", &b);
    *cycle = (int)c;
    *block = (int)b;
}

static void state_set(int cycle, int block)
{
    nvs_set_i32(s_nvs, "cycle", cycle);
    nvs_set_i32(s_nvs, "block", block);
    nvs_commit(s_nvs);
}

/* ---------------------------------------------------------------- report */

static void print_results(void)
{
    int n = rec_count(), cycle, block;
    state_get(&cycle, &block);
    printf("\n=== CLOCKLAB %s %s board %s variant %s: %d records, next cycle %d block %d of %d x %d ===\n",
           WATCH_NAME, WATCH_VERSION, BOARD_SHORT, VARIANT_NAME, n, cycle, block, CYCLES, NBLOCKS);
    printf("LABCSV,format,variant,cycle,block,name,n_ok,n_rej,req_ms,S_us,ref0_utc,ref0_mono,ref1_utc,ref1_mono,"
           "rtc0_s,rtc0_mono,rtc1_s,rtc1_mono,mv0,mv1\n");
    for (int i = 0; i < n; i++) {
        rec_t r;
        if (!rec_get(i, &r)) {
            continue;
        }
        printf("LABCSV,%u,%u,%u,%u,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
               ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%d\n",
               r.format, r.variant, r.cycle, r.block, r.block < NBLOCKS ? BLOCKS[r.block].name : "?", r.n_ok, r.n_rej,
               r.req_ms, r.s_us, r.ref0_utc, r.ref0_mono, r.ref1_utc, r.ref1_mono, r.rtc0_s, r.rtc0_mono, r.rtc1_s,
               r.rtc1_mono, r.mv0, r.mv1);
    }
    /* Derived, for reading at a glance (the CSV above is the data). */
    for (int i = 0; i < n; i++) {
        rec_t r;
        if (!rec_get(i, &r)) {
            continue;
        }
        double ref = (double)(r.ref1_utc - r.ref0_utc);           /* true elapsed (network) */
        double mono = (double)(r.ref1_mono - r.ref0_mono);        /* esp_timer elapsed */
        double err = mono - ref;                                  /* + = esp_timer fast */
        printf("LAB c%u %-5s %4.0f min: esp_timer %+9.0f ms (%+7.0f ppm overall", r.cycle,
               r.block < NBLOCKS ? BLOCKS[r.block].name : "?", ref / 6e7, err / 1000, err / ref * 1e6);
        if (r.n_ok) {
            printf(", %+7.0f ppm of sleep time, %+8.1f ms per sleep", err / (double)r.s_us * 1e6, err / r.n_ok / 1000);
        }
        printf(") sleeps %" PRIu32 " ok %" PRIu32 " rejected", r.n_ok, r.n_rej);
        if (r.rtc0_s && r.rtc1_s) {
            /* UTC at each RTC rollover, from the nearest network reference across awake-only gaps. */
            double u0 = (double)r.ref0_utc + (double)(r.rtc0_mono - r.ref0_mono);
            double u1 = (double)r.ref1_utc - (double)(r.ref1_mono - r.rtc1_mono);
            double rtc_err = (double)(r.rtc1_s - r.rtc0_s) * 1e6 - (u1 - u0);
            printf("; RTC %+7.1f ms (%+5.1f ppm)", rtc_err / 1000, rtc_err / (u1 - u0) * 1e6);
        }
        printf("\n");
    }
    printf("=== END CLOCKLAB ===\n");
    fflush(stdout);
}

/* ---------------------------------------------------------------- measurements */

static bool reference(int64_t *utc, int64_t *mono)
{
    for (int i = 0; i < REF_TRIES; i++) {
        if (net_measure_time(utc, mono)) {
            return true;
        }
        ESP_LOGW(TAG, "network time failed (try %d)", i + 1);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    return false;
}

static void rtc_mark(int64_t *s, int64_t *m)
{
    *s = 0;
    *m = 0;
    if (s_rtc) {
        int64_t win = 0;
        pcf_status_t st = pcf_read_edge(s, m, &win);
        if (st != PCF_OK) {
            ESP_LOGW(TAG, "RTC: %s", pcf_status_text(st));
            *s = 0;
        }
    }
}

static void light_sleep_ms(uint32_t ms, rec_t *r)
{
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
    int64_t m0 = esp_timer_get_time();
    esp_err_t e = esp_light_sleep_start();
    int64_t m1 = esp_timer_get_time();
    if (e == ESP_OK) {
        r->s_us += m1 - m0;
        r->n_ok++;
    } else {
        r->n_rej++;
    }
}

/* Returns false if USB power appeared (block discarded). */
static bool run_block(int cycle, int b)
{
    const block_t *bl = &BLOCKS[b];
    rec_t r = {.format = LAB_FORMAT, .variant = VARIANT, .cycle = (uint8_t)cycle, .block = (uint8_t)b,
               .req_ms = bl->sleep_ms};
    ESP_LOGI(TAG, "cycle %d block %s: reference", cycle, bl->name);
    if (!reference(&r.ref0_utc, &r.ref0_mono)) {
        return false;
    }
    rtc_mark(&r.rtc0_s, &r.rtc0_mono);
    r.mv0 = (int16_t)axp_battery_mv();
    ESP_LOGI(TAG, "cycle %d block %s: running", cycle, bl->name);
    vTaskDelay(pdMS_TO_TICKS(100));                  /* let the log drain */
    int64_t next_check = esp_timer_get_time() + 60000000;
    if (bl->count == 0) {
        int64_t end = esp_timer_get_time() + (int64_t)bl->awake_s * 1000000;
        while (esp_timer_get_time() < end) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (esp_timer_get_time() > next_check) {
                next_check += 60000000;
                if (axp_vbus_good()) {
                    return false;
                }
            }
        }
    } else {
        for (uint32_t i = 0; i < bl->count; i++) {
            light_sleep_ms(bl->sleep_ms, &r);
            if (esp_timer_get_time() > next_check) {
                next_check = esp_timer_get_time() + 60000000;
                if (axp_vbus_good()) {
                    return false;
                }
            }
        }
    }
    rtc_mark(&r.rtc1_s, &r.rtc1_mono);
    r.mv1 = (int16_t)axp_battery_mv();
    if (!reference(&r.ref1_utc, &r.ref1_mono)) {
        return false;
    }
    rec_add(&r);
    ESP_LOGI(TAG, "cycle %d block %s: saved", cycle, bl->name);
    return true;
}

/* ---------------------------------------------------------------- main loop */

static void erase_run(void)
{
    nvs_erase_all(s_nvs);
    nvs_commit(s_nvs);
    ESP_LOGW(TAG, "results erased: a new run starts when USB is unplugged");
}

void clocklab_run(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "CLOCK LAB firmware, slow clock %s", VARIANT_NAME);
    if (nvs_open("clocklab", NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable");
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    axp_init(bus);
    board_init(bus);
    parked_init(bus);                                /* same peripheral power as the watch */
    touch_sleep_only(bus);
    s_rtc = pcf_init(bus);
    gpio_config_t boot = {.pin_bit_mask = BIT64(PIN_BOOT), .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&boot);

    rec_t first;
    if (rec_count() > 0 && rec_get(0, &first) && first.format != LAB_FORMAT) {
        ESP_LOGW(TAG, "results from an older lab format: printing them now, then starting fresh");
        print_results();
        erase_run();
    }
    int64_t last_print = 0, boot_down_since = 0;
    for (;;) {
        if (axp_vbus_good()) {
            /* On USB: report, and allow a reset of the run with a 5 s BOOT hold. */
            int64_t now = esp_timer_get_time();
            if (now - last_print > 20000000 || last_print == 0) {
                last_print = now;
                print_results();
                printf("On USB: experiments paused. Unplug to run. Hold BOOT 5 s to erase results.\n");
            }
            if (gpio_get_level(PIN_BOOT) == 0) {
                if (!boot_down_since) {
                    boot_down_since = now;
                } else if (now - boot_down_since > 5000000) {
                    erase_run();
                    boot_down_since = 0;
                    last_print = 0;
                }
            } else {
                boot_down_since = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        last_print = 0;
        int cycle, block;
        state_get(&cycle, &block);
        int mv = axp_battery_mv();
        if (cycle >= CYCLES || (mv > 0 && mv < LOW_MV)) {
            ESP_LOGI(TAG, "%s: powering off (press PWR with USB connected to read the results)",
                     cycle >= CYCLES ? "run complete" : "battery low");
            vTaskDelay(pdMS_TO_TICKS(200));
            axp_power_off();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (run_block(cycle, block)) {
            block++;
            if (block >= NBLOCKS) {
                block = 0;
                cycle++;
            }
            state_set(cycle, block);
        } else if (!axp_vbus_good()) {
            ESP_LOGW(TAG, "block failed (no network time?): retrying in 10 min");
            esp_sleep_enable_timer_wakeup(600ULL * 1000000);
            esp_light_sleep_start();
        }
    }
}
