/* BUILD_NUMBER: 261003.113745 */
/**
 * @file vu_meter.cpp
 * @brief Stereo bar painters with ballistics, peak hold/decay, tip blend, afterglow.
 *
 * Pipeline per frame (after ADC→dB in app_main):
 *  1. vu_ballistics() — attack_ms; modes 0–5 use k_fast_release_ms (80 ms)
 *  2. vu_display() → paint_stereo()
 *     - map dB → float pixel height; stabilize (0.5 px deadband, classic+rainbow)
 *     - rainbow: peak hold/fall + pure-blue marker; fractional tip + short afterglow
 *     - classic: hard on/off from latched palette (no glow) + pure-blue peak marker
 *
 * Classic hard breaks — fixed integers for LED_COUNT==216:
 *
 *   Mode              Bar index i     Green     Yellow      Red
 *   3 linear          0…215           i < 21    i < 108     i >= 108
 *   4 inside-out      0…107           i < 10    i < 54      i >= 54
 *   5 outside-in      0…107 / end     i < 10    i < 54      i >= 54
 *
 * (Yellow is the else-if after green, so effectively gr <= i < yr.)
 * Classic tip follows stabilized height (aligned with rainbow stickiness).
 * Brightness: pot only (deadbanded); no automatic dimming with input level.
 * Rainbow palette: blue→green→orange→red with a red-biased tip.
 * Mode changes call web_ui_notify_mode() so the web dropdown stays in sync.
 */

#include "vu_config.h"
#include "pins.h"
#include "vu_leds.h"
#include "web_ui.h"

#include <math.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "esp_log.h"

/* --- Tunables / runtime state (definitions; declared in vu_config.h) --- */

extern "C" {
int brightness = 55;
int ADC_midpoint = 2048;
int mode = 0;
int MaxMode = 6;

/** ADC averaging window length (ms). */
int average_time = 25;

/**
 * Classic / rainbow zone boundaries — fixed pixel indices for LED_COUNT (216).
 * Mode 3: green i<21, yellow i<108, red i>=108.
 * Mode 5: green i<10, yellow i<54, red i>=54 (bar index from each end).
 */
int zeroRange = 0;
int blueRange = 0;
int greenRange = 21;   /**< mode 3: green if i < 21 */
int yellowRange = 108; /**< mode 3: yellow if i < 108 (after green check); red if i >= 108 */
int redRange = LED_COUNT;

/** How many pure-blue LEDs to draw for the peak-hold marker. */
int endBluePixels = 1;

float dBmax = 58;
float dBmin = 28;
int noise_level = 0;
float noise_floor_adc = 80.0f;

/**
 * Ballistics / peak / afterglow feel (see vu_config.h for meanings).
 * Modes 0–5 override release with k_fast_release_ms in vu_ballistics().
 */
float attack_ms = 25.0f;
float release_ms = 200.0f;
float peak_hold_ms = 350.0f;
float peak_fall_px_per_s = 60.0f;
float afterglow_halflife_ms = 70.0f;

/** Last painted bar heights (pixels); exposed for diagnostics / UI. */
float PixelCountLeft = 0;
float PixelCountRight = 0;
/** Peak marker positions (pixels); held/fallen independently of bar height. */
float previous_pixel_left = 0;
float previous_pixel_right = 0;

/**
 * Palette: 0 black, 1 blue, 2 green, 3 yellow, 4 red.
 * Yellow keeps some G; red is pure R so the hard break at yellowRange reads clearly.
 */
int colors[5][3] = {
    {0, 0, 0},
    {0, 0, 255},
    {0, 255, 0},     /* green */
    {255, 180, 0},   /* yellow (was orange 255,100,0 — too close to dim red) */
    {255, 0, 0},     /* red */
};

float VoltageFactor = 1000.0f * (3.3f / 2048.0f);
} /* extern "C" */

/** Mode 5 half-bar: green i<10, yellow i<54, red i>=54. */
static const int k_classic_gr_half = 10;
static const int k_classic_yr_half = 54;

static const char *TAG = "vu";

