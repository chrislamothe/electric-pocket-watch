/* Minimal digital face: large 12-hour time and the date on black. */
#include <stdio.h>
#include "faces.h"

static const uint32_t TEXT[WATCH_THEME_COUNT] = {0xE8C777, 0xE8EAED, 0xB8B0A0};
static const uint32_t DIM[WATCH_THEME_COUNT]  = {0x8A6F34, 0x8A9097, 0x6B5A3A};

static lv_obj_t *s_time, *s_ampm, *s_date;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int dy)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
    return l;
}

static lv_obj_t *create(lv_obj_t *parent, int theme)
{
    int S = lv_display_get_horizontal_resolution(NULL);
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, S, S);
    lv_obj_set_pos(root, 0, 0);

    s_time = label(root, &lv_font_montserrat_48, TEXT[theme], -20);
    s_ampm = label(root, &lv_font_montserrat_20, DIM[theme], 28);
    s_date = label(root, &lv_font_montserrat_20, TEXT[theme], 70);
    return root;
}

static void update(const struct tm *now)
{
    static const char *const DAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[32];
    int h = now->tm_hour % 12;
    snprintf(buf, sizeof(buf), "%d:%02d", h ? h : 12, now->tm_min);
    lv_label_set_text(s_time, buf);
    lv_label_set_text(s_ampm, now->tm_hour < 12 ? "AM" : "PM");
    snprintf(buf, sizeof(buf), "%s %s %d", DAY[now->tm_wday % 7], MON[now->tm_mon % 12], now->tm_mday);
    lv_label_set_text(s_date, buf);
}

const watch_face_t face_digital = {
    .name = "digital",
    .create = create,
    .update = update,
};
