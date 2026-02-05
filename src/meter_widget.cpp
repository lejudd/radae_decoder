#include "meter_widget.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

/* ── internal state ─────────────────────────────────────────────────────── */

struct MeterState {
    float level_left  = 0.f, level_right  = 0.f;   // current RMS  (linear 0..1)
    float peak_left   = 0.f, peak_right   = 0.f;   // peak-hold    (linear 0..1)
    int   timer_left  = 0,   timer_right  = 0;     // hold-frame counter
};

static constexpr const char* STATE_KEY       = "meter-state";
static constexpr int         PEAK_HOLD      = 45;     // frames before fall (~1.5 s @ 30 fps)
static constexpr float       PEAK_DECAY     = 0.925f; // per-frame multiplier during fall

/* ── dB / position helpers ──────────────────────────────────────────────── */

static constexpr float DB_MIN = -60.f;
static constexpr float DB_MAX =   0.f;

/* linear amplitude  →  0..1 meter position (bottom = 0, top = 1) */
static float level_to_pos(float level)
{
    if (level < 1e-6f) return 0.f;
    float db = 20.f * std::log10(level);
    db = std::max(DB_MIN, std::min(DB_MAX, db));
    return (db - DB_MIN) / (DB_MAX - DB_MIN);
}

/* ── cairo helpers ──────────────────────────────────────────────────────── */

/* dB tick positions used for grid lines and labels */
static constexpr int TICKS[] = { 0, -6, -12, -18, -24, -30, -36, -42, -48, -54, -60 };
static constexpr int NUM_TICKS = 11;

static float db_to_pos(int db)
{
    return (static_cast<float>(db) - DB_MIN) / (DB_MAX - DB_MIN);
}

/* ── draw one vertical bar ──────────────────────────────────────────────── */
/*    x, y   = top-left of the bar rectangle
 *    w, h   = width / height
 *    fill   = 0..1 how far up the bar is filled
 *    peak   = 0..1 where to draw the white peak line               */

static void draw_bar(cairo_t* cr,
                     double x, double y, double w, double h,
                     float  fill, float  peak)
{
    /* background */
    cairo_set_source_rgb(cr, 0.17, 0.17, 0.20);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);

    /* subtle border */
    cairo_set_source_rgb(cr, 0.30, 0.30, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0);
    cairo_stroke(cr);

    /* tick grid lines (very subtle, inside the bar) */
    cairo_set_source_rgba(cr, 0.40, 0.40, 0.44, 0.25);
    cairo_set_line_width(cr, 0.6);
    for (int i = 0; i < NUM_TICKS; ++i) {
        double ty = y + h - db_to_pos(TICKS[i]) * h;
        cairo_move_to(cr, x + 1, ty);
        cairo_line_to(cr, x + w - 1, ty);
        cairo_stroke(cr);
    }

    /* filled portion with vertical gradient (bottom → top) */
    double fill_h = fill * h;
    if (fill_h > 0.5) {
        cairo_pattern_t* grad =
            cairo_pattern_create_linear(0, y + h,   /* bottom */
                                        0, y);      /* top    */
        //                                  stop    R     G     B
        cairo_pattern_add_color_stop_rgb(grad, 0.00, 0.00, 0.65, 0.18);  // deep green
        cairo_pattern_add_color_stop_rgb(grad, 0.50, 0.05, 0.88, 0.10);  // bright green
        cairo_pattern_add_color_stop_rgb(grad, 0.68, 0.70, 0.92, 0.05);  // yellow-green
        cairo_pattern_add_color_stop_rgb(grad, 0.80, 0.95, 0.80, 0.02);  // yellow
        cairo_pattern_add_color_stop_rgb(grad, 0.90, 1.00, 0.45, 0.02);  // amber
        cairo_pattern_add_color_stop_rgb(grad, 1.00, 1.00, 0.08, 0.05);  // red

        cairo_set_source(cr, grad);
        cairo_rectangle(cr, x, y + h - fill_h, w, fill_h);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
    }

    /* peak-hold line */
    if (peak > 0.004f) {
        double py = y + h - peak * h;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.88);
        cairo_set_line_width(cr, 2.5);
        cairo_move_to(cr, x + 2, py);
        cairo_line_to(cr, x + w - 2, py);
        cairo_stroke(cr);
    }
}