static float s_smooth_l = 28.0f;
static float s_smooth_r = 28.0f;
static TickType_t s_last_ballistic_tick = 0;
static TickType_t s_last_paint_tick = 0;
static TickType_t s_peak_hold_until_l = 0;
static TickType_t s_peak_hold_until_r = 0;
/** Per-LED afterglow intensity 0…1 for each strip. */
static float s_glow_l[LED_COUNT];
static float s_glow_r[LED_COUNT];
/** Stabilized pixel heights (kill tip flicker from residual dB noise). */
static float s_disp_h_l = -1.0f;
static float s_disp_h_r = -1.0f;
static bool s_disp_falling_l = false;
static bool s_disp_falling_r = false;
/** Shared height fall deadband (pixels) — classic + rainbow tip feel aligned. */
static const float k_height_deadband_px = 0.5f;
/** Rainbow afterglow half-life (ms); classic is hard on/off (no glow). */
static const float k_rainbow_afterglow_halflife_ms = 20.0f;
/** Shared dB release (ms) for modes 0–5; snappier head than release_ms. */
static const float k_fast_release_ms = 80.0f;
/** Pot deadband (classic) so ADC noise does not shimmer zone edges. */
static const int k_classic_pot_deadband = 8;
/** Classic integer tip positions (−1 = unset). */
static int s_classic_tip_l = -1;
static int s_classic_tip_r = -1;
/** Last pot reading accepted into the classic palette. */
static int s_classic_pot = -1;
/** Brightness actually applied to the latched palette. */
static int s_brightness_applied = -1;
/**
 * Pre-scaled classic palette (index matches colors[]). Paint uses only this.
 */
