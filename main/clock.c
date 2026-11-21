#include "clock.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "clock_rtc.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "clock";

#define TZ_DEFAULT          "PST8PDT,M3.2.0,M11.1.0"
#define MIN_UTC_S           1704067200LL            /* 2024-01-01 */
#define HOUR_US             3600000000LL
#define REFINE_MAX_AGE_US   HOUR_US                 /* RTC boards: re-anchor at least hourly */
#define LEARN_MIN_SLEEP_US  (10 * 60 * 1000000LL)   /* sleep needed before an error sample counts */
#define PPM_LIMIT           50000.0
#define AUTO_SYNC_RTC_US    (7 * 24 * HOUR_US)
#define AUTO_SYNC_RC_US     (24 * HOUR_US)
#define AUTO_RETRY_US       (2 * HOUR_US)
#define NOTIFY_REFINE       0x01

/* ---------------------------------------------------------------- published state */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static clock_quality_t s_quality;
static clock_source_t s_source;
static int64_t s_anchor_utc, s_anchor_mono;     /* valid unless UNSET */
static volatile uint32_t s_gen;

/* ---------------------------------------------------------------- service-private state
 * Changed only with s_hw held (or before the task starts). */

static SemaphoreHandle_t s_hw;                  /* RTC/network transactions and sleep */
static TaskHandle_t s_task;
static bool s_rtc;                              /* PCF85063 present */
static bool s_rtc_trusted;                      /* running, valid, and not contradicted */
static pcf_status_t s_rtc_last = PCF_OK;        /* last RTC operation result */
static pcf_status_t s_rtc_write = PCF_OK;       /* last RTC write result */
static bool s_rtc_written;                      /* a write was attempted since power-on */
static int64_t s_ref_utc, s_ref_mono;           /* last precise reference (refine or network) */
static bool s_ref_ok;
static int64_t s_slept_us;                      /* asleep since that reference */
static int64_t s_sleep_start;
static double s_ppm;                            /* sleep-clock error, + = fast */
static bool s_ppm_ok;
static double s_ppm_last;                       /* last single measurement */
static int64_t s_last_err_us, s_last_err_sleep_us;
static int64_t s_mono_net;                      /* last network reference, 0 = none */
static int64_t s_next_auto;
static volatile bool s_auto_running;
static uint32_t s_refines, s_rtc_failures, s_conflicts;

/* Survives ESP32 resets (not power-off), as the libc clock does. */
#define KEEP_MAGIC 0x434C4B32u
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t source;
} s_keep;

static int64_t mono(void) { return esp_timer_get_time(); }

/* ---------------------------------------------------------------- reads (any task, lock-free) */

static int64_t est_at(int64_t m)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t u = s_anchor_utc + (m - s_anchor_mono);
    taskEXIT_CRITICAL(&s_mux);
    return u;
}

static int64_t now_est(void)
{
    return est_at(mono());
}

bool clock_utc_us(int64_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    bool set = s_quality != CLOCK_UNSET;
    int64_t u = s_anchor_utc + (mono() - s_anchor_mono);
    taskEXIT_CRITICAL(&s_mux);
    *out = u;
    return set;
}

bool clock_local(struct tm *out)
{
    int64_t u;
    if (!clock_utc_us(&u)) {
        memset(out, 0, sizeof(*out));
        out->tm_hour = 10;
        out->tm_min = 10;
        out->tm_mday = 1;
        out->tm_year = 126;
        return false;
    }
    time_t s = (time_t)(u / 1000000);
    localtime_r(&s, out);
    return true;
}

clock_quality_t clock_quality(void) { return s_quality; }
clock_source_t clock_source(void) { return s_source; }
uint32_t clock_generation(void) { return s_gen; }

/* ---------------------------------------------------------------- state changes (s_hw held) */

static void mirror_libc(void)
{
    int64_t u = now_est();
    struct timeval tv = {.tv_sec = (time_t)(u / 1000000), .tv_usec = (suseconds_t)(u % 1000000)};
    settimeofday(&tv, NULL);
}

