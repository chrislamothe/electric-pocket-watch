#include "timesvc.h"

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
#include "ext_rtc.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "time";

#define TZ_DEFAULT          "PST8PDT,M3.2.0,M11.1.0"
#define VALID_FROM          1704067200LL            /* 2024-01-01 UTC */
#define HOUR_US             3600000000LL
#define RTC_FIX_PERIOD_US   HOUR_US
#define DRIFT_PERIOD_US     HOUR_US
#define AUTO_SYNC_RTC_US    (7 * 24 * HOUR_US)
#define AUTO_SYNC_RC_US     (24 * HOUR_US)
#define AUTO_RETRY_US       (2 * HOUR_US)
#define NET_FRESH_US        (48 * HOUR_US)
#define MAX_SLEW_US         120000000LL             /* larger RTC/system differences need a policy */
#define MIN_LEARN_US        (4 * HOUR_US)
#define PPM_LIMIT           500.0f

/* Survives ESP32 resets (not power-off), like the system clock itself. */
#define KEEP_MAGIC 0x54494D45u
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t source;
} s_keep;

static SemaphoreHandle_t s_mx;
static time_source_t s_source;
static volatile uint32_t s_gen;
static bool s_rtc;                      /* PCF85063 present */
static bool s_rtc_dirty;                /* RTC must be rewritten from a trusted system clock */
static int64_t s_mono_net;              /* esp_timer of the last network sync, 0 = none this boot */
static int64_t s_mono_rtc_fix;          /* last RTC correction attempt, 0 = due */
static int64_t s_mono_rtc_write;        /* last RTC rewrite attempt */
static int64_t s_mono_drift;            /* last drift compensation step */
static int64_t s_mono_auto;             /* next automatic network sync */
static int64_t s_applied_us;            /* drift compensation applied since the last network sync */
static float s_ppm;                     /* RC drift, + = fast (1.75C) */
static bool s_ppm_ok;
static int64_t s_last_fix_us, s_last_fix_span_us;
static int64_t s_mono_last_fix;
static uint32_t s_rtc_failures, s_big_steps;

static int64_t mono(void) { return esp_timer_get_time(); }

int64_t timesvc_utc_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void set_utc_us(int64_t t)
{
    struct timeval tv = {.tv_sec = (time_t)(t / 1000000), .tv_usec = (suseconds_t)(t % 1000000)};
    settimeofday(&tv, NULL);
    s_gen++;
}

static void step_us(int64_t delta)
{
    set_utc_us(timesvc_utc_us() + delta);
}

static void set_source(time_source_t s)
{
    s_source = s;
    s_keep.magic = KEEP_MAGIC;
    s_keep.source = s;
}

static const char *source_name(time_source_t s)
{
    switch (s) {
    case TIME_NETWORK:  return "network";
    case TIME_RTC:      return "RTC";
    case TIME_RETAINED: return "kept over restart";
    default:            return "not set";
    }
}

static time_t build_utc(void)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int dd = 1, yy = 2026, hh = 0, mi = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &dd, &yy);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mi, &ss);
    const char *p = strstr(months, mon);
    int m = p ? (int)((p - months) / 3) + 1 : 1;
    int y = yy - (m <= 2);                          /* days_from_civil */
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1);
    long long days = era * 146097 + (long long)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    return (time_t)(days * 86400LL + hh * 3600 + mi * 60 + ss);
}

/* ---------------------------------------------------------------- public reads */

void timesvc_now(struct tm *out, int *ms)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t s = tv.tv_sec;
    localtime_r(&s, out);
    if (ms) {
        *ms = (int)(tv.tv_usec / 1000);
    }
}

time_t timesvc_utc(void) { return (time_t)(timesvc_utc_us() / 1000000); }
bool timesvc_trusted(void) { return s_source != TIME_UNSET; }
time_source_t timesvc_source(void) { return s_source; }
uint32_t timesvc_generation(void) { return s_gen; }

