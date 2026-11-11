/*
 * Settings menu drawn over the face.
 *
 *   Settings ─┬─ Wi-Fi ─────┬─ status block (radio, saved network, last sync) - live
 *             │             ├─ Choose network ─ scan list ─ password keyboard
 *             │             ├─ Set time now
 *             │             ├─ Open Wi-Fi: On/Off
 *             │             └─ Forget network
 *             ├─ Display ───┬─ Brightness slider (live, saved on release)
 *             │             └─ Screen on: 5/10/20/30 s
 *             ├─ Theme
 *             └─ Close
 *
 * Pages are rebuilt through lv_async_call so a button is never deleted from inside its own
 * callback. Scan/join/sync run on a worker task; results return to the LVGL thread through
 * lv_async_call under the display lock. A 1 s LVGL timer refreshes the Wi-Fi status text.
 * Content stays inside a ~320 px column so it fits the round 466 px panel.
 */
#include "settings_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "axp.h"
#include "battlog.h"
#include "version.h"
#include "bsp/esp-bsp.h"
#include "faces.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "settings_ui";

#define COL_BG      0x0B0A08
#define COL_ITEM    0x24201A
#define COL_ITEM_PR 0x3A3428
#define COL_TEXT    0xEFE8D6
#define COL_DIM     0xA89F88
#define COL_BRASS   0xC9A04A
#define COL_GOOD    0x7FB069
#define COL_BAD     0xD9734E
#define MAX_APS     20

typedef enum { PAGE_MAIN, PAGE_WIFI, PAGE_NETWORKS, PAGE_PASSWORD, PAGE_DISPLAY, PAGE_SYSTEM } page_t;
typedef enum { JOB_SCAN, JOB_JOIN, JOB_SYNC } job_type_t;

typedef struct {
    job_type_t type;
    char ssid[33];
    char pass[65];
} job_t;

typedef struct {
    job_type_t type;
    net_result_t result;
    int count;
    net_ap_t aps[MAX_APS];
} job_result_t;

static lv_obj_t *s_panel;           /* NULL when closed */
static lv_obj_t *s_status;          /* bottom status line */
static lv_obj_t *s_list;
static lv_obj_t *s_ta;
static lv_obj_t *s_wifi_info;       /* live status block on the Wi-Fi page */
static lv_obj_t *s_wifi_sub;        /* second line of the Wi-Fi item on the main page */
static lv_obj_t *s_bright_lbl;
static lv_timer_t *s_refresh;
static page_t s_page;
static QueueHandle_t s_jobs;
static volatile bool s_busy;
static net_ap_t s_aps[MAX_APS];
static int s_ap_count;
static char s_sel_ssid[33];
static char s_note[64];             /* one-shot message for the next page build */
static bool s_reset_armed;          /* factory reset needs a second tap */

static const int AWAKE_CHOICES[] = {5, 10, 20, 30, 0};   /* 0 = always on */
#define AWAKE_COUNT ((int)(sizeof(AWAKE_CHOICES) / sizeof(AWAKE_CHOICES[0])))

static void go(page_t page);

static void forget_panel(void)
{
    if (s_refresh) {
        lv_timer_delete(s_refresh);
        s_refresh = NULL;
    }
    s_panel = NULL;
    s_status = NULL;
    s_list = NULL;
    s_ta = NULL;
    s_wifi_info = NULL;
    s_wifi_sub = NULL;
    s_bright_lbl = NULL;
}

/* ---------------------------------------------------------------- status text */

