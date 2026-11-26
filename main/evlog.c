#include "evlog.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define EV_COUNT 96
#define EV_MAGIC 0x45564C31u

typedef struct {
    uint32_t ms;            /* esp_timer, ms since boot (wraps after 49 days) */
    uint32_t utc;           /* libc mirror of the clock, s (0 if not set) */
    char what[8];
    char tag[8];
    int32_t a, b;
} ev_t;

static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t next;          /* total events written */
    uint32_t boots;
    ev_t ev[EV_COUNT];
} s_log;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_checked;

static void check(void)
{
    if (!s_checked) {
        if (s_log.magic != EV_MAGIC) {
            memset(&s_log, 0, sizeof(s_log));
            s_log.magic = EV_MAGIC;
        }
        s_log.boots++;
        s_checked = true;
    }
}

void evlog_add(const char *what, const char *tag, int32_t a, int32_t b)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ev_t e = {
        .ms = (uint32_t)(esp_timer_get_time() / 1000),
        .utc = tv.tv_sec > 1704067200 ? (uint32_t)tv.tv_sec : 0,
        .a = a,
        .b = b,
    };
    strlcpy(e.what, what ? what : "", sizeof(e.what));
    strlcpy(e.tag, tag ? tag : "", sizeof(e.tag));
    taskENTER_CRITICAL(&s_mux);
    check();
    s_log.ev[s_log.next % EV_COUNT] = e;
    s_log.next++;
    taskEXIT_CRITICAL(&s_mux);
}

void evlog_dump(void)
{
    taskENTER_CRITICAL(&s_mux);
    check();
    uint32_t next = s_log.next;
    taskEXIT_CRITICAL(&s_mux);
    uint32_t n = next < EV_COUNT ? next : EV_COUNT;
    printf("EVLOG,seq,ms_since_boot,utc,event,tag,a,b  (boots seen %lu)\n", (unsigned long)s_log.boots);
    for (uint32_t i = next - n; i < next; i++) {
        ev_t e;
        taskENTER_CRITICAL(&s_mux);
        e = s_log.ev[i % EV_COUNT];
        taskEXIT_CRITICAL(&s_mux);
        printf("EVLOG,%lu,%lu,%lu,%s,%s,%ld,%ld\n", (unsigned long)i, (unsigned long)e.ms, (unsigned long)e.utc,
               e.what, e.tag, (long)e.a, (long)e.b);
    }
    printf("EVLOG,end\n");
    fflush(stdout);
}