static void publish(int64_t utc, int64_t m, clock_quality_t q, clock_source_t src)
{
    taskENTER_CRITICAL(&s_mux);
    s_anchor_utc = utc;
    s_anchor_mono = m;
    s_quality = q;
    s_source = src;
    s_gen++;
    taskEXIT_CRITICAL(&s_mux);
    s_keep.magic = KEEP_MAGIC;
    s_keep.source = src;
    mirror_libc();
}

static void set_quality(clock_quality_t q)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_quality != q) {
        s_quality = q;
        s_gen++;
    }
    taskEXIT_CRITICAL(&s_mux);
}

/* A precise reference (utc at mono m) arrived: measure the sleep-clock error against the
 * uncorrected bridge from the previous reference, then make this the new reference. */
static void take_reference(int64_t utc, int64_t m, const char *what)
{
    if (s_ref_ok && s_slept_us >= LEARN_MIN_SLEEP_US) {
        int64_t bridged = s_ref_utc + (m - s_ref_mono);           /* esp_timer alone */
        int64_t err = bridged - utc;                              /* + = sleep clock fast */
        double ppm = (double)err / (double)s_slept_us * 1e6;
        s_last_err_us = err;
        s_last_err_sleep_us = s_slept_us;
        if (fabs(ppm) < PPM_LIMIT) {
            s_ppm_last = ppm;
            s_ppm = s_ppm_ok ? 0.7 * s_ppm + 0.3 * ppm : ppm;
            s_ppm_ok = true;
            int p10 = (int)lrint(s_ppm * 10);
            settings_set_int("slp_ppm10", p10 ? p10 : 1);
            ESP_LOGI(TAG, "%s: sleep clock was %+lld ms off after %.1f min asleep (%+.0f ppm; using %+.0f ppm)",
                     what, (long long)(err / 1000), s_slept_us / 6e7, ppm, s_ppm);
        } else {
            ESP_LOGW(TAG, "%s: implausible sleep error %+lld ms over %.1f min: ignored", what,
                     (long long)(err / 1000), s_slept_us / 6e7);
        }
    }
    s_ref_utc = utc;
    s_ref_mono = m;
    s_ref_ok = true;
    s_slept_us = 0;
}

/* ---------------------------------------------------------------- RTC */

static void rtc_failed(pcf_status_t st, const char *during)
{
    s_rtc_last = st;
    s_rtc_failures++;
    if (st == PCF_OSC_LOST || st == PCF_STOPPED || st == PCF_BAD_DATA || st == PCF_NO_TICK) {
        s_rtc_trusted = false;                                    /* until a verified write */
    }
    ESP_LOGW(TAG, "RTC %s: %s%s", during, pcf_status_text(st), s_rtc_trusted ? "" : " - not trusted until a sync");
}

/* Precise anchor on the RTC rollover. s_hw held. */
static void rtc_refine(void)
{
    int64_t r = 0, m = 0, win = 0;
    pcf_status_t st = pcf_read_edge(&r, &m, &win);
    if (st != PCF_OK) {
        rtc_failed(st, "refine");
        if (s_quality == CLOCK_READY && s_source == CLOCK_SRC_RTC) {
            set_quality(CLOCK_ESTIMATED);
        }
        return;
    }
    s_rtc_last = PCF_OK;
    s_refines++;
    int64_t utc = r * 1000000;
    int64_t shift = utc - est_at(m);
    take_reference(utc, m, "RTC");
    clock_source_t src = (s_source == CLOCK_SRC_NETWORK && llabs(shift) < 2000000) ? CLOCK_SRC_NETWORK : CLOCK_SRC_RTC;
    publish(utc, m, CLOCK_READY, src);
    if (llabs(shift) >= 5000) {
        ESP_LOGI(TAG, "re-anchored on the RTC: %+lld ms (rollover bracket %lld us)", (long long)(shift / 1000),
                 (long long)win);
    }
}

/* ---------------------------------------------------------------- service task */

