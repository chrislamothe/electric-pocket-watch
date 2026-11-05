/*
 * Triple calendar moon-phase face: black dial edge to edge (no case ring), day and month
 * windows under 12, date ring 1..31 around the edge read by a brass pointer, moon-phase window
 * at 6, cream arabic numerals, silver hour/minute hands, red centre seconds hand.
 *
 * Moon phase: mean synodic month 29.530588853 d from the new moon of 2000-01-06 18:14 UTC.
 * Error is within about a day (mean, not true, lunation), fine for a watch dial. Drawn per pixel
 * on a canvas: lit side right while waxing, left while waning (northern-hemisphere view); the
 * unlit part stays faintly visible.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "faces.h"

#define COL_DIAL     0x000000
#define COL_NUM      0xF2EEDF
#define COL_TRACK    0x8A8A8A
#define COL_DATE     0x9A9A9A
#define COL_HAND     0xD9DCE0
#define COL_RED      0xC62828
#define COL_BRASS    0xC9A04A
#define COL_WIN_BG   0x141414
#define COL_WIN_EDGE 0x5A5A5A
#define COL_SKY      0x0E1A3A
#define COL_MOON     0xF0D78C
#define COL_MOON_DK  0x2A2C3A

#define MOON_D       72          /* moon disc diameter, px */
#define SKY_D        104         /* moon window diameter */

LV_DRAW_BUF_DEFINE_STATIC(s_moon_buf, MOON_D, MOON_D, LV_COLOR_FORMAT_ARGB8888);

typedef struct {
    lv_obj_t *obj;
    lv_point_precise_t pts[2];
    int w;
} hand_t;

static int S, C, MOON_CY;
static hand_t s_hour, s_min, s_sec, s_date;
static lv_point_precise_t s_tick_pts[60][2];
static lv_obj_t *s_day_lbl, *s_mon_lbl, *s_moon;
static int s_last_yday = -1;

/* ---------------------------------------------------------------- drawing helpers */

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *disc(lv_obj_t *parent, int cx, int cy, int d, uint32_t fill, int border_w, uint32_t border)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
    if (border_w > 0) {
        lv_obj_set_style_border_width(o, border_w, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    }
    return o;
}

static lv_obj_t *line_obj(lv_obj_t *parent, int width, uint32_t color)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

static void radial(lv_point_precise_t *pts, int cx, int cy, float deg, float r0, float r1)
{
    float a = deg * (float)M_PI / 180.0f;
    float s = sinf(a), c = cosf(a);
    pts[0].x = (lv_value_precise_t)(cx + r0 * s);
    pts[0].y = (lv_value_precise_t)(cy - r0 * c);
    pts[1].x = (lv_value_precise_t)(cx + r1 * s);
    pts[1].y = (lv_value_precise_t)(cy - r1 * c);
}

/* Hand object sized to its bounding box so a move only redraws the swept area. */
static void place_hand(hand_t *h, float deg, float r0, float r1)
{
    lv_point_precise_t a[2];
    radial(a, C, C, deg, r0, r1);
    int x0 = (int)a[0].x, y0 = (int)a[0].y, x1 = (int)a[1].x, y1 = (int)a[1].y;
    int pad = h->w / 2 + 3;
    int left = (x0 < x1 ? x0 : x1) - pad;
    int top = (y0 < y1 ? y0 : y1) - pad;
    h->pts[0].x = (lv_value_precise_t)(x0 - left);
    h->pts[0].y = (lv_value_precise_t)(y0 - top);
    h->pts[1].x = (lv_value_precise_t)(x1 - left);
    h->pts[1].y = (lv_value_precise_t)(y1 - top);
    lv_obj_set_pos(h->obj, left, top);
    lv_obj_set_size(h->obj, abs(x1 - x0) + 2 * pad, abs(y1 - y0) + 2 * pad);
    lv_line_set_points(h->obj, h->pts, 2);
}

static void make_hand(hand_t *h, lv_obj_t *parent, int width, uint32_t color)
{
    h->w = width;
    h->obj = line_obj(parent, width, color);
}

static lv_obj_t *label_at(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color,
                          float r, float deg)
{
    float a = deg * (float)M_PI / 180.0f;
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_CENTER, (int)(r * sinf(a)), (int)(-r * cosf(a)));
    return l;
}

/* Rectangular calendar window with a centred label; returns the label. */
static lv_obj_t *window(lv_obj_t *parent, int cx, int cy, int w, int h)
{
    lv_obj_t *box = plain(parent);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(COL_WIN_BG), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_WIN_EDGE), 0);
    lv_obj_t *l = lv_label_create(box);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_NUM), 0);
    lv_label_set_text(l, "");
    lv_obj_center(l);
    return l;
}

/* ---------------------------------------------------------------- moon */

static float moon_phase(time_t now)
{
    const double new_moon_2000 = 947182440.0;           /* 2000-01-06 18:14 UTC */
    const double synodic = 29.530588853 * 86400.0;
    double p = fmod(((double)now - new_moon_2000) / synodic, 1.0);
    return (float)(p < 0 ? p + 1.0 : p);                /* 0 new, 0.25 first quarter, 0.5 full */
}