static uint8_t s_classic_rgb[5][3];
static bool s_classic_rgb_valid = false;

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float mapf(float x, float in_min, float in_max, float out_min, float out_max)
{
    if (in_max == in_min) {
        return out_min;
    }
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/**
 * Elapsed milliseconds since *last, then refresh *last.
 * Clamped to [1, 100] ms so first call / stalls do not explode filters.
 * Uses float tick→ms (not portTICK_PERIOD_MS) to avoid integer-division clang-tidy.
 */
static float dt_ms_since(TickType_t *last)
{
    TickType_t now = xTaskGetTickCount();
    float dt = 0.0f;
    if (*last != 0) {
        dt = (float)(now - *last) * (1000.0f / (float)configTICK_RATE_HZ);
    }
    *last = now;
    if (dt < 1.0f) {
        dt = 1.0f;
    }
    if (dt > 100.0f) {
        dt = 100.0f;
    }
    return dt;
}

/** Scale RGB by 0…1 and round to uint8. */
static void scale_rgb(uint8_t r, uint8_t g, uint8_t b, float scale, uint8_t *ro, uint8_t *go, uint8_t *bo)
{
    scale = clampf(scale, 0.0f, 1.0f);
    *ro = (uint8_t)(r * scale + 0.5f);
    *go = (uint8_t)(g * scale + 0.5f);
    *bo = (uint8_t)(b * scale + 0.5f);
}

/**
 * Classic break indices for this geometry — compile-time integers only.
 * No span/LED_COUNT multiply at paint time.
 */
static void classic_breaks(int span, int *gr, int *yr)
{
    if (span >= LED_COUNT) {
        *gr = greenRange;   /* 21 */
        *yr = yellowRange;  /* 108 */
    } else {
        *gr = k_classic_gr_half; /* 10 */
        *yr = k_classic_yr_half; /* 54 */
    }
}

/**
 * Classic hard zone color for bar index @p i ∈ [0, span).
 * No cross-fade — palette stop changes only at the fixed breaks from classic_breaks().
 */
/** Zone id: 2=green (i<gr), 3=yellow (i<yr), 4=red (i>=yr). */
static int classic_zone(int i, int gr, int yr)
{
    if (i < gr) {
        return 2;
    }
    if (i < yr) {
        return 3;
    }
    return 4;
}

/** Rebuild s_classic_rgb[] from palette × latched brightness (call on latch only). */
static void classic_latch_palette(int bri)
{
    if (bri < 0) {
        bri = 0;
    }
    if (bri > 255) {
        bri = 255;
    }
    for (int c = 0; c < 5; c++) {
        s_classic_rgb[c][0] = (uint8_t)((colors[c][0] * bri) >> 8);
        s_classic_rgb[c][1] = (uint8_t)((colors[c][1] * bri) >> 8);
        s_classic_rgb[c][2] = (uint8_t)((colors[c][2] * bri) >> 8);
    }
    /* Force pure red (G=0) so yellow cannot bleed into the red zone under sag. */
    s_classic_rgb[4][1] = 0;
    s_classic_rgb[4][2] = 0;
    s_classic_rgb_valid = true;
}

/**
 * Write one classic LED from the latched zone palette only.
 * No live pot scale, no per-frame multiply — zone breaks stay bit-stable.
 */
static void classic_set(led_strip_handle_t strip, int phys, bool on, int gr, int yr, int bar_i,
                        int full)
{
    (void)full;
    if (phys < 0 || phys >= LED_COUNT) {
        return;
    }
    if (!on) {
        vu_leds_set_pixel_raw(strip, phys, 0, 0, 0);
        return;
    }
    if (!s_classic_rgb_valid) {
        const int bri = (s_brightness_applied > 0) ? s_brightness_applied : brightness;
        classic_latch_palette(bri);
    }
    const int z = classic_zone(bar_i, gr, yr);
    vu_leds_set_pixel_raw(strip, phys, s_classic_rgb[z][0], s_classic_rgb[z][1],
                          s_classic_rgb[z][2]);
}

/**
 * Smooth rainbow along normalized bar position t ∈ [0,1].
 * Modes 0–2 only. Upper stop is orange→red with a bias toward red so the
 * head reads red (classic yellow palette left unchanged for modes 3–5).
 */
static void rainbow_color_at(float t, uint8_t *r, uint8_t *g, uint8_t *b)
{
    t = clampf(t, 0.0f, 1.0f);
    const float t0 = 0.0f;
    const float t1 = clampf((float)greenRange / (float)LED_COUNT, 0.01f, 0.98f);
    /* Start the orange→red climb earlier than classic yellowRange (0.5). */
    const float t2 = clampf(0.38f, t1 + 0.01f, 0.99f);
    const float t3 = 1.0f;
    /* Rainbow mid stop: orange (classic yellow 255,180,0 stays for modes 3–5). */
    static const int k_rain_orange[3] = {255, 70, 0};

    float u;
    int c0[3], c1[3];
    if (t <= t1) {
        c0[0] = colors[1][0];
        c0[1] = colors[1][1];
        c0[2] = colors[1][2];
        c1[0] = colors[2][0];
        c1[1] = colors[2][1];
        c1[2] = colors[2][2];
        u = (t - t0) / (t1 - t0);
    } else if (t <= t2) {
        c0[0] = colors[2][0];
        c0[1] = colors[2][1];
        c0[2] = colors[2][2];
        c1[0] = k_rain_orange[0];
        c1[1] = k_rain_orange[1];
        c1[2] = k_rain_orange[2];
        u = (t - t1) / (t2 - t1);
    } else {
        c0[0] = k_rain_orange[0];
        c0[1] = k_rain_orange[1];
        c0[2] = k_rain_orange[2];
        c1[0] = colors[4][0];
        c1[1] = colors[4][1];
        c1[2] = colors[4][2];
        u = (t - t2) / (t3 - t2);
        /* Bias toward red so the tip is not stuck yellowish. */
        u = sqrtf(clampf(u, 0.0f, 1.0f));
    }
    u = clampf(u, 0.0f, 1.0f);
    *r = (uint8_t)(c0[0] + (c1[0] - c0[0]) * u + 0.5f);
    *g = (uint8_t)(c0[1] + (c1[1] - c0[1]) * u + 0.5f);
    *b = (uint8_t)(c0[2] + (c1[2] - c0[2]) * u + 0.5f);
}

static void set_px(led_strip_handle_t strip, int i, uint8_t r, uint8_t g, uint8_t b, float scale)
{
    uint8_t rr, gg, bb;
    scale_rgb(r, g, b, scale, &rr, &gg, &bb);
    vu_leds_set_pixel(strip, i, rr, gg, bb);
}

/**
 * Afterglow write: @p target is bar coverage 0…1 for this LED this frame.
 * Glow rises instantly; above the bar it decays by @p decay (and a small
 * linear step so the rainbow head does not hang). Hue from geometry.
 */
static void paint_px(led_strip_handle_t strip, float *glow, int i, uint8_t r, uint8_t g, uint8_t b,
                     float target, float decay)
{
    if (i < 0 || i >= LED_COUNT) {
        return;
    }
    target = clampf(target, 0.0f, 1.0f);
    if (target >= glow[i]) {
        glow[i] = target;
    } else {
        glow[i] *= decay;
        glow[i] -= 0.12f; /* snappier tip fall on top of exponential decay */
        if (glow[i] < target) {
            glow[i] = target;
        }
        if (glow[i] < 0.02f) {
            glow[i] = 0.0f;
        }
    }
    set_px(strip, i, r, g, b, glow[i]);
}

/**
 * Stabilize mapped pixel height: track rises immediately; ignore sub-deadband
 * dips; once a fall exceeds the deadband, track freely until the next rise.
 */
static float stabilize_height(float height, float *sticky, bool *falling, float deadband)
{
    if (*sticky < 0.0f) {
        *sticky = height;
        *falling = false;
        return *sticky;
    }
    if (height >= *sticky) {
        *sticky = height;
        *falling = false;
    } else if (*falling || height <= *sticky - deadband) {
        *falling = true;
        *sticky = height;
    }
    return *sticky;
}

/** Latch pot → classic palette (deadbanded). No automatic dimming with level. */
static void classic_sync_brightness(void)
{
    int pot = brightness;
    if (pot < 0) {
        pot = 0;
    }
    if (pot > 255) {
        pot = 255;
    }
    if (s_classic_pot < 0 || pot - s_classic_pot >= k_classic_pot_deadband ||
        s_classic_pot - pot >= k_classic_pot_deadband) {
        s_classic_pot = pot;
    }
    if (!s_classic_rgb_valid || s_classic_pot != s_brightness_applied) {
        s_brightness_applied = s_classic_pot;
        vu_leds_set_brightness(static_cast<uint8_t>(s_brightness_applied));
        classic_latch_palette(s_brightness_applied);
    }
}

/**
 * Peak-hold state machine: latch on new highs for peak_hold_ms, then fall at
 * peak_fall_px_per_s. Cleared when the bar is essentially at silence.
 */
static void update_peak(float height, float *peak, TickType_t *hold_until, float dt_ms, float span)
{
    if (height >= *peak) {
        *peak = height;
        *hold_until = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)peak_hold_ms);
    } else if (xTaskGetTickCount() >= *hold_until) {
        *peak -= peak_fall_px_per_s * (dt_ms / 1000.0f);
        if (*peak < height) {
            *peak = height;
        }
    }
    *peak = clampf(*peak, 0.0f, span);
    if (height <= 0.01f && *peak < 0.5f) {
        *peak = 0.0f;
    }
}