static void auto_sync_task(void *arg)
{
    (void)arg;
    net_result_t r = net_sync_time();
    ESP_LOGI(TAG, "automatic sync: %s", net_result_text(r));
    if (r != NET_OK && r != NET_OK_OPEN) {
        s_next_auto = mono() + AUTO_RETRY_US;
    }
    s_auto_running = false;
    vTaskDelete(NULL);
}

static void auto_sync_check(void)
{
    char ssid[33];
    if (s_auto_running || mono() < s_next_auto || net_busy()) {
        return;
    }
    s_next_auto = mono() + AUTO_RETRY_US;            /* until the job reports */
    if (!settings_get_str("ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        return;                                      /* no saved network */
    }
    s_auto_running = true;
    if (xTaskCreate(auto_sync_task, "autosync", 6144, NULL, 3, NULL) != pdPASS) {
        s_auto_running = false;
    }
}

static bool refine_due(void)
{
    return s_rtc && s_rtc_trusted && (!s_ref_ok || mono() - s_ref_mono >= REFINE_MAX_AGE_US);
}

static void clock_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(60000));
        if (((bits & NOTIFY_REFINE) && s_rtc && s_rtc_trusted) || refine_due()) {
            if (xSemaphoreTake(s_hw, pdMS_TO_TICKS(5000)) == pdTRUE) {
                rtc_refine();
                xSemaphoreGive(s_hw);
            }
        }
        auto_sync_check();
    }
}

/* ---------------------------------------------------------------- start-up */

void clock_init(i2c_master_bus_handle_t bus)
{
    s_hw = xSemaphoreCreateMutex();
    setenv("TZ", TZ_DEFAULT, 1);
    tzset();
    int p10 = settings_get_int("slp_ppm10", 0);
    if (p10 != 0) {
        s_ppm = p10 / 10.0;
        s_ppm_ok = true;
        ESP_LOGI(TAG, "sleep-clock correction %+.1f ppm (measured earlier)", s_ppm);
    }
    s_rtc = pcf_init(bus);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    bool retained = tv.tv_sec >= MIN_UTC_S && s_keep.magic == KEEP_MAGIC;
    int64_t libc_utc = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec, libc_mono = mono();

    if (s_rtc) {
        int64_t r = 0, m = 0, win = 0;
        pcf_status_t st = pcf_read_edge(&r, &m, &win);
        s_rtc_last = st;
        if (st == PCF_OK) {
            s_rtc_trusted = true;
            s_ref_utc = r * 1000000;
            s_ref_mono = m;
            s_ref_ok = true;
            publish(r * 1000000, m, CLOCK_READY, CLOCK_SRC_RTC);
            ESP_LOGI(TAG, "time from the RTC (rollover bracket %lld us)", (long long)win);
        } else {
            ESP_LOGW(TAG, "RTC not usable at start-up: %s", pcf_status_text(st));
        }
    }
    if (s_quality == CLOCK_UNSET && retained) {
        publish(libc_utc, libc_mono, CLOCK_ESTIMATED, CLOCK_SRC_RETAINED);
        ESP_LOGI(TAG, "time kept over the restart (estimate)");
    }
    if (s_quality == CLOCK_UNSET) {
        ESP_LOGW(TAG, "no time source: time not set until a sync");
    }
    s_next_auto = mono() + (s_quality == CLOCK_UNSET ? AUTO_RETRY_US : (s_rtc ? AUTO_SYNC_RTC_US : AUTO_SYNC_RC_US));
    xTaskCreate(clock_task, "clock", 4096, NULL, 4, &s_task);
}

/* ---------------------------------------------------------------- wake and sleep */

