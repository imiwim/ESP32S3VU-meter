**Build label:** `261003.113745`

# Archival & reproducibility (ESP32VU-meter)

Same version-control style as **`ESP32S3_KitchenClock`**: wall-clock / mtime **`YYMMDD.HHMMSS`** labels, source stamps, `Changes.txt`, and snapshot folders — not git tags.

## Toolchain & SDK

- **ESP-IDF:** 6.1.x (`$HOME/.espressif/v6.1/esp-idf`)
- **Target:** `esp32s3`
- **Flash:** 4 MB; console **921600**
- **Managed:** `espressif/led_strip` (RMT WS2812)
- **App image:** `build/ESP32VU-meter.bin`
- **Label header:** `build/generated/project_build_info.h`
- **IDE:** Espressif clangd via `.clangd` + `.vscode/settings.json` (`--query-driver`, esp-clang resource-dir)

## Scripts

| Command | Purpose |
|---------|---------|
| **`./all.sh`** | Snapshot **PREV** label → `Changes.py` / `prompt.sh` → wall-clock `VU_BUILD_LABEL` → `idf.py fullclean reconfigure build` → resync stamps → `flash monitor` |
| **`./ball.sh`** | Repair/append `Changes.txt` → rsync snapshot at **current** label (no build) |
| **`./Changes.sh`** | Append one prompt (`CURSOR_PROMPT=…`, optional `CURSOR_RESPONSE`) |
| **`./prompt.sh append [--since-build LABEL]`** | Import Cursor agent-transcript prompts into `Changes.txt` |
| **`tools/latest_mtime.py`** | Resolve label; write `project_build_info.h`; stamp `/* BUILD_NUMBER: … */` on `main/` sources and `**Build label:**` on root `.md` |

### Typical workflows

```bash
./all.sh          # new firmware + flash + monitor
./ball.sh         # archive current tree at the running label
```

## Labels

| Where | Form |
|-------|------|
| Firmware `esp_app_desc.version` | `project(VERSION …)` from CMake = `PROJECT_BUILD_NUMBER` |
| `build/generated/project_build_info.h` | `#define PROJECT_BUILD_NUMBER "YYMMDD.HHMMSS"` |
| `main/*.{c,cpp,h}` line 1 | `/* BUILD_NUMBER: YYMMDD.HHMMSS */` |
| Root markdown | `**Build label:** \`YYMMDD.HHMMSS\`` |
| Snapshot dirs | Top-level `YYMMDD.HHMMSS/` |

### Snapshot rsync excludes

Other `YYMMDD.HHMMSS*/` trees, `build/`, `managed_components/`, `.cache/`, `.pio/`, and the Arduino reference `ESP32VU-meter_241230/`. After copy, `build/generated/project_build_info.h` is placed into the snapshot so the label remains recoverable.

### Environment

| Variable | Effect |
|----------|--------|
| `VU_BUILD_LABEL` / `PROJECT_BUILD_LABEL` | Fixed label for one configure/build |
| `BUILD_LABEL_WALL_CLOCK=1` | Use current local time as label |
| `PROMPT_BUILD_LABEL` | Build tag written into new `Changes.txt` entries |
| `CURSOR_PROMPT` / `CURSOR_RESPONSE` | Manual `Changes.sh` append |
| `ESPBAUD` / `IDF_PORT` | Serial baud / port for `all.sh` (defaults 921600, `/dev/ttyACM0`) |

## Changes.txt

Cumulative Cursor prompt / agent log. Entry shape:

```
=== Run 261003.084818 2026-10-03T08:48:18+08:00 ===
sha256: …
prompt text

--- agent ---
optional reply
```

Deduped by prompt `sha256`. Synced by `all.sh` / `ball.sh` via `prompt.sh`.

## Comparing builds

```bash
diff -ru 261003.083724/ 261003.084818/   # skip noise as needed
# Why: read Changes.txt between === Run … === headers
# Firmware: strings build/ESP32VU-meter.bin | head   # app.version near start
```

## Local secrets (not for publication)

- `credentials.h` — Wi‑Fi SSID/PSK and static IP (gitignored; use `credentials.h.example`)
- Treat LAN WebSerial as unauthenticated; do not expose beyond a trusted network

## Key files to keep in an archive

- `sdkconfig`, `sdkconfig.defaults`, `partitions/`
- `main/` (especially `vu_meter.cpp`, `vu_config.h`, `pins.h`, `vu_leds.*`, web/Wi‑Fi/ADC), root `CMakeLists.txt`, `dependencies.lock` (after configure)
- `.clangd`, `.vscode/settings.json` (clangd / serial)
- `Changes.txt`, snapshot dirs `YYMMDD.HHMMSS/`
- `ARCHIVE.md`, `README.md`
- Optional: `build/compile_commands.json` for clangd off-machine

## Firmware behavior worth recording

- Signal path: ADC average → linear `noise_floor_adc` subtract → dB → `vu_ballistics()` → `vu_display()`
- Modes 0–5: shared tip deadband (0.5 px) and fast dB release (80 ms); classic tip follows height (no extra fall lag)
- Rainbow: short afterglow (~20 ms), fractional tip, orange→red tip bias
- Classic: hard on/off zones from latched palette + `vu_leds_set_pixel_raw`; fixed breaks (mode 3: 21 / 108; mode 5: 10 / 54); pot deadband; no auto-dim by level
- Peak marker: pure blue on classic and rainbow when the hold sits above the bar
- Mode names via `vu_mode_name()`; web UI dropdown sends `set:N`; button/WebSerial notify the page
- GPIO10 status LED: 500 ms blink after Wi‑Fi GOT_IP; onboard RGB (GPIO21): one-shot red at init only
- Web UI: UTF-8 HTML, favicon **204**, persistent WS payload + sync/async mode push

## Minimal sanity checklist

- `idf.py build` succeeds; boot log shows matching build label
- Both strips respond; mode button / web dropdown / WebSerial `mode` agree on the named mode
- Silent input sits near the bottom of the bars (`noise_floor_adc`)
- Rainbow: peak markers are pure blue and clear at silence; tip is not sticky
- Classic: green/yellow/red breaks stay at fixed indices; tip feel matches rainbow
- Status LED blinks 500 ms once Wi‑Fi has an IP
- OTA page shows the running build; upload reboots into new image
- `./ball.sh` produces a small snapshot without copying `build/` or the Arduino tree
- clangd: `vu_config.h` has no unused-include warnings; tick→ms uses float `configTICK_RATE_HZ`
