/*
 * Analog pocket-watch faces, all drawn by one routine:
 *   face_roman  - roman numerals, running-seconds sub-dial at 6 o'clock (theme colours)
 *   face_arabic - arabic numerals, red centre seconds hand (theme colours)
 *   face_black  - black dial, silver case, large cream arabic numerals, outer 5..60 minute
 *                 numbers, thin silver hands (fixed palette)
 * Geometry is derived from the display size (466 px on the 1.75C).
 *
 * Hands are line objects sized to their own bounding box and moved every second, so LVGL only
 * redraws the area a hand leaves and enters instead of the whole dial.
 */
#include <math.h>
#include "faces.h"

typedef struct {
    uint32_t bezel, bezel_edge, dial, sub, ink, accent, hand;
} analog_palette_t;

static const analog_palette_t THEMES[WATCH_THEME_COUNT] = {
    /* brass  */ {0xB5883A, 0x7A5A22, 0xF4EBD3, 0xEADFC2, 0x2A2016, 0x9C2A1E, 0x2A2016},
    /* silver */ {0xC9CDD2, 0x8A9097, 0xF5F6F7, 0xE6E8EA, 0x1E2328, 0x274B8F, 0x1E2328},
    /* dark   */ {0x2A2A2E, 0x6B5A3A, 0x0B0B0D, 0x151518, 0xE8DFC8, 0xD9A441, 0xE8DFC8},
};

/* black face: chrome case, black dial, cream numerals, grey minute numbers, silver hands */
static const analog_palette_t BLACK = {0xCDD1D6, 0x7E848B, 0x050505, 0x111111, 0xF2EEDF, 0x9A9A9A, 0xD9DCE0};

typedef struct {
    const char *const *numerals;        /* 12 strings, starting at 12 o'clock */
    const lv_font_t *font;
    int num_inset;                      /* numeral centre distance in from the display edge */
    bool sub_seconds;                   /* sub-dial at 6; otherwise centre seconds hand */
    bool minute_numbers;                /* 5..60 around the outside of the minute track */
    const analog_palette_t *palette;    /* NULL: use the selected theme */
    uint32_t sec_color;                 /* centre seconds hand; 0 = palette hand colour */
    int hour_w, min_w, sec_w;
    float hour_len, min_len, sec_len;   /* fractions of the radius */
} analog_style_t;

static const char *const ROMAN[12]  = {"XII", "I", "II", "III", "IIII", "V",
                                       "VI", "VII", "VIII", "IX", "X", "XI"};
static const char *const ARABIC[12] = {"12", "1", "2", "3", "4", "5",
                                       "6", "7", "8", "9", "10", "11"};

static const analog_style_t STYLE_ROMAN = {
    ROMAN, &lv_font_montserrat_28, 78, true, false, NULL, 0, 9, 6, 2, 0.52f, 0.80f, 0.0f};
static const analog_style_t STYLE_ARABIC = {
    ARABIC, &lv_font_montserrat_36, 78, false, false, NULL, 0xC62828, 9, 6, 3, 0.52f, 0.80f, 0.86f};
static const analog_style_t STYLE_BLACK = {
    ARABIC, &lv_font_montserrat_48, 92, false, true, &BLACK, 0, 5, 3, 2, 0.48f, 0.74f, 0.82f};

#define MINUTE_TICKS 60
#define SUB_TICKS    12

typedef struct {
    lv_obj_t *obj;
    lv_point_precise_t pts[2];          /* relative to obj */
    int w;
} hand_t;

static int S, C;                        /* display size and centre */
static int SUB_CY, SUB_R;               /* sub-dial centre y and radius */
static const analog_style_t *s_style;
static hand_t s_hour, s_min, s_sec;
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

/* Static tick: absolute points, content-sized object at the origin. */
static void tick(lv_obj_t *parent, lv_point_precise_t *pts, int width, uint32_t color)
{
    lv_obj_t *l = line_obj(parent, width, color);
    lv_obj_set_pos(l, 0, 0);
    lv_line_set_points(l, pts, 2);
}

/* Move a hand: the object is resized to the hand's bounding box (plus stroke padding) and the
 * points are made relative to it. Position/size changes invalidate the old and new areas. */
static void place_hand(hand_t *h, int cx, int cy, float deg, float r0, float r1)
{
    lv_point_precise_t a[2];
    radial(a, cx, cy, deg, r0, r1);
    int x0 = (int)a[0].x, y0 = (int)a[0].y, x1 = (int)a[1].x, y1 = (int)a[1].y;
    int pad = h->w / 2 + 3;
    int left = (x0 < x1 ? x0 : x1) - pad;
    int top = (y0 < y1 ? y0 : y1) - pad;
    int w = (x0 > x1 ? x0 - x1 : x1 - x0) + 2 * pad;
    int hgt = (y0 > y1 ? y0 - y1 : y1 - y0) + 2 * pad;
    h->pts[0].x = (lv_value_precise_t)(x0 - left);
    h->pts[0].y = (lv_value_precise_t)(y0 - top);
    h->pts[1].x = (lv_value_precise_t)(x1 - left);
    h->pts[1].y = (lv_value_precise_t)(y1 - top);
    lv_obj_set_pos(h->obj, left, top);
    lv_obj_set_size(h->obj, w, hgt);
    lv_line_set_points(h->obj, h->pts, 2);
}

