#include "watch_time.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "time";

static time_t build_epoch_utc(void)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int dd = 1, yy = 2026, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &dd, &yy);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *p = strstr(months, mon);
    struct tm t = {
        .tm_year = yy - 1900,
        .tm_mon = p ? (int)((p - months) / 3) : 0,
        .tm_mday = dd,
        .tm_hour = hh,
        .tm_min = mm,
        .tm_sec = ss,
        .tm_isdst = 0,
    };
    setenv("TZ", "UTC0", 1);
    tzset();
    return mktime(&t);
}

void watch_time_init(void)
{
    time_t build = build_epoch_utc();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < build) {
        tv.tv_sec = build;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "clock was unset; seeded from build time (UTC epoch %lld)", (long long)build);
    }
    setenv("TZ", WATCH_DEFAULT_TZ, 1);
    tzset();
}

void watch_time_now(struct tm *out, int *ms)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t s = tv.tv_sec;
    localtime_r(&s, out);
    if (ms) {
        *ms = (int)(tv.tv_usec / 1000);
    }
}
