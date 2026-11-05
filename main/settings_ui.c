/*
 * Settings menu drawn over the face. Pages are rebuilt through lv_async_call so a button is never
 * deleted from inside its own click callback. Network work (scan, join, sync) runs on a worker
 * task; results come back to the LVGL thread through lv_async_call under the display lock.
 *
 * Layout respects the round 466 px panel: content stays inside a ~320 px wide column, and the
 * keyboard is placed where the circle is still wide enough for it.
 */
#include "settings_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
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
#define MAX_APS     20

typedef enum { PAGE_MAIN, PAGE_WIFI, PAGE_PASSWORD } page_t;
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
static lv_obj_t *s_status;
static lv_obj_t *s_list;
static lv_obj_t *s_ta;
static page_t s_page;
static QueueHandle_t s_jobs;
static volatile bool s_busy;
static net_ap_t s_aps[MAX_APS];
static int s_ap_count;
static char s_sel_ssid[33];
static char s_main_note[48];

static const char *const THEME_NAMES[WATCH_THEME_COUNT] = {"Brass", "Silver", "Dark"};

static void go(page_t page);

static void forget_panel(void)
{
    s_panel = NULL;
    s_status = NULL;
    s_list = NULL;
    s_ta = NULL;
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

static lv_obj_t *item(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), 58);
    lv_obj_set_style_radius(b, 29, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_ITEM), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_ITEM_PR), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = label(b, txt, &lv_font_montserrat_28, COL_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_center(l);
    return b;
}

/* Title at the top, scrolling column of items in the middle, status line under it. */
static void frame(const char *title, int list_h)
{
    lv_obj_clean(s_panel);
    s_status = NULL;
    s_list = NULL;
    s_ta = NULL;

    lv_obj_t *t = label(s_panel, title, &lv_font_montserrat_28, COL_BRASS);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, 300);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 48);

    if (list_h > 0) {
        s_list = lv_obj_create(s_panel);
        lv_obj_remove_style_all(s_list);
        lv_obj_set_size(s_list, 320, list_h);
        lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 96);
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_list, 10, 0);
        lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    }

    s_status = label(s_panel, "", &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(s_status, 280);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -40);
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
        if (s_page == PAGE_WIFI && s_list) {
            s_ap_count = r->count;
            memcpy(s_aps, r->aps, sizeof(net_ap_t) * r->count);
            go(PAGE_WIFI);                                  /* rebuild with the list */
        }
        break;
    case JOB_JOIN:
        if (r->result == NET_OK || r->result == NET_NTP_FAILED) {
            snprintf(s_main_note, sizeof(s_main_note), "%s",
                     r->result == NET_OK ? "Wi-Fi saved, time synced" : "Wi-Fi saved");
            go(PAGE_MAIN);
        } else {
            set_status("Could not join. Check the password.");
        }
        break;
    case JOB_SYNC:
        set_status(net_result_text(r->result));
        break;
    }
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

/* ---------------------------------------------------------------- pages */

static void on_close(lv_event_t *e)
{
    (void)e;
    if (s_panel) {
        lv_obj_delete_async(s_panel);
        forget_panel();
    }
}

static void on_wifi(lv_event_t *e)   { (void)e; s_ap_count = -1; go(PAGE_WIFI); }
static void on_back(lv_event_t *e)   { (void)e; s_main_note[0] = '\0'; go(PAGE_MAIN); }
static void on_rescan(lv_event_t *e) { (void)e; s_ap_count = -1; go(PAGE_WIFI); }

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
    s_main_note[0] = '\0';
    go(PAGE_MAIN);
}

static void on_theme(lv_event_t *e)
{
    (void)e;
    int t = (faces_theme() + 1) % WATCH_THEME_COUNT;
    faces_set_theme_nolock(t);
    settings_set_int("theme", t);
    s_main_note[0] = '\0';
    go(PAGE_MAIN);
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
        go(PAGE_WIFI);
    } else if (code == LV_EVENT_READY && s_ta && !s_busy) {
        set_status("Joining...");
        post_job(JOB_JOIN, s_sel_ssid, lv_textarea_get_text(s_ta));
    }
}

static void build_main(void)
{
    frame("Settings", 300);
    char ssid[33], txt[64];
    if (settings_get_str("ssid", ssid, sizeof(ssid))) {
        snprintf(txt, sizeof(txt), "Wi-Fi: %s", ssid);
    } else {
        snprintf(txt, sizeof(txt), "Wi-Fi: not set");
    }
    item(s_list, txt, on_wifi, NULL);
    item(s_list, "Set time now", on_sync, NULL);
    snprintf(txt, sizeof(txt), "Open Wi-Fi: %s", settings_get_int("open_wifi", 1) ? "On" : "Off");
    item(s_list, txt, on_open_wifi, NULL);
    snprintf(txt, sizeof(txt), "Theme: %s", THEME_NAMES[faces_theme()]);
    item(s_list, txt, on_theme, NULL);
    item(s_list, "Close", on_close, NULL);
    set_status(s_main_note);
}

static void build_wifi(void)
{
    frame("Wi-Fi", 300);
    if (s_ap_count < 0) {                               /* start a scan */
        set_status("Scanning...");
        item(s_list, "Back", on_back, NULL);
        post_job(JOB_SCAN, NULL, NULL);
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        char txt[48];
        snprintf(txt, sizeof(txt), "%s%s", s_aps[i].ssid, s_aps[i].secured ? "" : "  (open)");
        item(s_list, txt, on_ap, (void *)(intptr_t)i);
    }
    item(s_list, "Scan again", on_rescan, NULL);
    item(s_list, "Back", on_back, NULL);
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

static void build_cb(void *arg)
{
    if (!s_panel) {
        return;
    }
    s_page = (page_t)(intptr_t)arg;
    switch (s_page) {
    case PAGE_MAIN:     build_main(); break;
    case PAGE_WIFI:     build_wifi(); break;
    case PAGE_PASSWORD: build_password(); break;
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
    s_main_note[0] = '\0';
    s_page = PAGE_MAIN;
    build_main();
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