bool timesvc_lock(int timeout_ms)
{
    return s_mx && xSemaphoreTake(s_mx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void timesvc_unlock(void)
{
    xSemaphoreGive(s_mx);
}

/* ---------------------------------------------------------------- start-up */

void timesvc_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_rtc = ext_rtc_present();
    setenv("TZ", TZ_DEFAULT, 1);
    tzset();
    int ppm10 = settings_get_int("rc_ppm10", 0);
    if (ppm10 != 0) {
        s_ppm = ppm10 / 10.0f;
        s_ppm_ok = true;
    }

    long long rtc_s = 0;
    bool rtc_ok = s_rtc && ext_rtc_read(&rtc_s);
    if (s_rtc && !rtc_ok) {
        s_rtc_dirty = true;                          /* rewrite it from the first trusted time */
    }
    if (timesvc_utc() >= VALID_FROM) {
        /* ESP32 reset without power loss: the system clock kept running. */
        time_source_t was = (s_keep.magic == KEEP_MAGIC && s_keep.source <= TIME_NETWORK)
                                ? (time_source_t)s_keep.source : TIME_RETAINED;
        set_source(was == TIME_NETWORK ? TIME_RETAINED : was);
        s_mono_rtc_fix = 0;                          /* RTC boards: check it right away */
        ESP_LOGI(TAG, "clock kept over the restart (%s)", source_name(s_source));
    } else if (rtc_ok) {
        set_utc_us(rtc_s * 1000000 + 500000);        /* fraction unknown until the hourly tick check */
        set_source(TIME_RTC);
        s_mono_rtc_fix = 0;
        ESP_LOGI(TAG, "clock set from the RTC");
    } else {
        set_utc_us((int64_t)build_utc() * 1000000);
        set_source(TIME_UNSET);
        ESP_LOGW(TAG, "no time source: seeded from the build time, shown as not set");
    }
    s_mono_drift = mono();
    s_mono_auto = mono() + (s_rtc ? AUTO_SYNC_RTC_US : AUTO_SYNC_RC_US);   /* boot sync runs separately */
    if (s_ppm_ok && !s_rtc) {
        ESP_LOGI(TAG, "RC drift compensation %+.1f ppm", s_ppm);
    }
}

/* ---------------------------------------------------------------- network */

void timesvc_net_synced(int64_t err_us)
{
    int64_t now = mono();
    if (!s_rtc && s_mono_net && s_source == TIME_NETWORK && now - s_mono_net >= MIN_LEARN_US) {
        /* Drift that would have built up without compensation since the previous sync. */
        double raw = (double)(err_us - s_applied_us);
        float ppm = (float)(raw / (double)(now - s_mono_net) * 1e6);
        if (fabsf(ppm) < PPM_LIMIT) {
            s_ppm = s_ppm_ok ? 0.5f * s_ppm + 0.5f * ppm : ppm;
            s_ppm_ok = true;
            settings_set_int("rc_ppm10", (int)lrintf(s_ppm * 10) ? (int)lrintf(s_ppm * 10) : 1);
            ESP_LOGI(TAG, "RC drift measured %+.1f ppm, compensation now %+.1f ppm", ppm, s_ppm);
        }
    }
    ESP_LOGI(TAG, "network time: clock was %+lld ms off", (long long)(err_us / 1000));
    s_gen++;                                         /* SNTP stepped the clock */
    s_mono_net = now;
    s_applied_us = 0;
    s_mono_drift = now;
    s_mono_auto = now + (s_rtc ? AUTO_SYNC_RTC_US : AUTO_SYNC_RC_US);
    set_source(TIME_NETWORK);
    if (s_rtc) {
        s_mono_rtc_write = now;
        if (ext_rtc_write_aligned(timesvc_utc_us)) {
            s_rtc_dirty = false;
            s_mono_rtc_fix = mono();
        } else {
            s_rtc_dirty = true;
            s_rtc_failures++;
            ESP_LOGE(TAG, "network time not saved to the RTC (%lu failures); retried hourly",
                     (unsigned long)s_rtc_failures);
        }
    }
}

/* ---------------------------------------------------------------- RTC boards */

void timesvc_on_wake(void)
{
    if (!s_rtc || s_rtc_dirty || !timesvc_lock(50)) {
        return;
    }
    long long r = 0;
    if (ext_rtc_read(&r)) {
        int64_t diff = timesvc_utc_us() - (r * 1000000 + 500000);
        if (llabs(diff) > 1500000 && llabs(diff) <= MAX_SLEW_US) {
            set_utc_us(r * 1000000 + 500000);
            s_mono_rtc_fix = 0;                      /* exact correction at the next service call */
            ESP_LOGW(TAG, "wake: clock %+lld ms from the RTC, stepped", (long long)(diff / 1000));
        } else if (llabs(diff) > MAX_SLEW_US) {
            s_mono_rtc_fix = 0;                      /* large: resolved by the policy in rtc_fix() */
        }
    }
    timesvc_unlock();
}

static void rtc_fix(void)
{
    int64_t now = mono();
    s_mono_rtc_fix = now;
    long long r = 0;
    int64_t m = 0;
    if (!ext_rtc_read_edge(&r, &m)) {
        s_rtc_failures++;
        ESP_LOGW(TAG, "RTC read failed (%lu failures); next try in 1 h", (unsigned long)s_rtc_failures);
        return;
    }
    int64_t sys_at_tick = timesvc_utc_us() - (mono() - m);
    int64_t err = sys_at_tick - r * 1000000;         /* + = system clock fast */
    if (llabs(err) > MAX_SLEW_US) {
        s_big_steps++;
        if (s_source == TIME_NETWORK && now - s_mono_net < NET_FRESH_US) {
            ESP_LOGE(TAG, "RTC is %+lld s off a recent network time: rewriting the RTC", (long long)(-err / 1000000));
            s_rtc_dirty = true;
            return;
        }
        ESP_LOGE(TAG, "clock is %+lld s off the running RTC: taking the RTC", (long long)(err / 1000000));
    }
    step_us(-err);
    if (s_source != TIME_NETWORK) {
        set_source(TIME_RTC);
    }
    if (s_mono_last_fix) {
        s_last_fix_span_us = m - s_mono_last_fix;
    }
    s_mono_last_fix = m;
    s_last_fix_us = err;
    if (s_last_fix_span_us > 600000000LL) {
        ESP_LOGI(TAG, "clock corrected by %+lld ms after %.1f h (%+.0f ppm)", (long long)(-err / 1000),
                 s_last_fix_span_us / 3.6e9, err / (s_last_fix_span_us / 1e6));
    } else {
        ESP_LOGI(TAG, "clock corrected by %+lld ms", (long long)(-err / 1000));
    }
}

static void rtc_rewrite(void)
{
    s_mono_rtc_write = mono();
    if (ext_rtc_write_aligned(timesvc_utc_us)) {
        s_rtc_dirty = false;
        s_mono_rtc_fix = mono();
        ESP_LOGI(TAG, "RTC rewritten from the %s clock", source_name(s_source));
    } else {
        s_rtc_failures++;
        ESP_LOGE(TAG, "RTC rewrite failed (%lu failures)", (unsigned long)s_rtc_failures);
    }
}

/* ---------------------------------------------------------------- automatic sync */

static volatile bool s_auto_running;

static void auto_sync_task(void *arg)
{
    (void)arg;
    net_result_t r = net_sync_time();
    bool ok = (r == NET_OK || r == NET_OK_OPEN);
    ESP_LOGI(TAG, "automatic sync: %s", net_result_text(r));
    if (!ok) {
        s_mono_auto = mono() + AUTO_RETRY_US;
    }
    s_auto_running = false;
    vTaskDelete(NULL);
}

static void auto_sync_if_due(void)
{
    char ssid[33];
    if (s_auto_running || mono() < s_mono_auto || net_busy()) {
        return;
    }
    if (!settings_get_str("ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        s_mono_auto = mono() + AUTO_SYNC_RC_US;      /* no saved network: nothing to do */
        return;
    }
    s_auto_running = true;
    s_mono_auto = mono() + AUTO_RETRY_US;            /* until the task reports */
    if (xTaskCreate(auto_sync_task, "autosync", 6144, NULL, 3, NULL) != pdPASS) {
        s_auto_running = false;
    }
}

/* ---------------------------------------------------------------- periodic work */

void timesvc_service(void)
{
    if (!timesvc_lock(0)) {
        return;                                      /* a sync is running */
    }
    int64_t now = mono();
    if (s_rtc) {
        if (s_rtc_dirty) {
            if (s_source != TIME_UNSET && now - s_mono_rtc_write >= RTC_FIX_PERIOD_US) {
                rtc_rewrite();
            }
        } else if (s_mono_rtc_fix == 0 || now - s_mono_rtc_fix >= RTC_FIX_PERIOD_US) {
            rtc_fix();
        }
    } else if (s_ppm_ok && s_source != TIME_UNSET && now - s_mono_drift >= DRIFT_PERIOD_US) {
        int64_t delta = -(int64_t)((double)s_ppm * (double)(now - s_mono_drift) / 1e6);
        step_us(delta);
        s_applied_us += delta;
        s_mono_drift = now;
    }
    timesvc_unlock();
    auto_sync_if_due();
}

uint64_t timesvc_us_until_due(void)
{
    int64_t now = mono(), due = s_mono_auto;
    if (s_rtc) {
        int64_t next = s_rtc_dirty ? s_mono_rtc_write + RTC_FIX_PERIOD_US
                                   : (s_mono_rtc_fix ? s_mono_rtc_fix + RTC_FIX_PERIOD_US : now);
        if (next < due) {
            due = next;
        }
    } else if (s_ppm_ok && s_mono_drift + DRIFT_PERIOD_US < due) {
        due = s_mono_drift + DRIFT_PERIOD_US;
    }
    return due > now ? (uint64_t)(due - now) : 0;
}

void timesvc_summary(char *buf, size_t len)
{
    int n = snprintf(buf, len, "Time: %s", source_name(s_source));
    if (s_mono_net && n > 0 && (size_t)n < len) {
        n += snprintf(buf + n, len - n, ", synced %.1f h ago", (mono() - s_mono_net) / 3.6e9);
    }
    if (n > 0 && (size_t)n < len) {
        if (s_rtc) {
            if (s_rtc_dirty) {
                n += snprintf(buf + n, len - n, "\nRTC needs a sync");
            } else if (s_last_fix_span_us > 600000000LL) {
                n += snprintf(buf + n, len - n, "\nRTC check: %+lld ms (%+.0f ppm)", (long long)(-s_last_fix_us / 1000),
                              s_last_fix_us / (s_last_fix_span_us / 1e6));
            }
            if (s_rtc_failures && n > 0 && (size_t)n < len) {
                snprintf(buf + n, len - n, "\nRTC failures: %lu", (unsigned long)s_rtc_failures);
            }
        } else if (s_ppm_ok) {
            snprintf(buf + n, len - n, "\nDrift compensation %+.0f ppm", s_ppm);
        }
    }
}