/**
 * Draw the peak-hold marker on a linear bar as pure blue (0,0,255).
 * Skipped when the marker sits on/under the live tip (wide gap avoids strobing).
 */
static void draw_peak_marker_linear(led_strip_handle_t strip, float peak, float height, int span)
{
    if (peak <= height + 1.0f || peak < 0.5f) {
        return;
    }
    int base = (int)floorf(peak);
    for (int j = 0; j < endBluePixels; j++) {
        int idx = base + j;
        if (idx < 0 || idx >= span) {
            continue;
        }
        vu_leds_set_pixel(strip, idx, 0, 0, 255);
    }
}

enum class Geom { Linear, InsideOut, OutsideIn };
enum class Style { Classic, Rainbow };

/** Draw pure-blue peak marker for the active geometry (classic + rainbow). */
static void draw_peak_marker(led_strip_handle_t strip, float peak, float height, int span, Geom geom)
{
    const int half = LED_COUNT / 2;
    if (geom == Geom::Linear) {
        draw_peak_marker_linear(strip, peak, height, span);
        return;
    }
    if (peak <= height + 1.0f || peak < 0.5f) {
        return;
    }
    int p = (int)floorf(peak);
    for (int j = 0; j < endBluePixels; j++) {
        if (geom == Geom::InsideOut) {
            vu_leds_set_pixel(strip, half - p + j - 1, 0, 0, 255);
            vu_leds_set_pixel(strip, half + p + j, 0, 0, 255);
        } else { /* OutsideIn */
            vu_leds_set_pixel(strip, p + j, 0, 0, 255);
            vu_leds_set_pixel(strip, LED_COUNT - p - 1 + j, 0, 0, 255);
        }
    }
}

/**
 * Paint one channel: map dB→height, update peak, fill LEDs.
 *
 * Classic (modes 3–5): fixed hard zone breaks, latched palette, solid on/off —
 * no glow / no fractional tip; pure-blue peak marker after the bar.
 * Rainbow: fractional tip, short afterglow, blue peak marker.
 */
