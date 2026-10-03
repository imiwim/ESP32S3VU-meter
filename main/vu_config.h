/* BUILD_NUMBER: 261003.113745 */
/**
 * @file vu_config.h
 * @brief Shared VU-meter state, calibration, and display API.
 *
 * Globals mirror the Arduino/PlatformIO port so app_main, ADC, web UI, and
 * painting code can share the same names. Display modes (0…MaxMode):
 *  - 0 Linear rainbow
 *  - 1 Inside-out rainbow
 *  - 2 Outside-in rainbow
 *  - 3 Linear classic (solid color bands)
 *  - 4 Inside-out classic
 *  - 5 Outside-in classic
 *  - 6 Off (blank / zero bars)
 *
 * Visual FX live in vu_meter.cpp: shared tip deadband / fast release for modes
 * 0–5; rainbow fractional tip + short afterglow; classic latched hard zones;
 * pure-blue peak marker on classic and rainbow. Tunables below adjust feel.
 *
 * Declarations only — no pins.h / vu_leds.h (include those where hardware is used).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Strip intensity 0–255 from brightness pot (updated each loop). */
extern int brightness;

/** DC bias of the audio ADC (~mid-scale for 12-bit = 2048). */
extern int ADC_midpoint;

/** Active display mode index (0…MaxMode). */
extern int mode;

/** Highest valid mode index (inclusive). */
extern int MaxMode;

/** Advance @c mode by one (wraps). Used by button, Web UI, WebSerial. */
void vu_cycle_mode(void);

/**
 * @brief Set display mode to @p m (clamped to 0…MaxMode) and notify the Web UI.
 */
void vu_set_mode(int m);

/**
 * @brief Human-readable name for a display mode index.
 * @param m Mode 0…MaxMode (unknown → "Unknown").
 * @return Static string suitable for UI / logs (never NULL).
 */
const char *vu_mode_name(int m);

/** Averaging window for stereo ADC magnitude, in milliseconds. */
extern int average_time;

/**
 * Color-band boundaries as fixed pixel indices for LED_COUNT (216).
 * Mode 3: green i<21, yellow i<108, red i>=108.
 * Mode 5: green i<10, yellow i<54, red i>=54 (half-strip consts in vu_meter.cpp).
 */
extern int zeroRange;
extern int blueRange;
extern int greenRange;
extern int yellowRange;
extern int redRange;

/** Peak-hold marker width in pixels (drawn pure blue 0,0,255). */
extern int endBluePixels;

/** Upper dB of the display map (full scale → LED_COUNT). */
extern float dBmax;

/** Lower dB of the display map (silence floor after noise subtract). */
extern float dBmin;

/** Extra dB trim applied after linear noise-floor subtract (usually 0). */
extern int noise_level;

/**
 * Mean |ADC| counts with silent/open input; subtracted in linear domain
 * before dB conversion so a quiet line sits at dBmin.
 */
extern float noise_floor_adc;

/** Attack time constant (ms) — how fast the bar rises toward a louder sample. */
extern float attack_ms;

/**
 * Release time constant (ms) — fallback when not in modes 0–5.
 * Active display modes use a faster internal constant in vu_ballistics().
 */
extern float release_ms;

/** How long the pure-blue peak marker holds at its max before falling (ms). */
extern float peak_hold_ms;

/** Pure-blue peak marker fall speed after hold expires (pixels per second). */
extern float peak_fall_px_per_s;

/**
 * Afterglow half-life (ms) for rainbow pixels above the bar.
 * Classic modes do not use afterglow; rainbow paint may use a shorter constant.
 */
extern float afterglow_halflife_ms;

/** Smoothed bar heights in pixels (diagnostic / UI); updated each paint. */
extern float PixelCountLeft;
extern float PixelCountRight;

/** Pure-blue peak marker positions in pixels (float). */
extern float previous_pixel_left;
extern float previous_pixel_right;

/**
 * Palette rows: {R,G,B}
 * 0 black, 1 blue, 2 green, 3 yellow, 4 red.
 */
extern int colors[5][3];

/**
 * Scale from linear ADC magnitude to voltage-like units used in
 * 20*log10(VoltageFactor * mean). Tuned for the original Arduino sketch.
 */
extern float VoltageFactor;

/**
 * @brief Apply attack/release ballistics to raw dB samples (call each frame).
 * @param raw_left   Instantaneous left dB.
 * @param raw_right  Instantaneous right dB.
 * @param[out] out_left   Smoothed left dB.
 * @param[out] out_right  Smoothed right dB.
 */
void vu_ballistics(float raw_left, float raw_right, float *out_left, float *out_right);

/**
 * @brief Dispatch L/R dB levels to the active mode painter (expects smoothed dB).
 */
void vu_display(float average_left, float average_right);

/** Create LED strips and light onboard RGB once. */
void vu_init_strips(void);

/** Push global @c brightness into the LED wrapper. */
void vu_apply_brightness(void);

/** Poll MODE_PIN; on press, cycle mode with debounce delay. */
void vu_handle_mode_button(void);

#ifdef __cplusplus
}

/** Classic solid-band bar, base→tip. */
void LinearClassicBar(float average_left, float average_right);

/** Classic bands growing from strip center outward. */
void InsideOutClassicBar(float average_left, float average_right);

/** Classic bands growing from both ends toward center. */
void OutsideInClassicBar(float average_left, float average_right);

/** Rainbow gradient bar, base→tip. */
void LinearRainbowBar(float average_left, float average_right);

/** Rainbow growing from center outward. */
void InsideOutRainbowBar(float average_left, float average_right);

/** Rainbow growing from ends toward center. */
void OutsideInRainbowBar(float average_left, float average_right);

#endif