/* ── draw the dB label column ───────────────────────────────────────────── */

static void draw_db_labels(cairo_t* cr, double x, double y, double w, double h)
{
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.58);
    cairo_set_font_size(cr, 10.0);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_SUBPIXEL);

    cairo_font_options_t* opts = cairo_font_options_create();
    cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr, opts);
    cairo_font_options_destroy(opts);

    for (int i = 0; i < NUM_TICKS; ++i) {
        double ty = y + h - db_to_pos(TICKS[i]) * h;

        char label[8];
        snprintf(label, sizeof label, "%d", TICKS[i]);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);

        cairo_move_to(cr,
                      x + (w - ext.width) * 0.5 - ext.x_bearing,
                      ty - ext.height * 0.5 - ext.y_bearing);
        cairo_show_text(cr, label);
    }
}

/* ── main draw callback ─────────────────────────────────────────────────── */

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer /*data*/)
{
    auto* st = static_cast<MeterState*>(
        g_object_get_data(G_OBJECT(widget), STATE_KEY));
    if (!st) return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    double W = alloc.width;
    double H = alloc.height;

    /* ── layout ──────────────────────────────────────────────────── */
    constexpr double margin_x   = 12;
    constexpr double margin_top =  8;
    constexpr double margin_bot = 24;   // room for "L" / "R"
    constexpr double gap        =  5;
    constexpr double label_col  = 34;   // dB-label column width

    double bar_h = H - margin_top - margin_bot;
    double bar_w = (W - 2 * margin_x - 2 * gap - label_col) / 2.0;
    if (bar_w < 12) bar_w = 12;

    double x_left   = margin_x;
    double x_labels = x_left + bar_w + gap;
    double x_right  = x_labels + label_col + gap;
    double y_top    = margin_top;

    /* ── overall background ──────────────────────────────────────── */
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.14);
    cairo_paint(cr);

    /* ── bars ────────────────────────────────────────────────────── */
    draw_bar(cr, x_left,  y_top, bar_w, bar_h,
             level_to_pos(st->level_left),  level_to_pos(st->peak_left));
    draw_bar(cr, x_right, y_top, bar_w, bar_h,
             level_to_pos(st->level_right), level_to_pos(st->peak_right));

    /* ── dB labels ───────────────────────────────────────────────── */
    draw_db_labels(cr, x_labels, y_top, label_col, bar_h);

    /* ── channel labels "L"  "R" ─────────────────────────────────── */
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.82);
    cairo_set_font_size(cr, 13.0);

    auto center_text = [&](const char* txt, double cx) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, txt, &ext);
        cairo_move_to(cr,
                      cx  - ext.width * 0.5 - ext.x_bearing,
                      H - 6 - ext.y_bearing);
        cairo_show_text(cr, txt);
    };
    center_text("L", x_left  + bar_w  * 0.5);
    center_text("R", x_right + bar_w  * 0.5);

    return FALSE;
}

/* ── public API ─────────────────────────────────────────────────────────── */

GtkWidget* meter_widget_new(void)
{
    GtkWidget* da = gtk_drawing_area_new();

    auto* state = new MeterState{};
    g_object_set_data_full(G_OBJECT(da), STATE_KEY, state,
        [](gpointer p){ delete static_cast<MeterState*>(p); });

    g_signal_connect(da, "draw", G_CALLBACK(on_draw), nullptr);

    gtk_widget_set_size_request(da, 240, 320);   // minimum comfortable size
    return da;
}

void meter_widget_update(GtkWidget* widget, float level_left, float level_right)
{
    auto* st = static_cast<MeterState*>(
        g_object_get_data(G_OBJECT(widget), STATE_KEY));
    if (!st) return;

    st->level_left  = level_left;
    st->level_right = level_right;

    /* peak-hold / fall logic ------------------------------------------ */
    auto update_peak = [](float level, float& peak, int& timer) {
        if (level >= peak) {
            peak  = level;
            timer = PEAK_HOLD;
        } else {
            if (timer > 0)  --timer;
            else            peak *= PEAK_DECAY;
            if (peak < 1e-7f) peak = 0.f;
        }
    };
    update_peak(level_left,  st->peak_left,  st->timer_left);
    update_peak(level_right, st->peak_right, st->timer_right);

    gtk_widget_queue_draw(widget);
}
