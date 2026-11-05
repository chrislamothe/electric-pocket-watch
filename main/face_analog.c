/*
 * Analog pocket-watch faces, sharing one drawing routine:
 *   face_roman  - roman numerals, running-seconds sub-dial at 6 o'clock
 *   face_arabic - arabic numerals, red centre seconds hand
 * Geometry is derived from the display size (466 px on the 1.75C).
 */
#include <math.h>
#include "faces.h"

typedef struct {
    uint32_t bezel, bezel_edge, dial, sub, ink, accent;
} analog_theme_t;

static const analog_theme_t THEMES[WATCH_THEME_COUNT] = {
    /* brass  */ {0xB5883A, 0x7A5A22, 0xF4EBD3, 0xEADFC2, 0x2A2016, 0x9C2A1E},
    /* silver */ {0xC9CDD2, 0x8A9097, 0xF5F6F7, 0xE6E8EA, 0x1E2328, 0x274B8F},
    /* dark   */ {0x2A2A2E, 0x6B5A3A, 0x0B0B0D, 0x151518, 0xE8DFC8, 0xD9A441},
};

#define SECONDS_RED  0xC62828       /* centre seconds hand, same on every theme */

typedef struct {
    const char *const *numerals;       /* 12 strings, starting at 12 o'clock */
    const lv_font_t *font;
    bool sub_seconds;                  /* true: sub-dial at 6; false: red centre seconds hand */
} analog_style_t;

static const char *const ROMAN[12]  = {"XII", "I", "II", "III", "IIII", "V",
                                       "VI", "VII", "VIII", "IX", "X", "XI"};
static const char *const ARABIC[12] = {"12", "1", "2", "3", "4", "5",
                                       "6", "7", "8", "9", "10", "11"};

static const analog_style_t STYLE_ROMAN  = {ROMAN, &lv_font_montserrat_28, true};
static const analog_style_t STYLE_ARABIC = {ARABIC, &lv_font_montserrat_36, false};

#define MINUTE_TICKS 60
#define SUB_TICKS    12

static int S, C;                       /* display size and centre */
static int SUB_CY, SUB_R;              /* sub-dial centre y and radius */
static const analog_style_t *s_style;

static lv_obj_t *s_hour, *s_min, *s_sec;
static lv_point_precise_t s_hour_pts[2], s_min_pts[2], s_sec_pts[2];
static lv_point_precise_t s_tick_pts[MINUTE_TICKS + SUB_TICKS][2];

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

/* Hands use a line object covering the whole face so its bounding box never changes between
 * updates; static ticks keep the default content-sized box. */
static lv_obj_t *line(lv_obj_t *parent, lv_point_precise_t *pts, int width, uint32_t color, bool full)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(l, 0, 0);
    if (full) {
        lv_obj_set_size(l, S, S);
    }
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_line_set_points(l, pts, 2);
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

static lv_obj_t *create(lv_obj_t *parent, int theme, const analog_style_t *style)
{
    const analog_theme_t *t = &THEMES[theme];
    s_style = style;
    S = lv_display_get_horizontal_resolution(NULL);
    C = S / 2;
    SUB_R = S / 10;
    SUB_CY = C + (S * 19) / 100;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);   /* stays clickable: receives the swipe */
    lv_obj_set_size(root, S, S);
    lv_obj_set_pos(root, 0, 0);

    disc(root, C, C, S, t->bezel, 0, 0);
    disc(root, C, C, S - 28, t->dial, 3, t->bezel_edge);

    /* minute track and hour ticks */
    int n = 0;
    for (int i = 0; i < MINUTE_TICKS; i++, n++) {
        bool hour = (i % 5) == 0;
        float r_out = C - 22;
        float r_in = hour ? r_out - 20 : r_out - 9;
        radial(s_tick_pts[n], C, C, i * 6.0f, r_in, r_out);
        line(root, s_tick_pts[n], hour ? 5 : 2, t->ink, false);
    }

    /* numerals (roman uses IIII, the traditional dial form) */
    float r_num = C - 78;
    for (int i = 0; i < 12; i++) {
        float a = i * 30.0f * (float)M_PI / 180.0f;
        lv_obj_t *l = lv_label_create(root);
        lv_obj_set_style_text_font(l, style->font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(t->ink), 0);
        lv_label_set_text(l, style->numerals[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, (int)(r_num * sinf(a)), (int)(-r_num * cosf(a)));
    }

    if (style->sub_seconds) {
        /* running-seconds sub-dial at 6 o'clock, drawn under the main hands */
        disc(root, C, SUB_CY, SUB_R * 2, t->sub, 2, t->bezel_edge);
        for (int i = 0; i < SUB_TICKS; i++, n++) {
            radial(s_tick_pts[n], C, SUB_CY, i * 30.0f, SUB_R - 9, SUB_R - 3);
            line(root, s_tick_pts[n], 2, t->ink, false);
        }
        radial(s_sec_pts, C, SUB_CY, 0, -6, SUB_R - 6);
        s_sec = line(root, s_sec_pts, 2, t->accent, true);
    }

    /* hour and minute hands */
    radial(s_hour_pts, C, C, 0, -18, C * 0.52f);
    s_hour = line(root, s_hour_pts, 9, t->ink, true);
    radial(s_min_pts, C, C, 0, -22, C * 0.80f);
    s_min = line(root, s_min_pts, 6, t->ink, true);

    if (style->sub_seconds) {
        disc(root, C, C, 18, t->ink, 3, t->accent);
    } else {
        /* red centre seconds hand with a counterweight tail, over the other hands */
        disc(root, C, C, 18, t->ink, 0, 0);
        radial(s_sec_pts, C, C, 0, -C * 0.20f, C * 0.86f);
        s_sec = line(root, s_sec_pts, 3, SECONDS_RED, true);
        disc(root, C, C, 10, SECONDS_RED, 0, 0);
    }
    return root;
}

static lv_obj_t *create_roman(lv_obj_t *parent, int theme)  { return create(parent, theme, &STYLE_ROMAN); }
static lv_obj_t *create_arabic(lv_obj_t *parent, int theme) { return create(parent, theme, &STYLE_ARABIC); }

static void update(const struct tm *now)
{
    float sec = (float)now->tm_sec;
    float min = now->tm_min + sec / 60.0f;
    float hour = (now->tm_hour % 12) + min / 60.0f;

    if (s_style->sub_seconds) {
        radial(s_sec_pts, C, SUB_CY, sec * 6.0f, -6, SUB_R - 6);
    } else {
        radial(s_sec_pts, C, C, sec * 6.0f, -C * 0.20f, C * 0.86f);
    }
    lv_line_set_points(s_sec, s_sec_pts, 2);
    radial(s_min_pts, C, C, min * 6.0f, -22, C * 0.80f);
    lv_line_set_points(s_min, s_min_pts, 2);
    radial(s_hour_pts, C, C, hour * 30.0f, -18, C * 0.52f);
    lv_line_set_points(s_hour, s_hour_pts, 2);
}

const watch_face_t face_roman = {
    .name = "roman",
    .create = create_roman,
    .update = update,
};

const watch_face_t face_arabic = {
    .name = "arabic",
    .create = create_arabic,
    .update = update,
};
