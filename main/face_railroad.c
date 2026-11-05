/*
 * Railroad faces. Day: cream enamel dial with a thin black edge ring. Night: the same dial
 * inverted (black to the edge, cream ink). Both: heavy high-contrast serif
 * numerals (Abril Fatface, OFL, digits only, main/fonts/), minute track with triangle markers at
 * 12/3/6/9, running-seconds sub-dial at 6 numbered 10..60, black spade hands.
 *
 * The static dial (ticks, triangles, sub-dial track) is one object with a custom draw callback.
 * Spade hands are drawn shapes (shaft line + bulb + tip triangle + neck triangle) on objects
 * sized to each hand's bounding box, so a move only redraws the swept area.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "faces.h"

LV_FONT_DECLARE(font_railroad_64);
LV_FONT_DECLARE(font_railroad_20);

typedef struct {
    uint32_t dial, ink, pivot;
    int edge_ring;              /* px; 0 = dial runs to the glass edge */
} rr_palette_t;

static const rr_palette_t DAY   = {0xF3EFE4, 0x161412, 0x8A7A5A, 5};
static const rr_palette_t NIGHT = {0x000000, 0xF2EEDF, 0x8A7A5A, 0};   /* inverted, for AMOLED */
static const rr_palette_t *s_pal = &DAY;
#define SUB_R     64             /* sub-dial radius */
#define SUB_DY    92             /* sub-dial centre below the dial centre */

typedef struct {
    lv_obj_t *obj;
    float deg;
    float tail, bulb_at, bulb_r, tip, shaft_w;
} spade_t;

typedef struct {
    lv_obj_t *obj;
    lv_point_precise_t pts[2];
    int w;
} line_hand_t;

static int S, C;
static spade_t s_hour, s_min;
static line_hand_t s_sec;

/* ---------------------------------------------------------------- helpers */

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *disc(lv_obj_t *parent, int cx, int cy, int d, uint32_t fill, lv_opa_t opa,
                      int border_w, uint32_t border)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
    if (border_w > 0) {
        lv_obj_set_style_border_width(o, border_w, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    }
    return o;
}

static void polar(float cx, float cy, float deg, float r, float *x, float *y)
{
    float a = deg * (float)M_PI / 180.0f;
    *x = cx + r * sinf(a);
    *y = cy - r * cosf(a);
}

static void draw_line(lv_layer_t *layer, float x0, float y0, float x1, float y1, int w, bool round)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(s_pal->ink);
    d.width = w;
    d.round_start = round;
    d.round_end = round;
    d.p1.x = (lv_value_precise_t)x0;
    d.p1.y = (lv_value_precise_t)y0;
    d.p2.x = (lv_value_precise_t)x1;
    d.p2.y = (lv_value_precise_t)y1;
    lv_draw_line(layer, &d);
}

static void draw_tri(lv_layer_t *layer, float ax, float ay, float bx, float by, float cx, float cy)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(s_pal->ink);
    d.opa = LV_OPA_COVER;
    d.p[0].x = (lv_value_precise_t)ax;
    d.p[0].y = (lv_value_precise_t)ay;
    d.p[1].x = (lv_value_precise_t)bx;
    d.p[1].y = (lv_value_precise_t)by;
    d.p[2].x = (lv_value_precise_t)cx;
    d.p[2].y = (lv_value_precise_t)cy;
    lv_draw_triangle(layer, &d);
}

static void draw_dot(lv_layer_t *layer, float x, float y, float r)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(s_pal->ink);
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {(int32_t)(x - r), (int32_t)(y - r), (int32_t)(x + r), (int32_t)(y + r)};
    lv_draw_rect(layer, &d, &a);
}

/* ---------------------------------------------------------------- static dial */