void clock_display_wake(void)
{
    if (!s_rtc || !s_rtc_trusted) {
        if (s_quality == CLOCK_READY && s_slept_us > 0) {
            set_quality(CLOCK_ESTIMATED);            /* bridged across sleep */
        }
        return;
    }
    if (xSemaphoreTake(s_hw, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;                                      /* a transaction is running; refine follows */
    }
    int64_t r = 0;
    pcf_status_t st = pcf_read(&r);
    if (st == PCF_OK) {
        int64_t m = mono(), est = est_at(m), lo = r * 1000000;
        if (est >= lo - 20000 && est < lo + 1020000) {
            set_quality(CLOCK_READY);                /* bridge agrees with the RTC second: keep its fraction */
        } else {
            publish(lo + 500000, m, CLOCK_READY, CLOCK_SRC_RTC);   /* phase unknown until the refine */
            ESP_LOGI(TAG, "wake: estimate was %+lld ms off the RTC second", (long long)((est - lo - 500000) / 1000));
        }
    } else {
        rtc_failed(st, "wake read");
        set_quality(CLOCK_ESTIMATED);
    }
    xSemaphoreGive(s_hw);
    if (s_task) {
        xTaskNotify(s_task, NOTIFY_REFINE, eSetBits);
    }
}

bool clock_sleep_prepare(void)
{
    if (xSemaphoreTake(s_hw, 0) != pdTRUE) {
        return false;
    }
    s_sleep_start = mono();
    return true;
}

void clock_sleep_resume(bool slept)
{
    if (slept) {
        int64_t d = mono() - s_sleep_start;
        s_slept_us += d;
        if (s_ppm_ok && s_quality != CLOCK_UNSET) {
            /* esp_timer was bridged by the sleep clock: remove its measured error. */
            int64_t corr = -(int64_t)llround(s_ppm * (double)d / 1e6);
            if (corr) {
                taskENTER_CRITICAL(&s_mux);
                s_anchor_utc += corr;
                taskEXIT_CRITICAL(&s_mux);
            }
        }
    }
    xSemaphoreGive(s_hw);
    if (slept && refine_due() && s_task) {
        xTaskNotify(s_task, NOTIFY_REFINE, eSetBits);
    }
}

uint64_t clock_us_until_due(void)
{
    int64_t now = mono(), due = s_next_auto;
    if (s_rtc && s_rtc_trusted && s_ref_ok && s_ref_mono + REFINE_MAX_AGE_US < due) {
        due = s_ref_mono + REFINE_MAX_AGE_US;
    }
    return due > now ? (uint64_t)(due - now) : 0;
}

/* ---------------------------------------------------------------- network (SNTP) */

static portMUX_TYPE s_sntp_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_sntp_sem;
static struct timeval s_sntp_tv;
static int64_t s_sntp_mono;

/* Replaces ESP-IDF's weak sntp_sync_time() (components/lwip/apps/sntp/sntp.c), which would call
 * settimeofday() itself. Runs in the lwIP thread: record and signal only. */
void sntp_sync_time(struct timeval *tv)
{
    taskENTER_CRITICAL(&s_sntp_mux);
    s_sntp_tv = *tv;
    s_sntp_mono = mono();
    taskEXIT_CRITICAL(&s_sntp_mux);
    if (s_sntp_sem) {
        xSemaphoreGive(s_sntp_sem);
    }
}

void clock_sntp_arm(void)
{
    if (!s_sntp_sem) {
        s_sntp_sem = xSemaphoreCreateBinary();
    }
    xSemaphoreTake(s_sntp_sem, 0);                   /* drop a stale measurement */
}

bool clock_sntp_wait_sample(int timeout_ms, int64_t *utc_us, int64_t *mono_us)
{
    if (!s_sntp_sem || xSemaphoreTake(s_sntp_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    taskENTER_CRITICAL(&s_sntp_mux);
    *utc_us = (int64_t)s_sntp_tv.tv_sec * 1000000 + s_sntp_tv.tv_usec;
    *mono_us = s_sntp_mono;
    taskEXIT_CRITICAL(&s_sntp_mux);
    return *utc_us / 1000000 >= MIN_UTC_S;
}

bool clock_sntp_wait_apply(int timeout_ms)
{
    if (!s_sntp_sem || xSemaphoreTake(s_sntp_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    taskENTER_CRITICAL(&s_sntp_mux);
    int64_t utc = (int64_t)s_sntp_tv.tv_sec * 1000000 + s_sntp_tv.tv_usec, m = s_sntp_mono;
    taskEXIT_CRITICAL(&s_sntp_mux);
    if (utc / 1000000 < MIN_UTC_S) {
        ESP_LOGW(TAG, "network time %lld rejected", (long long)(utc / 1000000));
        return false;
    }
    if (xSemaphoreTake(s_hw, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "clock busy: network time dropped");
        return false;
    }
    bool was_set = s_quality != CLOCK_UNSET;
    int64_t off = was_set ? est_at(m) - utc : 0;
    if (s_rtc && s_rtc_trusted) {
        int64_t r = 0;
        if (pcf_read(&r) == PCF_OK) {
            int64_t rtc_off = r * 1000000 + 500000 - (utc + (mono() - m));
            if (llabs(rtc_off) > 2000000) {
                s_conflicts++;
                ESP_LOGW(TAG, "RTC was %+lld s off the network time", (long long)(rtc_off / 1000000));
            }
        }
    }
    if (!s_rtc) {
        take_reference(utc, m, "network");           /* the only reference on the 1.75C */
    } else {
        s_ref_utc = utc;                             /* the RTC refine measures the sleep error */
        s_ref_mono = m;
        s_ref_ok = true;
        s_slept_us = 0;
    }
    publish(utc, m, CLOCK_READY, CLOCK_SRC_NETWORK);
    s_mono_net = m;
    s_next_auto = mono() + (s_rtc ? AUTO_SYNC_RTC_US : AUTO_SYNC_RC_US);
    if (was_set) {
        ESP_LOGI(TAG, "network time: clock was %+lld ms off", (long long)(off / 1000));
    } else {
        ESP_LOGI(TAG, "network time: clock set");
    }
    if (s_rtc) {
        s_rtc_written = true;
        s_rtc_write = pcf_write(now_est);
        if (s_rtc_write == PCF_OK) {
            s_rtc_trusted = true;
            s_rtc_last = PCF_OK;
        } else {
            rtc_failed(s_rtc_write, "write");
            s_rtc_trusted = false;
        }
    }
    xSemaphoreGive(s_hw);
    return true;
}

/* ---------------------------------------------------------------- diagnostics */

void clock_summary(char *buf, size_t len)
{
    static const char *q[] = {"not set", "estimated", "ok"};
    static const char *src[] = {"none", "kept over restart", "RTC", "network"};
    int n = snprintf(buf, len, "Time: %s (%s)", q[s_quality], src[s_source]);
    if (s_mono_net && n > 0 && (size_t)n < len) {
        n += snprintf(buf + n, len - n, ", synced %.1f h ago", (mono() - s_mono_net) / 3.6e9);
    }
    if (s_rtc && n > 0 && (size_t)n < len) {
        n += snprintf(buf + n, len - n, "\nRTC: %s%s", s_rtc_trusted ? "running" : "not trusted",
                      s_rtc_last != PCF_OK ? " - " : "");
        if (s_rtc_last != PCF_OK && n > 0 && (size_t)n < len) {
            n += snprintf(buf + n, len - n, "%s", pcf_status_text(s_rtc_last));
        }
        if (s_rtc_written && s_rtc_write != PCF_OK && n > 0 && (size_t)n < len) {
            n += snprintf(buf + n, len - n, "\nLast RTC write: %s", pcf_status_text(s_rtc_write));
        }
        if ((s_rtc_failures || s_conflicts) && n > 0 && (size_t)n < len) {
            n += snprintf(buf + n, len - n, "\nRTC failures %lu, conflicts %lu", (unsigned long)s_rtc_failures,
                          (unsigned long)s_conflicts);
        }
    }
    if (s_ppm_ok && n > 0 && (size_t)n < len) {
        n += snprintf(buf + n, len - n, "\nSleep clock: %+.0f ppm (%+.0f s/day asleep)", s_ppm, s_ppm * 0.0864);
        if (s_last_err_sleep_us && n > 0 && (size_t)n < len) {
            snprintf(buf + n, len - n, "\nLast: %+lld ms over %.0f min asleep", (long long)(s_last_err_us / 1000),
                     s_last_err_sleep_us / 6e7);
        }
    }
}