static void draw_moon(float phase)
{
    lv_color_t lit = lv_color_hex(COL_MOON);
    lv_color_t dark = lv_color_hex(COL_MOON_DK);
    float r = MOON_D / 2.0f;
    float k = cosf(2.0f * (float)M_PI * phase);
    lv_canvas_fill_bg(s_moon, lv_color_black(), LV_OPA_TRANSP);
    for (int py = 0; py < MOON_D; py++) {
        float y = (py + 0.5f - r) / r;
        float w = 1.0f - y * y;
        if (w <= 0) {
            continue;
        }
        w = sqrtf(w);
        for (int px = 0; px < MOON_D; px++) {
            float x = (px + 0.5f - r) / r;
            float d2 = x * x + y * y;
            if (d2 > 1.0f) {
                continue;
            }
            bool on = phase <= 0.5f ? (x > w * k) : (x < -w * k);
            /* soft 1 px edge on the limb */
            lv_opa_t opa = d2 > 0.94f ? LV_OPA_70 : LV_OPA_COVER;
            lv_canvas_set_px(s_moon, px, py, on ? lit : dark, opa);
        }
    }
}

/* ---------------------------------------------------------------- face */

static lv_obj_t *create(lv_obj_t *parent, int theme)
{
    (void)theme;                                        /* fixed palette */
    S = lv_display_get_horizontal_resolution(NULL);
    C = S / 2;
    MOON_CY = C + 64;
    s_last_yday = -1;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, S, S);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_DIAL), 0);

    /* date ring 1..31 at the edge */
    for (int d = 1; d <= 31; d++) {
        char t[4];
        snprintf(t, sizeof(t), "%d", d);
        label_at(root, t, &lv_font_montserrat_16, COL_DATE, C - 24, (d - 1) * (360.0f / 31.0f));
    }

    /* minute track */
    for (int i = 0; i < 60; i++) {
        bool five = (i % 5) == 0;
        radial(s_tick_pts[i], C, C, i * 6.0f, five ? C - 66 : C - 59, C - 50);
        lv_obj_t *l = line_obj(root, five ? 3 : 1, five ? COL_NUM : COL_TRACK);
        lv_obj_set_pos(l, 0, 0);
        lv_line_set_points(l, s_tick_pts[i], 2);
    }

    /* hour numerals */
    static const char *const NUM[12] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"};
    for (int i = 0; i < 12; i++) {
        label_at(root, NUM[i], &lv_font_montserrat_36, COL_NUM, C - 95, i * 30.0f);
    }

    /* day and month windows under 12 */
    s_day_lbl = window(root, C - 46, C - 72, 76, 32);
    s_mon_lbl = window(root, C + 46, C - 72, 76, 32);

    /* moon window at 6: night sky, a few stars, moon canvas */
    disc(root, C, MOON_CY, SKY_D, COL_SKY, 2, COL_BRASS);
    static const int STARS[][2] = {{-38, -14}, {-30, 20}, {34, -22}, {40, 12}, {-12, -40}, {16, 38}};
    for (unsigned i = 0; i < sizeof(STARS) / sizeof(STARS[0]); i++) {
        disc(root, C + STARS[i][0], MOON_CY + STARS[i][1], 3, 0xE8E2C8, 0, 0);
    }
    LV_DRAW_BUF_INIT_STATIC(s_moon_buf);
    s_moon = lv_canvas_create(root);
    lv_canvas_set_draw_buf(s_moon, &s_moon_buf);
    lv_obj_remove_flag(s_moon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_moon, C - MOON_D / 2, MOON_CY - MOON_D / 2);

    /* hands: date pointer (brass), hour, minute, red seconds on top */
    make_hand(&s_date, root, 2, COL_BRASS);
    make_hand(&s_hour, root, 7, COL_HAND);
    make_hand(&s_min, root, 4, COL_HAND);
    disc(root, C, C, 16, COL_HAND, 0, 0);
    make_hand(&s_sec, root, 2, COL_RED);
    disc(root, C, C, 8, COL_RED, 0, 0);

    place_hand(&s_date, 0, 0, C - 38);
    place_hand(&s_hour, 0, -16, C * 0.42f);
    place_hand(&s_min, 0, -20, C * 0.66f);
    place_hand(&s_sec, 0, -C * 0.18f, C * 0.78f);
    return root;
}

static void update(const struct tm *now)
{
    static const char *const DAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const MON[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    float sec = (float)now->tm_sec;
    float min = now->tm_min + sec / 60.0f;
    float hour = (now->tm_hour % 12) + min / 60.0f;
    place_hand(&s_sec, sec * 6.0f, -C * 0.18f, C * 0.78f);
    place_hand(&s_min, min * 6.0f, -20, C * 0.66f);
    place_hand(&s_hour, hour * 30.0f, -16, C * 0.42f);

    if (now->tm_yday != s_last_yday) {                  /* calendar parts change once a day */
        s_last_yday = now->tm_yday;
        lv_label_set_text(s_day_lbl, DAY[now->tm_wday % 7]);
        lv_label_set_text(s_mon_lbl, MON[now->tm_mon % 12]);
        place_hand(&s_date, (now->tm_mday - 1) * (360.0f / 31.0f), 0, C - 38);
        struct tm copy = *now;
        draw_moon(moon_phase(mktime(&copy)));
    }
}

const watch_face_t face_calendar = {.name = "calendar", .create = create, .update = update};