static void make_hand(hand_t *h, lv_obj_t *parent, int width, uint32_t color)
{
    h->w = width;
    h->obj = line_obj(parent, width, color);
}

static void label_at(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color, float r, float deg)
{
    float a = deg * (float)M_PI / 180.0f;
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_CENTER, (int)(r * sinf(a)), (int)(-r * cosf(a)));
}

static void sec_geometry(int *cy, float *r0, float *r1)
{
    if (s_style->sub_seconds) {
        *cy = SUB_CY;
        *r0 = -6;
        *r1 = SUB_R - 6;
    } else {
        *cy = C;
        *r0 = -C * 0.20f;
        *r1 = C * s_style->sec_len;
    }
}

static void set_hands(float sec, float min, float hour)
{
    int cy;
    float r0, r1;
    sec_geometry(&cy, &r0, &r1);
    place_hand(&s_sec, C, cy, sec * 6.0f, r0, r1);
    place_hand(&s_min, C, C, min * 6.0f, -22, C * s_style->min_len);
    place_hand(&s_hour, C, C, hour * 30.0f, -18, C * s_style->hour_len);
}

static lv_obj_t *create(lv_obj_t *parent, int theme, const analog_style_t *style)
{
    const analog_palette_t *t = style->palette ? style->palette : &THEMES[theme];
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

    /* minute track: moved inward when the 5..60 numbers sit outside it */
    float r_out = style->minute_numbers ? C - 48 : C - 22;
    int n = 0;
    for (int i = 0; i < MINUTE_TICKS; i++, n++) {
        bool five = (i % 5) == 0;
        float r_in;
        if (style->minute_numbers) {
            r_in = five ? r_out - 12 : r_out - 8;
        } else {
            r_in = five ? r_out - 20 : r_out - 9;
        }
        radial(s_tick_pts[n], C, C, i * 6.0f, r_in, r_out);
        tick(root, s_tick_pts[n], five ? (style->minute_numbers ? 3 : 5) : 2,
             style->minute_numbers ? t->accent : t->ink);
    }
    if (style->minute_numbers) {
        static const char *const MIN[12] = {"60", "5", "10", "15", "20", "25",
                                            "30", "35", "40", "45", "50", "55"};
        for (int i = 0; i < 12; i++) {
            label_at(root, MIN[i], &lv_font_montserrat_16, t->accent, C - 31, i * 30.0f);
        }
    }

    for (int i = 0; i < 12; i++) {
        label_at(root, style->numerals[i], style->font, t->ink, C - style->num_inset, i * 30.0f);
    }

    if (style->sub_seconds) {
        disc(root, C, SUB_CY, SUB_R * 2, t->sub, 2, t->bezel_edge);
        for (int i = 0; i < SUB_TICKS; i++, n++) {
            radial(s_tick_pts[n], C, SUB_CY, i * 30.0f, SUB_R - 9, SUB_R - 3);
            tick(root, s_tick_pts[n], 2, t->ink);
        }
        make_hand(&s_sec, root, style->sec_w, t->accent);       /* under the main hands */
    }

    make_hand(&s_hour, root, style->hour_w, t->hand);
    make_hand(&s_min, root, style->min_w, t->hand);

    if (style->sub_seconds) {
        disc(root, C, C, 18, t->hand, 3, t->accent);
    } else {
        uint32_t sc = style->sec_color ? style->sec_color : t->hand;
        disc(root, C, C, 14, t->hand, 0, 0);
        make_hand(&s_sec, root, style->sec_w, sc);               /* over the main hands */
        disc(root, C, C, 8, sc, 0, 0);
    }

    set_hands(0, 0, 0);
    return root;
}

static lv_obj_t *create_roman(lv_obj_t *parent, int theme)  { return create(parent, theme, &STYLE_ROMAN); }
static lv_obj_t *create_arabic(lv_obj_t *parent, int theme) { return create(parent, theme, &STYLE_ARABIC); }
static lv_obj_t *create_black(lv_obj_t *parent, int theme)  { return create(parent, theme, &STYLE_BLACK); }

static void update(const struct tm *now)
{
    float sec = (float)now->tm_sec;
    float min = now->tm_min + sec / 60.0f;
    float hour = (now->tm_hour % 12) + min / 60.0f;
    set_hands(sec, min, hour);
}

const watch_face_t face_roman  = {.name = "roman",  .create = create_roman,  .update = update};
const watch_face_t face_arabic = {.name = "arabic", .create = create_arabic, .update = update};
const watch_face_t face_black  = {.name = "black",  .create = create_black,  .update = update};