static void fmt_clock(long long epoch, char *buf, size_t len)
{
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    int h = tm.tm_hour % 12;
    snprintf(buf, len, "%d:%02d %s", h ? h : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

/* One-line summary for the main menu. */
static void wifi_summary(char *buf, size_t len, uint32_t *color)
{
    net_status_t st;
    net_get_status(&st);
    char ssid[33];
    bool saved = settings_get_str("ssid", ssid, sizeof(ssid));
    *color = COL_DIM;
    if (st.radio == NET_RADIO_SCANNING) {
        snprintf(buf, len, "Scanning...");
    } else if (st.radio != NET_RADIO_OFF) {
        snprintf(buf, len, "%s %s", st.radio == NET_RADIO_CONNECTED ? "Connected:" : "Joining", st.radio_ssid);
        *color = COL_BRASS;
    } else if (st.last_sync) {
        char t[16];
        fmt_clock(st.last_sync, t, sizeof(t));
        snprintf(buf, len, "Synced %s", t);
        *color = COL_GOOD;
    } else if (st.have_result) {
        snprintf(buf, len, "Last sync failed");
        *color = COL_BAD;
    } else if (saved) {
        snprintf(buf, len, "%s, not synced yet", ssid);
    } else {
        snprintf(buf, len, "Not set up");
        *color = COL_BAD;
    }
}

/* Multi-line block for the Wi-Fi page. */
static void wifi_details(char *buf, size_t len)
{
    net_status_t st;
    net_get_status(&st);
    char ssid[33], line[80];
    size_t o = 0;

    switch (st.radio) {
    case NET_RADIO_OFF:        snprintf(line, sizeof(line), "Radio: off"); break;
    case NET_RADIO_SCANNING:   snprintf(line, sizeof(line), "Radio: scanning"); break;
    case NET_RADIO_CONNECTING: snprintf(line, sizeof(line), "Radio: joining %s", st.radio_ssid); break;
    case NET_RADIO_CONNECTED:  snprintf(line, sizeof(line), "Radio: connected to %s", st.radio_ssid); break;
    }
    o += snprintf(buf + o, len - o, "%s\n", line);

    if (settings_get_str("ssid", ssid, sizeof(ssid))) {
        o += snprintf(buf + o, len - o, "Saved: %s\n", ssid);
    } else {
        o += snprintf(buf + o, len - o, "Saved: none\n");
    }

    char t[16];
    if (st.last_sync) {
        fmt_clock(st.last_sync, t, sizeof(t));
        o += snprintf(buf + o, len - o, "Clock synced %s", t);
        if (st.have_result && st.last_attempt != st.last_sync) {
            fmt_clock(st.last_attempt, t, sizeof(t));
            o += snprintf(buf + o, len - o, "\nLast try %s: %s", t, net_result_text(st.last_result));
        } else if (st.last_ssid[0]) {
            o += snprintf(buf + o, len - o, " via %s", st.last_ssid);
        }
    } else if (st.have_result) {
        fmt_clock(st.last_attempt, t, sizeof(t));
        o += snprintf(buf + o, len - o, "Last try %s: %s", t, net_result_text(st.last_result));
    } else {
        o += snprintf(buf + o, len - o, "Clock not synced since power-on");
    }
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    char buf[200];
    if (s_wifi_info) {
        wifi_details(buf, sizeof(buf));
        lv_label_set_text(s_wifi_info, buf);
    }
    if (s_wifi_sub) {
        uint32_t c;
        wifi_summary(buf, sizeof(buf), &c);
        lv_label_set_text(s_wifi_sub, buf);
        lv_obj_set_style_text_color(s_wifi_sub, lv_color_hex(c), 0);
    }
}

/* ---------------------------------------------------------------- widgets */

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, txt);
    return l;
}

static void set_status(const char *txt)
{
    if (s_status) {
        lv_label_set_text(s_status, txt);
    }
}

static lv_obj_t *button(lv_obj_t *parent, int h, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_ITEM), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_ITEM_PR), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

static lv_obj_t *item(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = button(parent, 58, cb, user);
    lv_obj_t *l = label(b, txt, &lv_font_montserrat_28, COL_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_center(l);
    return b;
}

/* Two-line item: title plus a smaller coloured line; returns the second label. */
static lv_obj_t *item2(lv_obj_t *parent, const char *title, const char *sub, uint32_t sub_color,
                       lv_event_cb_t cb)
{
    lv_obj_t *b = button(parent, 74, cb, NULL);
    lv_obj_t *t = label(b, title, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *s = label(b, sub, &lv_font_montserrat_16, sub_color);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -8);
    return s;
}

/* Title at the top, scrolling column in the middle, status line under it. */
static void frame(const char *title, int list_h)
{
    lv_obj_clean(s_panel);
    s_status = NULL;
    s_list = NULL;
    s_ta = NULL;
    s_wifi_info = NULL;
    s_wifi_sub = NULL;
    s_bright_lbl = NULL;

    lv_obj_t *t = label(s_panel, title, &lv_font_montserrat_28, COL_BRASS);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, 300);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 44);

    if (list_h > 0) {
        s_list = lv_obj_create(s_panel);
        lv_obj_remove_style_all(s_list);
        lv_obj_set_size(s_list, 320, list_h);
        lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 88);
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_list, 10, 0);
        lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    }

    s_status = label(s_panel, s_note, &lv_font_montserrat_16, COL_DIM);
    s_note[0] = '\0';
    lv_obj_set_width(s_status, 260);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -34);
}

/* ---------------------------------------------------------------- worker */

