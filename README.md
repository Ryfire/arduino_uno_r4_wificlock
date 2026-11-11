# UNO R4 WiFi - Smart Digital Clock

![Board](https://img.shields.io/badge/board-UNO_R4_WiFi-00878F)
![License](https://img.shields.io/badge/license-MIT-green)
![Toolchain](https://img.shields.io/badge/build-PlatformIO_%7C_Arduino_IDE-orange)

A WiFi-enabled digital clock using the Arduino UNO R4 WiFi's built-in 8×12 LED matrix. Shows real time with automatic US daylight-saving changes, a rotating time/minutes/date/year display with a scrolling day-name marquee, a WiFi signal-strength page (animated fan icon + live dBm reading), a periodic two-eyed face animation, and self-healing WiFi/NTP reconnection that hands the display to a NO WIFI warning while offline — no buttons, no configuration on the device itself.

## Features

- **Real-Time Clock**: NTP-synced (3 fallback servers) with automatic **US daylight saving** (2nd Sun Mar → 1st Sun Nov) — never needs seasonal editing
- **7-Page Rotating Display**: time → large minutes → date → year → day-name scroll → WiFi signal, each page held 7 seconds, plus a **two-eyed face** every 5th cycle
- **Night Dimming**: the whole matrix instantly drops to 15% brightness at 22:00 and back to 100% at 06:00 — no ramp, DST-aware local time, all via `#ifndef` constants
- **Sweep Transitions**: pages wipe in column-by-column from the right edge (~0.5 s) instead of hard-cuts; the date page drops its month in from the top instead
- **Time-of-Day Scenes**: each visit to the time page opens with a short animated vignette matched to the hour — sunrise, morning, day, sunset, dusk, or a moonlit night — then wipes away to reveal the clock
- **Self-Healing Network**: WiFi reconnects forever on its own (backoff 5s→10s→30s→60s); the NO WIFI banner covers the outage, and real time resumes from the last good sync on reconnect
- **NTP Resilience**: retry backoff (1→5→15 min) with round-robin server rotation; time keeps running even while offline
- **WiFi Loss Warning**: while the network is down, a full-screen **X flash** + scrolling **NO WIFI** marquee loops until the connection returns — the clock never silently pretends to be fine
- **On-Screen Status Indicators**: tiny pixels in the top corners tell you stale sync and the 24-hour tick
- **Optional 24-hour mode** via `TIME_FORMAT_24H`

---

## Quick Start

### Prerequisites (PlatformIO — recommended)

- [PlatformIO Core](https://platformio.org/install/cli) or VS Code with the PlatformIO IDE extension
- Arduino UNO R4 WiFi board
- USB-C cable (data-capable, not charge-only) for the first flash
- WiFi network with internet access

### 1. Configure Secrets

The project compiles out of the box — if `include/secrets.h` doesn't exist, it falls back to the placeholder template (WiFi will fail to connect until you add real credentials). To configure:

```bash
cd arduino_uno_r4_wificlock
cp include/secrets.example.h include/secrets.h
```

### 2. Edit `include/secrets.h`

Minimum required — just WiFi:

```cpp
const char* WIFI_SSID = "Your_WiFi_Name";
const char* WIFI_PASSWORD = "Your_WiFi_Password";
```

Optional time settings (defaults shown; `main.cpp` supplies these if you omit them):

```cpp
#define TZ_BASE_OFFSET (-5 * 3600)  // Standard-time offset (EST here)
#define TZ_USE_US_DST 1             // Automatic US daylight saving
#define TIME_FORMAT_24H 0           // 1 = 24-hour, 0 = 12-hour AM/PM
```

### 3. Build & Upload

```bash
pio run                    # Build the project
pio run --target upload    # Upload to board
pio device monitor         # View serial output (115200 baud)
```

### Using with the Arduino IDE instead

PlatformIO is the recommended toolchain, but the sketch itself is plain Arduino code, so the IDE works too:

1. Install **Arduino IDE 2.x** and the **Arduino UNO R4 WiFi** board package (Boards Manager)
2. Create a new sketch named `arduino_uno_r4_wificlock` and paste the contents of `src/main.cpp` over the generated `.ino`
3. Add these files as **tabs** in the sketch (Sketch ▸ Add File…, or drag them in):
   - `include/glyphFonts.h`
   - `include/secrets.example.h` (or your configured `secrets.h`)
   - `lib/DimmableR4Matrix/src/DimmableR4Matrix.h`
4. Install **NTPClient** (by Fabrice Weinberg) via the Library Manager
5. Select **Arduino UNO R4 WiFi** under Tools ▸ Board, pick the port, and **Upload**

No include paths need editing — the sketch resolves its headers from the tab files next to it. First upload needs USB; afterwards the clock behaves exactly as described in this README. (The wireless OTA flashing tooling mentioned in the project's history is a local-only debug extra and is not part of this repository's build.)

---

## How to Read the Display

The matrix is 12 columns × 8 rows. The clock rotates through **seven pages**, each held **7 seconds**, in this order:

### 1. Time page (7 s)

12-hour mode shows hour + AM/PM with a **blinking colon** between them; 24-hour mode shows four digits with a blinking tick pixel instead.

```
     "11 P"              "9 AM"             "2352" (24h)
  ............        ............        ............
  ............        ............        ............
  .#..#....##.        ###....#.#.#        ############
  ##.##..#.#.#        #.#.#.#.####        ..#..##....#
  .#..#....##.        ###...####.#        ############
  .#..#..#.#..        ..#.#.#.##.#        #....#..##..
  ######...#..        ###...#.##.#        ############
  ............        ............        ............
     dots between       dots after          tick pixel
        11 and P           the 9          replaces colon
```

The colon dots sit in the **blank cell after the hour** (`11 · P` → dots between `11` and `P`; `9 · AM` → dots after the `9`), so they never merge into a digit.

Before the clock appears, every visit to this page opens with a short **time-of-day scene** (~2.5 s) picked from the clock's local hour:

| Hours (local) | Scene |
|---------------|-------|
| 21:00–04:59 | **night** — crescent moon over the ground, four stars twinkling |
| 05:00–06:59 | **sunrise** — the sun climbs out of the ground while stars wink out |
| 07:00–10:59 | **morning** — a small sun with pulsing rays, a cloud drifting by |
| 11:00–16:59 | **day** — full sunburst, cardinal rays pulsing |
| 17:00–18:59 | **sunset** — the sun sinks behind the ground under thin clouds |
| 19:00–20:59 | **dusk** — the afterglow dome sinks back into the horizon while stars pop in one by one |

The scene then wipes away with the usual column sweep to reveal the clock:

```
        night                 sunrise                dusk
  ....#.......        ....#.......        ............
  ..#.....##..        ..#.........        ..#.........
  .......##...        ............        ............
  ....#..##...        ....#.......        ....#.......
  .......##...        ............        ............
  .#......##..        .....##.....        .#.######...
  ............        ....####....        ..########..
  ############        ############        ############
```

The scenes are procedural (drawn by the same frame pipeline as every other page), and `SCENE_FORCE` at the top of `src/main.cpp` lets you preview any bucket on the bench without waiting for the real hour.

**Choosing the style** — `SCENE_STYLE` (declared with a default in `src/main.cpp`, overridable from `secrets.h`) picks what plays during that ~2.5 s window:

| Value | Behavior |
|-------|----------|
| `2` | **Animated scenes** (default) — motion, twinkles, drifting clouds |
| `1` | **Static image art** — one purpose-drawn still per bucket (`SCENE_ART`), every star lit and every ray on |
| `0` | **No intro** — the time page wipes straight to the clock |

### 2. Minutes page (7 s)

The current minutes in **large full-height digits**. This is the only page with tall glyphs — the size difference is how you tell it from everything else. (`05` shown; tens at left, gap, ones at right.)

```
............
.###...#####
#...#..#....
#...#..####.
#...#......#
#...#......#
#...#..#...#
.###....###.
```

### 3. Date page (7 s) — drop-in month, then alternating flip

The page opens with a **drop-in entrance**: on a dark screen, the month pair falls from above the top edge — one row every 70 ms — and lands in the centered slots. It sits alone for **3 seconds**, and only then does the regular month ↔ day flip begin, trading places every 350 ms:

```
   falling in             landed, holds 3 s
...#....#..              ............
..##...##..              ............
...#....#..              ...#....#..
...#....#..              ..##...##..
..###..###..             ...#....#..
............             ...#....#..
............             ..###..###..
............             ............
```

Both halves use the **same full-size digits as the time page**, at the same centered position the year page uses — there are no shrunken glyphs anywhere. After the ~0.5 s fall and the 3 s month-only hold, each half gets ~5 looks during the rest of the slot, and the entrance replays on every visit to the page. Knobs: `DATE_DROP_MS` (fall speed), `DATE_HOLD_MS` (month-only hold), `DATE_FLIP_MS` (flip rate).

### 4. Year page (7 s)

Two-digit year, centered with a gap:

```
............
............
..###..###..
....#..#....
..###..###..
..#....#.#..
..###..###..
............
     "2 6"
```

### 5. Day-name scroll (7 s)

The day of the week scrolls across the display in **large letters** — the same 7×5 size as the minutes page — entering from the right edge, crossing the screen, and exiting left. At 90 ms per column, WEDNESDAY (the longest name) takes about 6.6 s of the 7 s slot; shorter names finish earlier and the display sits blank for the remainder of the phase.

```
entering ( MONDAY )    mid-word ( MONDAY )    exiting ( MONDAY )
#...#...###.           #...#..####.           .###...#...#
##.##..#...#           ##..#..#...#           #...#..#...#
#.#.#..#...#           #.#.#..#...#           #...#...#.#.
#...#..#...#           #..##..#...#           #####....#..
#...#..#...#           #...#..#...#           #...#....#..
#...#..#...#           #...#..#...#           #...#....#..
#...#...###.           #...#..####.           #...#....#..
............           ............           ............
    "MO"                   "ND"                   "AY"
```

The letters are read two at a time at the same columns as the minutes page (tens at cols 0–4, ones at cols 7–11), so the word scrolls in pairs: `MO` → `ON` → `ND` → `DA` → `AY`.

### 6. WiFi signal (7 s)

Signal strength of the connected network. The page opens with a **fan build-up**: the dot appears first, then the arcs light up one by one bottom-to-top — but only as many as the current signal quality supports, so the animation doubles as the meter:

| RSSI | fan builds up to |
|------|------------------|
| −60 dBm or stronger | dot + all 3 arcs |
| −70 … −61 dBm | dot + 2 arcs |
| −80 … −71 dBm | dot + 1 arc |
| weaker than −80 dBm | dot only |

After the build-up (~1.4 s) and a short hold, the live **RSSI in dBm** replaces the icon — and then the slot never sits still: the number first **counts up from −100** like a meter locking onto the signal, then alternates between holding the real reading and a **quick fan re-pulse**:

```
   fan build-up           number "-57"
.....##.....        ............
...##..##...        ............
..#......#..        ....###.###.
....####....        ....#.....#.
...#....#...        ##..###..#..
.....##.....        ......#..#..
....#..#....        ....###..#..
.....##.....        ............
```

The minus is a short bar tucked against the left edge, followed by a 2-column gap before the digits — the font's full-width dash glyph would otherwise fuse into the first digit — and the two digits carry a 1-column space between them. Readings of −100 dBm or weaker drop the minus and just center the digits with the same spacing; the value is definitionally negative on this page.

The timing knobs are `WIFI_BARS_MS` (350 ms per buildup step), `WIFI_BARS_HOLD` (600 ms the full icon stays before the number), `WIFI_COUNT_MS` (900 ms count-up), `WIFI_NUM_MS` (1600 ms per number hold) and `WIFI_PULSE_MS` (500 ms fan re-pulse). When WiFi is down this page doesn't run at all — the WiFi loss banner below takes over instead.

### WiFi loss banner (display override)

Not one of the timed pages: whenever the WiFi connection is down — at boot or after a dropout — it replaces the entire display until the connection returns, with no clock in between:

1. A full-screen **X flashes** three times (~2 s)
2. **NO WIFI** scrolls across in the same large letters as the day-name page
3. The loop restarts immediately

The moment WiFi reconnects, the normal page cycle resumes automatically. Timing knobs: `WARN_X_ON_MS` / `WARN_X_OFF_MS` / `WARN_X_FLASHES` for the flash phase and `WARN_SCROLL_MS` for the marquee speed.

### 7. Two-eyed face (every 5th cycle, ~5 s)

After the WiFi page, a two-eyed face appears — two "0" glyphs filling the whole height, with a 1-wide × 2-tall vertical pill pupil inside each. The pupils look left, then right, from a center rest, then the eyes blink closed briefly. It plays once every 5th full cycle (~every 4 minutes) and simply doesn't appear in the other four cycles.

```
   center        left         right        blink
.###....###.  .###....###.  .###....###.  .###....###.
#...#..#...#  #...#..#...#  #...#..#...#  #...#..#...#
#...#..#...#  #...#..#...#  #...#..#...#  #...#..#...#
#.#.#..#.#.#  ##..#..##..#  #..##..#..##  #####..#####
#.#.#..#.#.#  ##..#..##..#  #..##..#..##  #####..#####
#...#..#...#  #...#..#...#  #...#..#...#  #...#..#...#
.###....###.  .###....###.  .###....###.  .###....###.
............  ............  ............  ............
```

The pupils are drawn in the middle column of each eye's interior, then shifted one column left and one column right for the left/right poses, then closed with a full interior bar. If the `EYE_EVERY_N_CYCLES` constant is set to `0`, the face is disabled and the cycle stays at six pages.

### Page-identity cheat sheet

| What you see | Which page |
|--------------|------------|
| 2–4 small digits + AM/PM + blinking colon dots | **Time** |
| A tiny landscape — moon, rising/sinking sun, stars — then it wipes to the clock | **Time page scene intro** (first ~2.5 s of the Time page) |
| Huge digits filling the whole height | **Minutes** |
| A 2-digit number that drops in from the top, holds 3 s, then flips every ~350 ms | **Date** |
| `2 6`-style pair, static, centered | **Year** |
| A word in large letters scrolling across the screen | **Day-name scroll** |
| A fan builds up, a number counts up from −100, then number and fan alternate | **WiFi signal** |
| Full-screen X flashing, then NO WIFI scrolling, in a loop | **WiFi loss banner (override)** |
| A two-eyed face with pupils looking around, blinking every ~4 min | **Two-eyed face** |

### On-Screen Status Indicators

Small pixels in the top rows that no page ever uses:

| Pixel | Meaning |
|-------|---------|
| Top row, right of center — **solid** | Stale: no successful sync for over 1 hour (time is drifting) |
| Second row, left of center — **blinking** | 24-hour mode only: replaces the colon |

---

## Night Dimming & Sweep Transitions

### Night dimming (instant schedule switch)

Between 22:00 and 06:00 (local hours, DST-aware), the whole matrix instantly switches to 15% brightness — no ramp, so the change is unmistakable. At 06:00 it jumps back to full brightness. The schedule is a no-op until the first NTP sync, so the NO WIFI banner always runs at full brightness.

The brightness control is implemented by a local fork of the bundled LED matrix driver (`lib/DimmableR4Matrix/`): the scan timer runs at 64 kHz instead of 10 kHz, and a Bresenham accumulator gates whole scan cycles, so dimming is flicker-free at any level while full brightness is identical to the stock driver.

| Constant | Default | Meaning |
|----------|---------|---------|
| `DIM_START_HOUR` | 22 | local hour the night window starts |
| `DIM_END_HOUR` | 6 | local hour the night window ends |
| `DIM_NIGHT_PERCENT` | 15 | brightness % during the night window |
| `DIM_DAY_PERCENT` | 100 | brightness % during the day |

All four are `#ifndef`-guarded — override them in `include/secrets.h` if you want a different schedule.

### Sweep transitions

On every page change — except the day-name scroll (its marquee entrance is already motion) and the date page (which falls in from the top instead, see the date section) — the outgoing page is wiped away as the incoming page is revealed behind it: a column-by-column wipe from the right edge, taking ~0.5 s (`SWEEP_MS` = 40 ms per column).

---

## How to Use It

There is nothing to operate: **plug it in and wait**. Typical power-up:

1. **Boots** (~2 s), tries to join WiFi (bounded to ~15 s)
2. **First NTP sync** attempt as soon as WiFi is up
3. **Display starts**: WiFi + sync both OK → real time and the full 7-page cycle; WiFi up but no sync yet → blinking `SYNC`; WiFi down → the looping **NO WIFI banner**

Everyday behavior:

- **Leave it alone.** Time, date, and DST changes happen automatically over the network.
- **WiFi drops?** The display switches to the **NO WIFI banner** — flashing X + scrolling NO WIFI on loop — until the connection returns. Reconnection is automatic, and real time resumes from the last good sync the moment WiFi is back.
- **Router off at boot?** The NO WIFI banner loops while the clock silently retries; it switches to real time by itself the moment WiFi + NTP both succeed.
- **Daylight saving?** Applied automatically on the correct US dates; the displayed time jumps at 2 AM on the boundary nights.
- **Power cycle?** It re-syncs from NTP on boot; no battery backup of settings is needed.

After a sync the display shows:

| Actual moment | Time page | Minutes page | Date page | Year page | Day-name scroll | WiFi signal |
|---------------|-----------|--------------|-----------|-----------|-----------------|-------------|
| Sun Nov 8, 2026, 9:05 AM | `9 AM` | `05` | `11` ↔ `08` | `2 6` | `SUNDAY` | fan → `-57` |
| Tue Mar 9, 2027, 10:15 PM | `10 P` | `15` | `03` ↔ `09` | `2 7` | `TUESDAY` | fan → `-71` |

---

## Project Structure

```
arduino_uno_r4_wificlock/
├── platformio.ini              # Build configuration
├── LICENSE                     # MIT license for this project's code
├── NOTICE                      # Third-party component notices
├── src/
│   └── main.cpp               # Application entry point (FSM + all logic)
├── include/
│   ├── secrets.h              # Your credentials (gitignored, NEVER commit)
│   ├── secrets.example.h      # Template for all config fields
│   └── glyphFonts.h           # All glyph data: compact 5×3 (digits, A-Z, space, colon, dash, !) + large 7×5 (digits, A-Z)
├── lib/
│   └── DimmableR4Matrix/      # Patched fork of the bundled LED matrix driver with brightness control (MIT, © Arduino SA — see its README)
├── tools/                      # Dev utilities (driver generator, font checks, README art generator)
└── README.md                   # This file
```

> **Important:** `include/secrets.h` contains your private WiFi credentials. It is listed in `.gitignore` and must **never** be committed. Copy `include/secrets.example.h` as the template.

---

---

## State Machine Architecture

```
INITIALIZING ──first loop──▶ TIME_DISPLAY (primary, default)
                                │
                                │ 3 consecutive NTP failures
                                ▼
                          ERROR_DISPLAY
                          (blink ERR! 2.4s)
                                │
                                ▼
                          Return to TIME_DISPLAY
```

**State descriptions:**
- **`TIME_DISPLAY`**: Renders the page cycle, services the WiFi reconnect FSM, and schedules NTP syncs with backoff. While the network is down, the NO WIFI banner overrides the display until the connection is back.
- **`ERROR_DISPLAY`**: 2.4-second `ERR!` blink (3 × 400 ms) after every 3rd consecutive NTP failure, then returns to the clock. The time keeps running underneath.
- **`INITIALIZING`**: One-pass startup state; `setup()` does the (bounded) blocking connect and first sync attempt.

### Network behavior

- **WiFi loss**: reconnect attempts forever with backoff (5 s → 10 s → 30 s → 60 s cap). While offline, the NO WIFI banner replaces the display; the stale pixel only matters when WiFi is up but the sync is more than 1 h old.
- **Boot without WiFi**: the NO WIFI banner loops while the clock retries in the background and switches to real time automatically once both WiFi and NTP succeed.
- **NTP unreachable**: retries at 1 min → 5 min → 15 min, rotating through 3 fallback servers; no `ERR!` loop, no display freezes beyond the ≤1 s NTP round-trip.

---

## Character Set

All glyphs are **5 rows × 3 columns** in `PROGMEM`:

| Type | Characters |
|------|-------------|
| Digits | `0-9` |
| Letters | `A-Z` (auto-converted to uppercase) |
| Space | ` ` (blank, for spacing) |
| Colon | `:` (time separator, blinks every second) |
| Dash | `-` |
| Exclamation | `!` |

---

## Timezone & Daylight Saving

Configuration lives in `include/secrets.h` (optional — defaults are Eastern US with auto-DST):

| Define | Default | Purpose |
|--------|---------|---------|
| `TZ_BASE_OFFSET` | `-5 * 3600` | Standard-time offset from UTC |
| `TZ_USE_US_DST` | `1` | Apply US DST rules automatically |
| `TIME_FORMAT_24H` | `0` | `1` = 4-digit 24-hour, `0` = AM/PM |

**Common offsets:**

| Timezone | Value |
|----------|-------|
| UTC | `0 * 3600` |
| EST / EDT | `-5 * 3600` (DST applied automatically) |
| CST / CDT | `-6 * 3600` |
| MST / MDT | `-7 * 3600` |
| PST / PDT | `-8 * 3600` |
| CET / CEST | `1 * 3600` (set `TZ_USE_US_DST 0` — EU rules differ) |

DST boundaries follow the US rule: **2:00 AM on the 2nd Sunday of March → 2:00 AM on the 1st Sunday of November**.

---

## Troubleshooting

### Build Errors

| Error | Solution |
|-------|----------|
| Clock shows fake time, WiFi never connects | `secrets.h` missing or has placeholder credentials — the project *still compiles* (it falls back to `secrets.example.h`), so just copy the example: `cp include/secrets.example.h include/secrets.h` |
| `WiFiS3 not found` | Check `platformio.ini` has `board = uno_r4_wifi` |
| Board not detected | Ensure USB cable is data-capable |

### Runtime Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| X flashing + `NO WIFI` scrolling, looping | WiFi is down (boot failure or dropout) | Restore the router / fix credentials; reconnection is automatic and the pages resume |
| Top-right pixel solid | No successful sync for >1 h | Check internet/NTP access; time is drifting |
| `ERR!` blinks briefly | 3 consecutive NTP failures | Normal if internet hiccups; rotates servers automatically |
| `WAIT` after boot | WiFi up but first sync failed | Keeps retrying in the background |
| `FAIL` at boot | WiFi never connected | Check credentials; the banner loops until it connects |
| Clock 1 hour off | Wrong `TZ_BASE_OFFSET` or `TZ_USE_US_DST 0` | Fix the define in `secrets.h` |

### Serial Monitor Output (Normal Boot)

```
UNO R4 WiFi Smart Clock
Connecting to WiFi...
Connecting to: Your_WiFi_Name
[WiFi] Connected
[WiFi] IP Address: 192.168.1.100
NTP Client started
Time synchronized: 09:05:00
[Dim] full brightness 100% at hour 9
Setup complete. Entering state machine...
State: TIME_DISPLAY
```

At 22:00 the same log line shows `[Dim] night dim 15% at hour 22` (and back to full brightness at 06:00).

---

## Adding Custom Symbols

To add a new character (e.g., degree symbol `°`):

1. **Add pattern** to `include/glyphFonts.h` (compact 5×3 family — digits, letters, and punctuation live here):
```cpp
static const uint8_t compactDegree[5][3] PROGMEM = {
  {1,1,0},
  {1,1,0},
  {0,0,0},
  {0,0,0},
  {0,0,0}
};
```

2. **Add to renderer** in `src/main.cpp`, inside `renderCompactChar()`:
```cpp
} else if (c == '°') {
  pattern = (const uint8_t (*)[3])compactDegree;
}
```

> **Critical:** Every symbol must be exactly `[5][3]` (5 rows × 3 columns) stored in `PROGMEM`. Any other size breaks the frame buffer math.

---

## Libraries Used

All managed automatically by PlatformIO via `lib_deps` in `platformio.ini`:

| Library | Purpose |
|---------|---------|
| `DimmableR4Matrix` | Patched fork of Arduino's bundled LED matrix driver, adds `setBrightness()` — see `lib/DimmableR4Matrix/` for provenance |
| `WiFiS3` | WiFi connectivity (ships with the UNO R4 board package) |
| `NTPClient` | Network Time Protocol client (PlatformIO pulls it via `lib_deps`) |

## License & Credits

This project's code is released under the [MIT License](LICENSE) — © 2026 Ryfire.

Third-party components keep their own notices (see [NOTICE](NOTICE)):

- **DimmableR4Matrix** (`lib/DimmableR4Matrix/`) is a patched fork of Arduino's [Arduino_LED_Matrix](https://github.com/arduino/ArduinoCore-renesas/tree/main/libraries/Arduino_LED_Matrix) v1.1.0 — MIT License, © 2022-2023 Arduino SA
- **NTPClient** ([arduino-libraries/NTPClient](https://github.com/arduino-libraries/NTPClient)) — MIT License, © Fabrice Weinberg

---

## Ideas for Extension

- Brightness control via ambient light sensor (auto-dim on top of the schedule)
- Countdown timer or stopwatch mode
- Multiple timezone quick-view
