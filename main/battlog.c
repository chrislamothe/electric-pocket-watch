#include "battlog.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "axp.h"

static const char *TAG = "battlog";

#define PERIOD_US    ((int64_t)BATTLOG_PERIOD_MIN * 60 * 1000000)
#define REC_SIZE     32
#define SECTOR       4096
#define VALID_EPOCH  1704067200u                /* 2024-01-01: anything earlier is an unset clock */
#define FLAG_USB     0x01
static const uint8_t MAGIC[4] = {'P', 'W', 'B', 'L'};

static bool valid(const rec_t *r)
{
    return r->seq != 0xFFFFFFFF && memcmp(r->magic, MAGIC, sizeof(MAGIC)) == 0;
}

typedef struct __attribute__((packed)) {
    uint32_t seq;                               /* 0xFFFFFFFF = empty slot */
    uint32_t epoch;                             /* 0 if the clock was not set */
    uint32_t uptime_s;
    uint16_t boot;
    uint16_t mv;                                /* 0 = no reading */
    int8_t gauge;
    uint8_t flags;                              /* bit0 USB, bits[3:1] charge state */
    uint16_t wakes_button, wakes_tap, wakes_motion;
    uint8_t magic[4];                           /* "PWBL": rejects leftover factory/garbage data */
    uint8_t pad[4];
} rec_t;
_Static_assert(sizeof(rec_t) == REC_SIZE, "record size");

static const esp_partition_t *s_part;
static uint32_t s_slots, s_head, s_seq;         /* next slot to write, next sequence number */
static uint16_t s_boot;
static int64_t s_last_us = -PERIOD_US;
static uint16_t s_wakes[3];

/* ---------------------------------------------------------------- storage */

static bool rd(uint32_t slot, rec_t *r)
{
    return esp_partition_read(s_part, slot * REC_SIZE, r, REC_SIZE) == ESP_OK;
}

void battlog_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "battlog");
    if (!s_part) {
        ESP_LOGE(TAG, "no battlog partition");
        return;
    }
    s_slots = s_part->size / REC_SIZE;
    uint32_t best_seq = 0, best_slot = 0;
    bool any = false;
    rec_t r;
    for (uint32_t i = 0; i < s_slots; i++) {
        if (rd(i, &r) && valid(&r) && (!any || r.seq > best_seq)) {
            any = true;
            best_seq = r.seq;
            best_slot = i;
            s_boot = r.boot;
        }
    }
    s_head = any ? (best_slot + 1) % s_slots : 0;
    s_seq = any ? best_seq + 1 : 0;
    s_boot = any ? (uint16_t)(s_boot + 1) : 0;
    ESP_LOGI(TAG, "%lu slots, %s, boot #%u", (unsigned long)s_slots,
             any ? "resuming" : "empty", s_boot);
}

static void append(const rec_t *r)
{
    if (!s_part) {
        return;
    }
    uint32_t off = s_head * REC_SIZE;
    if (off % SECTOR == 0) {
        esp_partition_erase_range(s_part, off, SECTOR);  /* oldest 128 records go */
    }
    esp_partition_write(s_part, off, r, REC_SIZE);
    s_head = (s_head + 1) % s_slots;
}

/* Visit records oldest first. The sector after the head may be erased or hold the oldest data. */
typedef void (*visit_fn)(const rec_t *r, void *ctx);

static void for_each(visit_fn fn, void *ctx)
{
    if (!s_part) {
        return;
    }
    rec_t r;
    for (uint32_t k = 0; k < s_slots; k++) {
        uint32_t i = (s_head + k) % s_slots;
        if (rd(i, &r) && valid(&r)) {
            fn(&r, ctx);
        }
    }
}

/* ---------------------------------------------------------------- sampling */

void battlog_note_wake(battlog_wake_t w)
{
    if (s_wakes[w] < 0xFFFF) {
        s_wakes[w]++;
    }
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
    struct timeval tv;
    gettimeofday(&tv, NULL);
    bool usb = axp_vbus_good();
    int mv = axp_battery_mv();
    rec_t r;
    memset(&r, 0, sizeof(r));
    r.seq = s_seq++;
    memcpy(r.magic, MAGIC, sizeof(MAGIC));
    r.epoch = (uint32_t)tv.tv_sec >= VALID_EPOCH ? (uint32_t)tv.tv_sec : 0;
    r.uptime_s = (uint32_t)(now / 1000000);
    r.boot = s_boot;
    r.mv = mv > 0 ? (uint16_t)mv : 0;
    r.gauge = (int8_t)axp_gauge_percent();
    r.flags = (usb ? FLAG_USB : 0) | (uint8_t)((axp_charge_state() & 3) << 1);
    r.wakes_button = s_wakes[0];
    r.wakes_tap = s_wakes[1];
    r.wakes_motion = s_wakes[2];
    append(&r);
    ESP_LOGI(TAG, "[%s] #%lu boot %u battery %d mV (%d%%), gauge %d%%, %s, wakes %u/%u/%u", why,
             (unsigned long)r.seq, r.boot, mv, axp_mv_to_percent(mv), r.gauge, usb ? "USB" : "battery",
             r.wakes_button, r.wakes_tap, r.wakes_motion);
}