static void dial_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    float x0, y0, x1, y1;

    /* minute track: ticks, heavier at the five-minute marks, triangles at 12/3/6/9 */
    for (int i = 0; i < 60; i++) {
        float deg = i * 6.0f;
        if (i % 15 == 0) {
            float ax, ay, bx, by, cx, cy;
            polar(C, C, deg, C - 26, &ax, &ay);           /* apex points inward */
            polar(C, C, deg - 1.8f, C - 14, &bx, &by);
            polar(C, C, deg + 1.8f, C - 14, &cx, &cy);
            draw_tri(layer, ax, ay, bx, by, cx, cy);
        } else if (i % 5 == 0) {
            polar(C, C, deg, C - 30, &x0, &y0);
            polar(C, C, deg, C - 15, &x1, &y1);
            draw_line(layer, x0, y0, x1, y1, 4, false);
        } else {
            polar(C, C, deg, C - 25, &x0, &y0);
            polar(C, C, deg, C - 15, &x1, &y1);
            draw_line(layer, x0, y0, x1, y1, 2, false);
        }
    }

    /* sub-dial track */
    float sy = C + SUB_DY;
    for (int i = 0; i < 60; i++) {
        float deg = i * 6.0f;
        bool five = (i % 5) == 0;
        polar(C, sy, deg, five ? SUB_R - 13 : SUB_R - 8, &x0, &y0);
        polar(C, sy, deg, SUB_R - 3, &x1, &y1);
        draw_line(layer, x0, y0, x1, y1, five ? 2 : 1, false);
    }
}

/* ---------------------------------------------------------------- spade hands */

static void spade_points(const spade_t *h, float *tx, float *ty, float *bx, float *by, float *ux,
                         float *uy, float *nx, float *ny)
{
    float a = h->deg * (float)M_PI / 180.0f;
    *ux = sinf(a);
    *uy = -cosf(a);
    *nx = cosf(a);
    *ny = sinf(a);
    *tx = C + *ux * h->tip;
    *ty = C + *uy * h->tip;
    *bx = C + *ux * h->bulb_at;
    *by = C + *uy * h->bulb_at;
}

static void spade_draw_cb(lv_event_t *e)
{
    const spade_t *h = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    float tx, ty, bx, by, ux, uy, nx, ny;
    spade_points(h, &tx, &ty, &bx, &by, &ux, &uy, &nx, &ny);
    float r = h->bulb_r;

    /* shaft from the tail through the centre to the neck */
    draw_line(layer, C - ux * h->tail, C - uy * h->tail, bx - ux * r, by - uy * r, (int)h->shaft_w, true);
    /* neck: tapers from the shaft into the bulb */
    float kx = bx - ux * r * 2.4f, ky = by - uy * r * 2.4f;
    draw_tri(layer, kx, ky, bx + nx * r * 0.75f, by + ny * r * 0.75f, bx - nx * r * 0.75f, by - ny * r * 0.75f);
    /* bulb */
    draw_dot(layer, bx, by, r);
    /* tip: from the widest part of the bulb to the point */
    draw_tri(layer, tx, ty, bx + nx * r * 0.95f, by + ny * r * 0.95f, bx - nx * r * 0.95f, by - ny * r * 0.95f);
}

static void spade_place(spade_t *h, float deg)
{
    h->deg = deg;
    float tx, ty, bx, by, ux, uy, nx, ny;
    spade_points(h, &tx, &ty, &bx, &by, &ux, &uy, &nx, &ny);
    float xs[4] = {tx, C - ux * h->tail, bx - h->bulb_r, bx + h->bulb_r};
    float ys[4] = {ty, C - uy * h->tail, by - h->bulb_r, by + h->bulb_r};
    float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < 4; i++) {
        minx = fminf(minx, xs[i]);
        maxx = fmaxf(maxx, xs[i]);
        miny = fminf(miny, ys[i]);
        maxy = fmaxf(maxy, ys[i]);
    }
    int pad = (int)h->shaft_w + 4;
    lv_obj_set_pos(h->obj, (int)minx - pad, (int)miny - pad);
    lv_obj_set_size(h->obj, (int)(maxx - minx) + 2 * pad, (int)(maxy - miny) + 2 * pad);
    lv_obj_invalidate(h->obj);                          /* angle may change inside the same box */
}

static void spade_make(spade_t *h, lv_obj_t *parent, float tail, float bulb_at, float bulb_r, float tip, float shaft_w)
{
    h->tail = tail;
    h->bulb_at = bulb_at;
    h->bulb_r = bulb_r;
    h->tip = tip;
    h->shaft_w = shaft_w;
    h->obj = plain(parent);
    lv_obj_add_event_cb(h->obj, spade_draw_cb, LV_EVENT_DRAW_MAIN, h);
}

/* ---------------------------------------------------------------- sub-dial seconds hand */