static void post_job(job_type_t type, const char *ssid, const char *pass)
{
    job_t j = {.type = type};
    if (ssid) {
        strlcpy(j.ssid, ssid, sizeof(j.ssid));
    }
    if (pass) {
        strlcpy(j.pass, pass, sizeof(j.pass));
    }
    s_busy = true;
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        s_busy = false;
        set_status("Busy, try again");
    }
}

static void apply_result(void *arg)                     /* LVGL thread */
{
    job_result_t *r = arg;
    s_busy = false;
    if (!s_panel) {
        free(r);
        return;
    }
    switch (r->type) {
    case JOB_SCAN:
        if (s_page == PAGE_NETWORKS) {
            s_ap_count = r->count;
            memcpy(s_aps, r->aps, sizeof(net_ap_t) * r->count);
            go(PAGE_NETWORKS);                              /* rebuild with the list */
        }
        break;
    case JOB_JOIN:
        if (r->result == NET_OK || r->result == NET_NTP_FAILED) {
            snprintf(s_note, sizeof(s_note), "%s",
                     r->result == NET_OK ? "Wi-Fi saved, time synced" : "Wi-Fi saved, time server failed");
            go(PAGE_WIFI);
        } else {
            set_status("Could not join. Check the password.");
        }
        break;
    case JOB_SYNC:
        set_status(net_result_text(r->result));
        break;
    }
    refresh_cb(NULL);
    free(r);
}

static void worker(void *arg)
{
    job_t j;
    while (1) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        job_result_t *r = calloc(1, sizeof(*r));
        if (!r) {
            s_busy = false;
            continue;
        }
        r->type = j.type;
        switch (j.type) {
        case JOB_SCAN:
            r->count = net_scan(r->aps, MAX_APS);
            break;
        case JOB_JOIN:
            r->result = net_provision(j.ssid, j.pass);
            break;
        case JOB_SYNC:
            r->result = net_sync_time();
            break;
        }
        ESP_LOGI(TAG, "job %d done", j.type);
        if (bsp_display_lock(2000) == ESP_OK) {
            lv_async_call(apply_result, r);
            bsp_display_unlock();
        } else {
            s_busy = false;
            free(r);
        }
    }
}

/* ---------------------------------------------------------------- callbacks */

static void on_close(lv_event_t *e)
{
    (void)e;
    if (s_panel) {
        lv_obj_delete_async(s_panel);
        forget_panel();
    }
}

static void on_to_main(lv_event_t *e)     { (void)e; go(PAGE_MAIN); }
static void on_to_wifi(lv_event_t *e)     { (void)e; go(PAGE_WIFI); }
static void on_to_display(lv_event_t *e)  { (void)e; go(PAGE_DISPLAY); }
static void on_to_system(lv_event_t *e)   { (void)e; s_reset_armed = false; go(PAGE_SYSTEM); }

static void on_restart(lv_event_t *e)
{
    (void)e;
    ESP_LOGW(TAG, "restart from Settings");
    bsp_display_brightness_set(0);
    esp_restart();
}

static void on_dump(lv_event_t *e)
{
    (void)e;
    set_status("Sending battery log over USB...");
    battlog_dump();
    set_status("Battery log sent (lines start with BATTLOG)");
}

static void on_factory_reset(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (!s_reset_armed) {
        s_reset_armed = true;
        lv_obj_t *l = lv_obj_get_child(btn, 0);
        if (l) {
            lv_label_set_text(l, "Tap again to erase");
        }
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x5A2A1E), 0);
        set_status("Erases Wi-Fi, display settings and face, then restarts");
        return;
    }
    bsp_display_brightness_set(0);
    settings_factory_reset();
}
static void on_to_networks(lv_event_t *e) { (void)e; s_ap_count = -1; go(PAGE_NETWORKS); }

static void on_sync(lv_event_t *e)
{
    (void)e;
    if (s_busy) {
        return;
    }
    set_status("Syncing time...");
    post_job(JOB_SYNC, NULL, NULL);
}

static void on_open_wifi(lv_event_t *e)
{
    (void)e;
    settings_set_int("open_wifi", !settings_get_int("open_wifi", 1));
    go(PAGE_WIFI);
}

static void on_forget(lv_event_t *e)
{
    (void)e;
    net_forget();
    snprintf(s_note, sizeof(s_note), "Saved network removed");
    go(PAGE_WIFI);
}