/* ---------------------------------------------------------------- export */

static void dump_one(const rec_t *r, void *ctx)
{
    (void)ctx;
    printf("BATTLOG,%lu,%u,%lu,%lu,%u,%d,%u,%u,%u,%u,%u\n", (unsigned long)r->seq, r->boot,
           (unsigned long)r->epoch, (unsigned long)r->uptime_s, r->mv, r->gauge, r->flags & FLAG_USB,
           (r->flags >> 1) & 3, r->wakes_button, r->wakes_tap, r->wakes_motion);
}

void battlog_dump(void)
{
    printf("BATTLOG,seq,boot,epoch,uptime_s,mv,gauge,usb,charge,wakes_button,wakes_tap,wakes_motion\n");
    for_each(dump_one, NULL);
    printf("BATTLOG,end,shutdown_mv=%d,charge_target_mv=%d\n", axp_shutdown_mv(), axp_charge_target_mv());
    fflush(stdout);
}

typedef struct {
    int32_t *out;
    int max, n;
} recent_t;

static void collect(const rec_t *r, void *ctx)
{
    recent_t *c = ctx;
    if (!r->mv) {
        return;
    }
    if (c->n == c->max) {                       /* keep the most recent max values */
        memmove(c->out, c->out + 1, (c->max - 1) * sizeof(int32_t));
        c->n--;
    }
    c->out[c->n++] = r->mv;
}

int battlog_recent_mv(int32_t *out, int max)
{
    recent_t c = {out, max, 0};
    for_each(collect, &c);
    return c.n;
}

/* ---------------------------------------------------------------- summary
 * Finds the most recent discharge run: from the first battery sample after USB power to the
 * last battery sample before USB came back (or to now). Duration from the wall clock when both
 * ends have it, otherwise from uptime when both ends are in the same boot. */
typedef struct {
    bool in_run;
    rec_t start, last;
    bool have_done;
    rec_t done_start, done_end;
} run_t;

static void track(const rec_t *r, void *ctx)
{
    run_t *t = ctx;
    bool usb = r->flags & FLAG_USB;
    if (!usb && r->mv) {
        if (!t->in_run) {
            t->in_run = true;
            t->start = *r;
        }
        t->last = *r;
    } else if (usb && t->in_run) {
        t->in_run = false;
        t->have_done = true;
        t->done_start = t->start;
        t->done_end = t->last;
    }
}

static float hours_between(const rec_t *a, const rec_t *b)
{
    if (a->epoch && b->epoch && b->epoch >= a->epoch) {
        return (b->epoch - a->epoch) / 3600.0f;
    }
    if (a->boot == b->boot && b->uptime_s >= a->uptime_s) {
        return (b->uptime_s - a->uptime_s) / 3600.0f;
    }
    return -1;
}

static size_t describe(char *buf, size_t len, const char *what, const rec_t *a, const rec_t *b)
{
    float h = hours_between(a, b);
    int used = axp_mv_to_percent(a->mv) - axp_mv_to_percent(b->mv);
    if (h < 0) {
        return snprintf(buf, len, "%s: %d.%02d -> %d.%02d V (time unknown)\n", what, a->mv / 1000,
                        (a->mv % 1000) / 10, b->mv / 1000, (b->mv % 1000) / 10);
    }
    return snprintf(buf, len, "%s: %.1f h, %d.%02d -> %d.%02d V, %d%%%s\n", what, h, a->mv / 1000,
                    (a->mv % 1000) / 10, b->mv / 1000, (b->mv % 1000) / 10, used,
                    h >= 0.5f ? "" : " (measuring)");
}

void battlog_summary(char *buf, size_t len)
{
    int mv = axp_battery_mv();
    int g = axp_gauge_percent();
    bool usb = axp_vbus_good();
    static const char *const CHG[] = {"idle", "charging", "discharging", "?"};
    size_t o = 0;
    if (mv > 0) {
        o += snprintf(buf + o, len - o, "Battery %d.%02d V = %d%% (gauge %d%%)\n%s, %s\n", mv / 1000,
                      (mv % 1000) / 10, axp_mv_to_percent(mv), g, usb ? "USB power" : "on battery",
                      CHG[axp_charge_state() & 3]);
    } else {
        o += snprintf(buf + o, len - o, "No battery reading\n");
    }
    run_t t = {0};
    for_each(track, &t);
    if (t.in_run && o < len) {
        o += describe(buf + o, len - o, "This run", &t.start, &t.last);
    }
    if (t.have_done && o < len) {
        o += describe(buf + o, len - o, "Last run", &t.done_start, &t.done_end);
    }
    if (o < len) {
        snprintf(buf + o, len - o, "Wakes this boot: %u button, %u tap, %u motion only\nPower cut at %d mV",
                 s_wakes[0], s_wakes[1], s_wakes[2], axp_shutdown_mv());
    }
}