static void sec_place(float deg)
{
    float x0, y0, x1, y1;
    polar(C, C + SUB_DY, deg, -12, &x0, &y0);
    polar(C, C + SUB_DY, deg, SUB_R - 8, &x1, &y1);
    int pad = s_sec.w / 2 + 3;
    int left = (int)fminf(x0, x1) - pad, top = (int)fminf(y0, y1) - pad;
    s_sec.pts[0].x = (lv_value_precise_t)(x0 - left);
    s_sec.pts[0].y = (lv_value_precise_t)(y0 - top);
    s_sec.pts[1].x = (lv_value_precise_t)(x1 - left);
    s_sec.pts[1].y = (lv_value_precise_t)(y1 - top);
    lv_obj_set_pos(s_sec.obj, left, top);
    lv_obj_set_size(s_sec.obj, (int)fabsf(x1 - x0) + 2 * pad, (int)fabsf(y1 - y0) + 2 * pad);
    lv_line_set_points(s_sec.obj, s_sec.pts, 2);
}

/* ---------------------------------------------------------------- face */

static void label_at(lv_obj_t *parent, const char *txt, const lv_font_t *font, float cx, float cy, float r, float deg)
{
    float x, y;
    polar(cx, cy, deg, r, &x, &y);
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(s_pal->ink), 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_CENTER, (int)(x - C), (int)(y - C));
}

static lv_obj_t *create(lv_obj_t *parent, const rr_palette_t *pal)
{
    s_pal = pal;
    S = lv_display_get_horizontal_resolution(NULL);
    C = S / 2;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, S, S);
    lv_obj_set_pos(root, 0, 0);

    disc(root, C, C, S, pal->dial, LV_OPA_COVER, pal->edge_ring, pal->ink);   /* dial + optional edge ring */

    lv_obj_t *dial = plain(root);
    lv_obj_set_size(dial, S, S);
    lv_obj_add_event_cb(dial, dial_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    static const char *const NUM[12] = {"12", "1", "2", "3", "4", "5", NULL, "7", "8", "9", "10", "11"};
    for (int i = 0; i < 12; i++) {
        if (NUM[i]) {                                   /* 6 gives way to the sub-dial */
            label_at(root, NUM[i], &font_railroad_64, C, C, C - 73, i * 30.0f);
        }
    }

    disc(root, C, C + SUB_DY, SUB_R * 2, pal->dial, LV_OPA_TRANSP, 2, pal->ink);
    static const char *const SUBNUM[6] = {"60", "10", "20", "30", "40", "50"};
    for (int i = 0; i < 6; i++) {
        label_at(root, SUBNUM[i], &font_railroad_20, C, C + SUB_DY, SUB_R - 26, i * 60.0f);
    }

    s_sec.w = 2;
    s_sec.obj = lv_line_create(root);
    lv_obj_remove_flag(s_sec.obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(s_sec.obj, 2, 0);
    lv_obj_set_style_line_color(s_sec.obj, lv_color_hex(s_pal->ink), 0);
    lv_obj_set_style_line_rounded(s_sec.obj, true, 0);
    disc(root, C, C + SUB_DY, 8, pal->ink, LV_OPA_COVER, 0, 0);

    spade_make(&s_hour, root, 14, C * 0.40f, 13, C * 0.56f, 4);
    spade_make(&s_min, root, 18, C * 0.62f, 11, C * 0.80f, 3);
    disc(root, C, C, 20, pal->ink, LV_OPA_COVER, 0, 0);
    disc(root, C, C, 6, pal->pivot, LV_OPA_COVER, 0, 0);       /* pivot */

    sec_place(0);
    spade_place(&s_hour, 0);
    spade_place(&s_min, 0);
    return root;
}

static void update(const struct tm *now)
{
    float sec = (float)now->tm_sec;
    float min = now->tm_min + sec / 60.0f;
    float hour = (now->tm_hour % 12) + min / 60.0f;
    sec_place(sec * 6.0f);
    spade_place(&s_min, min * 6.0f);
    spade_place(&s_hour, hour * 30.0f);
}

static lv_obj_t *create_day(lv_obj_t *parent, int theme)   { (void)theme; return create(parent, &DAY); }
static lv_obj_t *create_night(lv_obj_t *parent, int theme) { (void)theme; return create(parent, &NIGHT); }

const watch_face_t face_railroad       = {.name = "railroad",       .create = create_day,   .update = update};
const watch_face_t face_railroad_night = {.name = "railroad-night", .create = create_night, .update = update};