static void on_ap(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ap_count || s_busy) {
        return;
    }
    strlcpy(s_sel_ssid, s_aps[i].ssid, sizeof(s_sel_ssid));
    if (s_aps[i].secured) {
        go(PAGE_PASSWORD);
    } else {
        set_status("Joining...");
        post_job(JOB_JOIN, s_sel_ssid, "");
    }
}

static void on_keyboard(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) {
        s_ap_count = -1;
        go(PAGE_NETWORKS);
    } else if (code == LV_EVENT_READY && s_ta && !s_busy) {
        set_status("Joining...");
        post_job(JOB_JOIN, s_sel_ssid, lv_textarea_get_text(s_ta));
    }
}

static void on_brightness(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target_obj(e);
    int v = (int)lv_slider_get_value(sl);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bsp_display_brightness_set(v);                  /* live preview */
        if (s_bright_lbl) {
            lv_label_set_text_fmt(s_bright_lbl, "Brightness %d%%", v);
        }
    } else {                                            /* released: save once */
        settings_set_int("bright", v);
    }
}

static void on_batt_ring(lv_event_t *e)
{
    (void)e;
    settings_set_int("batt_ring", !settings_get_int("batt_ring", 1));
    faces_battery_ring_changed_nolock();
    go(PAGE_DISPLAY);
}

static void on_chg_on(lv_event_t *e)
{
    (void)e;
    settings_set_int("chg_on", !settings_get_int("chg_on", 1));
    go(PAGE_DISPLAY);
}

static void on_awake(lv_event_t *e)
{
    (void)e;
    int cur = settings_get_int("awake_s", CONFIG_WATCH_AWAKE_MS / 1000);
    int i = 0;
    while (i < AWAKE_COUNT && AWAKE_CHOICES[i] != cur) {
        i++;
    }
    settings_set_int("awake_s", AWAKE_CHOICES[(i + 1) % AWAKE_COUNT]);
    go(PAGE_DISPLAY);
}

/* ---------------------------------------------------------------- pages */

static void build_main(void)
{
    frame("Settings", 320);
    char sum[64], txt[48];
    uint32_t c;
    wifi_summary(sum, sizeof(sum), &c);
    s_wifi_sub = item2(s_list, "Wi-Fi", sum, c, on_to_wifi);
    snprintf(txt, sizeof(txt), "Brightness %d%%", settings_get_int("bright", CONFIG_WATCH_BRIGHTNESS));
    item2(s_list, "Display", txt, COL_DIM, on_to_display);
    snprintf(txt, sizeof(txt), "v%s", WATCH_VERSION);
    item2(s_list, "System", txt, COL_DIM, on_to_system);
    item(s_list, "Close", on_close, NULL);
}

static void build_wifi(void)
{
    frame("Wi-Fi", 320);
    char buf[200];
    wifi_details(buf, sizeof(buf));
    s_wifi_info = label(s_list, buf, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(s_wifi_info, lv_pct(100));
    lv_label_set_long_mode(s_wifi_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_ver(s_wifi_info, 4, 0);

    item(s_list, "Choose network", on_to_networks, NULL);
    item(s_list, "Set time now", on_sync, NULL);
    char txt[40];
    snprintf(txt, sizeof(txt), "Use open Wi-Fi: %s", settings_get_int("open_wifi", 1) ? "On" : "Off");
    item(s_list, txt, on_open_wifi, NULL);
    char ssid[33];
    if (settings_get_str("ssid", ssid, sizeof(ssid))) {
        item(s_list, "Forget network", on_forget, NULL);
    }
    item(s_list, "Back", on_to_main, NULL);
}

static void build_networks(void)
{
    frame("Networks", 320);
    if (s_ap_count < 0) {                               /* start a scan */
        set_status("Scanning...");
        item(s_list, "Back", on_to_wifi, NULL);
        post_job(JOB_SCAN, NULL, NULL);
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        char txt[48];
        snprintf(txt, sizeof(txt), "%s%s", s_aps[i].ssid, s_aps[i].secured ? "" : "  (open)");
        item(s_list, txt, on_ap, (void *)(intptr_t)i);
    }
    item(s_list, "Scan again", on_to_networks, NULL);
    item(s_list, "Back", on_to_wifi, NULL);
    set_status(s_ap_count ? "Tap a network" : "No networks found");
}

static void build_password(void)
{
    frame(s_sel_ssid, 0);

    s_ta = lv_textarea_create(s_panel);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_password_mode(s_ta, true);
    lv_textarea_set_max_length(s_ta, 64);
    lv_textarea_set_placeholder_text(s_ta, "Password");
    lv_obj_set_width(s_ta, 300);
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_28, 0);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 96);

    lv_obj_t *kb = lv_keyboard_create(s_panel);
    lv_keyboard_set_textarea(kb, s_ta);
    lv_keyboard_set_popovers(kb, true);
    lv_obj_set_size(kb, 316, 196);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_add_event_cb(kb, on_keyboard, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_keyboard, LV_EVENT_CANCEL, NULL);

    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -52);
    set_status("OK key joins, keyboard key goes back");
}