static void paint_channel(led_strip_handle_t strip, float *glow, float db, float *height_out, float *peak,
                          TickType_t *hold_until, float *disp_h, bool *disp_falling, float dt_ms,
                          float decay, Geom geom, Style style)
{
    const int span = (geom == Geom::Linear) ? LED_COUNT : (LED_COUNT / 2);
    const bool classic = (style == Style::Classic);
    float height = mapf(db, dBmin, dBmax, 0.0f, (float)span);
    height = clampf(height, 0.0f, (float)span);
    height = stabilize_height(height, disp_h, disp_falling, k_height_deadband_px);
    *height_out = height;
    update_peak(height, peak, hold_until, dt_ms, (float)span);

    const int half = LED_COUNT / 2;
    int full;
    float frac = 0.0f;

    if (classic) {
        /* Same tip tracking as rainbow: follow stabilized height (no extra lag). */
        full = (int)floorf(height + 0.5f);
        if (full > span) {
            full = span;
        }
        if (full < 0) {
            full = 0;
        }
        int gr, yr;
        classic_breaks(span, &gr, &yr);
        if (strip == stripLeft) {
            s_classic_tip_l = full;
        } else {
            s_classic_tip_r = full;
        }
        classic_sync_brightness();

        for (int i = 0; i < span; i++) {
            const bool on = (i < full);
            if (geom == Geom::Linear) {
                classic_set(strip, i, on, gr, yr, i, full);
            } else if (geom == Geom::InsideOut) {
                classic_set(strip, half - i - 1, on, gr, yr, i, full);
                classic_set(strip, half + i, on, gr, yr, i, full);
            } else { /* OutsideIn */
                classic_set(strip, i, on, gr, yr, i, full);
                classic_set(strip, LED_COUNT - i - 1, on, gr, yr, i, full);
            }
        }

        draw_peak_marker(strip, *peak, height, span, geom);
        return;
    }

    full = (int)floorf(height);
    frac = height - (float)full;

    for (int i = 0; i < span; i++) {
        float target = 0.0f;
        if (i < full) {
            target = 1.0f;
        } else if (i == full && frac > 0.0f) {
            target = frac;
        }

        uint8_t r, g, b;
        float t = (span > 1) ? ((float)i / (float)(span - 1)) : 0.0f;
        rainbow_color_at(t, &r, &g, &b);

        if (geom == Geom::Linear) {
            paint_px(strip, glow, i, r, g, b, target, decay);
        } else if (geom == Geom::InsideOut) {
            paint_px(strip, glow, half - i - 1, r, g, b, target, decay);
            paint_px(strip, glow, half + i, r, g, b, target, decay);
        } else {
            paint_px(strip, glow, i, r, g, b, target, decay);
            paint_px(strip, glow, LED_COUNT - i - 1, r, g, b, target, decay);
        }
    }

    draw_peak_marker(strip, *peak, height, span, geom);
}

/** Paint both strips with the same geometry/style and refresh. */
static void paint_stereo(float average_left, float average_right, Geom geom, Style style)
{
    float dt = dt_ms_since(&s_last_paint_tick);
    const float glow_hl =
        (style == Style::Rainbow) ? k_rainbow_afterglow_halflife_ms : afterglow_halflife_ms;
    const float decay = powf(0.5f, dt / fmaxf(glow_hl, 1.0f));

    paint_channel(stripLeft, s_glow_l, average_left, &PixelCountLeft, &previous_pixel_left,
                  &s_peak_hold_until_l, &s_disp_h_l, &s_disp_falling_l, dt, decay, geom, style);
    paint_channel(stripRight, s_glow_r, average_right, &PixelCountRight, &previous_pixel_right,
                  &s_peak_hold_until_r, &s_disp_h_r, &s_disp_falling_r, dt, decay, geom, style);

    vu_leds_show(stripLeft);
    vu_leds_show(stripRight);
}

extern "C" void vu_ballistics(float raw_left, float raw_right, float *out_left, float *out_right)
{
    float dt = dt_ms_since(&s_last_ballistic_tick);
    const float atk = 1.0f - expf(-dt / fmaxf(attack_ms, 1.0f));
    /* Modes 0–5 share fast release so classic/rainbow heads feel the same. */
    const float rel_ms = (mode >= 0 && mode <= 5) ? k_fast_release_ms : release_ms;
    const float rel = 1.0f - expf(-dt / fmaxf(rel_ms, 1.0f));

    auto step = [&](float &s, float x) {
        x = clampf(x, dBmin, dBmax + 6.0f);
        const float a = (x > s) ? atk : rel;
        s += (x - s) * a;
    };

    step(s_smooth_l, raw_left);
    step(s_smooth_r, raw_right);
    *out_left = s_smooth_l;
    *out_right = s_smooth_r;
}

