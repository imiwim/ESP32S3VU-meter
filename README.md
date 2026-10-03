**Build label:** `261003.113745`

# ESP32 VU Meter (ESP-IDF)

ESP-IDF port of the Arduino/PlatformIO project in `ESP32VU-meter_241230/`.

Stereo line-level audio is sampled on two ADC pins, converted to dB (after a linear noise-floor subtract), smoothed with attack/release ballistics, and painted on two WS2812 strips. Classic modes use fixed hard color zones from a latched palette; rainbow modes use a smooth position gradient (orange→red tip), fractional tip, afterglow, and a pure-blue peak marker. Wi‑Fi adds a mode-select web UI, OTA upload, and WebSerial. Build labels (`YYMMDD.HHMMSS`) are embedded in the firmware and stamped on sources — see [ARCHIVE.md](ARCHIVE.md).

## Hardware (stereo)

| Function | GPIO | Notes |
|----------|------|--------|
| Left audio ADC | 4 | 12-bit oneshot, mid-scale bias ~2048 |
| Right audio ADC | 5 | Same ADC unit as left + brightness |
| Brightness pot | 6 | Mapped to strip intensity 0–255 (EMA-filtered) |
| Mode button | 7 | Active-low, internal pull-up |
| Left NeoPixel | 8 | WS2812 via RMT (`led_strip`) |
| Right NeoPixel | 9 | WS2812 via RMT |
| Status LED | 10 | Active-low; 500 ms blink when Wi‑Fi has IP |
| Onboard RGB | 21 | Optional; one-shot red at init; skipped if RMT exhausted |

- **LEDs per strip:** 216 (`LED_COUNT` in `main/pins.h`)
- **Target:** ESP32-S3 (QT Py ESP32-S3 N4R2 / ESP32-S3-Zero class), 4 MB flash
- **Console / flash baud:** 921600 (`sdkconfig.defaults`, `.vscode/settings.json`)

## Setup

```bash
. $HOME/.espressif/v6.1/esp-idf/export.sh

cd /path/to/ESP32VU-meter_261004
cp credentials.h.example credentials.h   # edit SSID / password / static IP

idf.py set-target esp32s3
idf.py build
ESPBAUD=921600 idf.py -p /dev/ttyACM0 -b 921600 flash monitor
```

Or one-shot (snapshot previous build, new label, rebuild, flash+monitor):

```bash
./all.sh
```

## Signal path

1. Settled dual-read ADC on L/R (alternating start channel each window)
2. Average `|sample − midpoint|` over `average_time` ms
3. Subtract `noise_floor_adc` in the linear domain (default **80**)
4. `20·log10(VoltageFactor · mean)` → clamp to `[dBmin, dBmax]`
5. `vu_ballistics()` — attack `attack_ms`; release **80 ms** for modes 0–5 (`k_fast_release_ms`), else `release_ms`
6. `vu_display()` paints both strips with the same mode algorithm, then refreshes

## Visual FX (`main/vu_meter.cpp`)

| Effect | Behavior |
|--------|----------|
| Ballistics | Exponential attack/release on dB before paint |
| Height stabilize | Shared 0.5 px deadband for classic and rainbow tip feel |
| Peak hold / fall | Timed hold (`peak_hold_ms`), fall at `peak_fall_px_per_s` (classic + rainbow) |
| Peak marker | Pure blue `(0,0,255)`, width `endBluePixels` (classic + rainbow) |
| Fractional tip | Rainbow: leading LED brightness = fractional height |
| Afterglow | Rainbow: ~20 ms half-life + linear step (classic is hard on/off) |
| Classic zones | Fixed indices (`LED_COUNT` 216): mode 3 green `i<21`, yellow `i<108`, red `i>=108`; mode 5 half-bar `i<10` / `i<54` / `i>=54` |
| Classic paint | Latched pre-scaled palette (`vu_leds_set_pixel_raw`); pot deadband; no level-based auto-dim |
| Rainbow colors | Blue→green→orange→red; red bias on the upper tip (classic yellow palette unchanged) |

Tunables are globals in `vu_meter.cpp` / `vu_config.h` (`vu_config.h` is declarations-only; it does not include `pins.h` / `vu_leds.h`).

## Network features

After STA connects (static IP from `credentials.h`):

| URL | Purpose |
|-----|---------|
| `http://<ip>/` | Mode dropdown (WebSocket `/ws`); human-readable names; build label |
| `http://<ip>/update` | OTA firmware upload |
| `http://<ip>:81/webserial` | Browser console; send `mode` to cycle |

Mode page protocol: server pushes the mode **name**; client sends `set:N` (0…6) on dropdown change. Physical button / WebSerial also call `web_ui_notify_mode()` so the select stays in sync. Favicon returns **204**; WebSocket fds clear on close / EAGAIN. If Wi‑Fi fails, the meter still runs without HTTP/OTA/WebSerial.

## Display modes

| Mode | Name (UI / logs) |
|------|------------------|
| 0 | Linear Rainbow |
| 1 | Inside-Out Rainbow |
| 2 | Outside-In Rainbow |
| 3 | Linear Classic |
| 4 | Inside-Out Classic |
| 5 | Outside-In Classic |
| 6 | Off |

Change with the mode button, the web dropdown, or WebSerial `mode`.

## Layout

| Path | Role |
|------|------|
| `main/` | App, ADC, Wi‑Fi, HTTP/OTA/WebSerial, VU painters, LED wrapper |
| `main/pins.h` | GPIO map |
| `main/vu_meter.cpp` | Ballistics, painters, peak/afterglow FX |
| `main/vu_config.h` | Shared tunables and display API (no hardware includes) |
| `main/vu_leds.c` / `.h` | `led_strip` wrapper; scaled + raw pixel writes |
| `credentials.h` | Local secrets (gitignored; copy from `.example`) |
| `tools/latest_mtime.py` | Build label + source stamps |
| `all.sh` / `ball.sh` | Dev cycle / snapshot (see [ARCHIVE.md](ARCHIVE.md)) |
| `ESP32VU-meter_241230/` | Original PlatformIO/Arduino reference (not required to build) |

Managed component: **`espressif/led_strip`** (RMT WS2812).

## IDE (clangd)

Espressif clangd is configured via `.clangd` and `.vscode/settings.json` (`--query-driver` for the xtensa toolchain, esp-clang resource-dir). Prefer clangd over Microsoft C/C++ IntelliSense for this tree. Tick→ms conversion in `vu_meter.cpp` uses float `configTICK_RATE_HZ` math to avoid `bugprone-integer-division` on `portTICK_PERIOD_MS`.

## Source docs

Firmware sources use Doxygen-style `@file` / `@brief` and API comments under `main/`.