static void build_display(void)
{
    frame("Display", 320);
    int b = settings_get_int("bright", CONFIG_WATCH_BRIGHTNESS);

    s_bright_lbl = label(s_list, "", &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_width(s_bright_lbl, lv_pct(100));
    lv_label_set_text_fmt(s_bright_lbl, "Brightness %d%%", b);

    lv_obj_t *holder = lv_obj_create(s_list);           /* padding so the knob is easy to grab */
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, lv_pct(100), 56);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sl = lv_slider_create(holder);
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, b, LV_ANIM_OFF);
    lv_obj_set_width(sl, 260);
    lv_obj_center(sl);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_BRASS), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_BRASS), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, on_brightness, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, on_brightness, LV_EVENT_RELEASED, NULL);

    char txt[32];
    int awake = settings_get_int("awake_s", CONFIG_WATCH_AWAKE_MS / 1000);
    if (awake > 0) {
        snprintf(txt, sizeof(txt), "Screen on: %d s", awake);
    } else {
        snprintf(txt, sizeof(txt), "Screen on: always");
    }
    item(s_list, txt, on_awake, NULL);
    snprintf(txt, sizeof(txt), "On while charging: %s", settings_get_int("chg_on", 1) ? "On" : "Off");
    item(s_list, txt, on_chg_on, NULL);
    snprintf(txt, sizeof(txt), "Battery: %s", settings_get_int("batt_ring", 1) ? "On" : "Off");
    item(s_list, txt, on_batt_ring, NULL);
    item(s_list, "Back", on_to_main, NULL);
}

static void build_system(void)
{
    frame("System", 320);
    char buf[420];
    int o = snprintf(buf, sizeof(buf), "%s %s\n", WATCH_NAME, WATCH_VERSION);
    battlog_summary(buf + o, sizeof(buf) - o);
    lv_obj_t *info = label(s_list, buf, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(info, lv_pct(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_MODE_WRAP);
    item(s_list, "Send log over USB", on_dump, NULL);
    item(s_list, "Restart", on_restart, NULL);
    item(s_list, "Factory reset", on_factory_reset, NULL);
    item(s_list, "Back", on_to_main, NULL);
    set_status("Frozen? Unplug USB, hold PWR 6 s to power off, then press PWR.");
}

static void build_cb(void *arg)
{
    if (!s_panel) {
        return;
    }
    s_page = (page_t)(intptr_t)arg;
    switch (s_page) {
    case PAGE_MAIN:     build_main(); break;
    case PAGE_WIFI:     build_wifi(); break;
    case PAGE_NETWORKS: build_networks(); break;
    case PAGE_PASSWORD: build_password(); break;
    case PAGE_DISPLAY:  build_display(); break;
    case PAGE_SYSTEM:   build_system(); break;
    }
}

static void go(page_t page)
{
    lv_async_call(build_cb, (void *)(intptr_t)page);
}

/* ---------------------------------------------------------------- public */

void settings_ui_init(void)
{
    s_jobs = xQueueCreate(2, sizeof(job_t));
    xTaskCreate(worker, "settings_net", 8192, NULL, 3, NULL);
}

void settings_ui_open_nolock(void)
{
    if (s_panel) {
        return;
    }
    s_panel = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(COL_BG), 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_panel);
    s_note[0] = '\0';
    s_page = PAGE_MAIN;
    build_main();
    s_refresh = lv_timer_create(refresh_cb, 1000, NULL);
}

void settings_ui_toggle(void)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        return;
    }
    if (s_panel) {
        lv_obj_delete(s_panel);
        forget_panel();
    } else {
        settings_ui_open_nolock();
    }
    bsp_display_unlock();
}

void settings_ui_close(void)
{
    if (!s_panel || bsp_display_lock(1000) != ESP_OK) {
        return;
    }
    if (s_panel) {
        lv_obj_delete(s_panel);
        forget_panel();
    }
    bsp_display_unlock();
}

bool settings_ui_is_open(void) { return s_panel != NULL; }
bool settings_ui_busy(void)    { return s_busy; }