void LinearClassicBar(float average_left, float average_right)
{
    paint_stereo(average_left, average_right, Geom::Linear, Style::Classic);
}

void InsideOutClassicBar(float average_left, float average_right)
{
    paint_stereo(average_left, average_right, Geom::InsideOut, Style::Classic);
}

void OutsideInClassicBar(float average_left, float average_right)
{
    paint_stereo(average_left, average_right, Geom::OutsideIn, Style::Classic);
}

void LinearRainbowBar(float average_left, float average_right)
{
    paint_stereo(average_left, average_right, Geom::Linear, Style::Rainbow);
}

void InsideOutRainbowBar(float average_left, float average_right)
{
    paint_stereo(average_left, average_right, Geom::InsideOut, Style::Rainbow);
}

void OutsideInRainbowBar(float average_left, float average_right)
{
    paint_stereo(average_left, average_right, Geom::OutsideIn, Style::Rainbow);
}

extern "C" void vu_init_strips(void)
{
    ESP_ERROR_CHECK(vu_leds_init());
    for (int i = 0; i < LED_COUNT; i++) {
        s_glow_l[i] = 0.0f;
        s_glow_r[i] = 0.0f;
    }
    s_disp_h_l = -1.0f;
    s_disp_h_r = -1.0f;
    s_disp_falling_l = false;
    s_disp_falling_r = false;
    s_classic_tip_l = -1;
    s_classic_tip_r = -1;
    /* One-shot red on onboard RGB if present; no animation loop afterward. */
    vu_leds_set_pixel(rgbLed, 0, 255, 0, 0);
    vu_leds_show(rgbLed);
}

extern "C" void vu_apply_brightness(void)
{
    /* Classic modes latch palette inside paint (classic_sync_brightness). */
    if (mode >= 3 && mode <= 5) {
        return;
    }
    int b = brightness;
    if (b < 0) {
        b = 0;
    }
    if (b > 255) {
        b = 255;
    }
    int db = b - s_brightness_applied;
    if (db < 0) {
        db = -db;
    }
    if (s_brightness_applied < 0 || db >= 3) {
        s_brightness_applied = b;
        vu_leds_set_brightness(static_cast<uint8_t>(s_brightness_applied));
    }
}

extern "C" const char *vu_mode_name(int m)
{
    switch (m) {
    case 0: return "Linear Rainbow";
    case 1: return "Inside-Out Rainbow";
    case 2: return "Outside-In Rainbow";
    case 3: return "Linear Classic";
    case 4: return "Inside-Out Classic";
    case 5: return "Outside-In Classic";
    case 6: return "Off";
    default: return "Unknown";
    }
}

extern "C" void vu_set_mode(int m)
{
    if (m < 0) {
        m = 0;
    }
    if (m > MaxMode) {
        m = MaxMode;
    }
    if (m == mode) {
        web_ui_notify_mode();
        return;
    }
    mode = m;
    ESP_LOGI(TAG, "Mode -> %d (%s)", mode, vu_mode_name(mode));
    web_ui_notify_mode();
}

extern "C" void vu_cycle_mode(void)
{
    vu_set_mode((mode + 1) % (MaxMode + 1));
}

extern "C" void vu_handle_mode_button(void)
{
    if (gpio_get_level(static_cast<gpio_num_t>(MODE_PIN)) == 0) {
        vu_cycle_mode();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

extern "C" void vu_display(float average_left, float average_right)
{
    const float l = average_left - (float)noise_level;
    const float r = average_right - (float)noise_level;
    switch (mode) {
    case 0: LinearRainbowBar(l, r); break;
    case 1: InsideOutRainbowBar(l, r); break;
    case 2: OutsideInRainbowBar(l, r); break;
    case 3: LinearClassicBar(l, r); break;
    case 4: InsideOutClassicBar(l, r); break;
    case 5: OutsideInClassicBar(l, r); break;
    default:
        /* Blank / zero bars: paint silence so afterglow and peaks settle. */
        paint_stereo(dBmin, dBmin, Geom::Linear, Style::Classic);
        break;
    }
}
