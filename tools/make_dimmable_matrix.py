"""Generate lib/DimmableR4Matrix/src/DimmableR4Matrix.h from the bundled
Arduino_LED_Matrix.h, guaranteeing byte-fidelity for the pin table and GPIO
code (safety-critical charlieplex data) while adding brightness control.

Transformations:
 1. Rename static symbols with a dr4m_ prefix (no linkage collisions).
 2. Rename the class ArduinoLEDMatrix -> DimmableR4Matrix.
 3. Timer frequency 10 kHz -> 64 kHz (same duty cycle, 6.4x scan rate) so
    dimming can gate whole scan cycles without visible flicker.
 4. Add dimming state + ISR gate + setBrightness()/getBrightness().
 5. Drop the gallery.h include (icon data the clock never uses; keeps the
    fork self-contained instead of relying on the bundled library's copy).
"""
import re
import pathlib

SRC = pathlib.Path.home() / ".platformio" / "packages" / "framework-arduinorenesas-uno" / "libraries" / "Arduino_LED_Matrix" / "src" / "Arduino_LED_Matrix.h"
DST = pathlib.Path(__file__).resolve().parent.parent / "lib" / "DimmableR4Matrix" / "src" / "DimmableR4Matrix.h"

text = SRC.read_text()

# --- 5. drop the gallery.h include (icon data the clock never uses) ---
old_include = '#include "gallery.h"\n'
assert old_include in text, "gallery.h include not found"
text = text.replace(old_include, "")

# --- 1. rename static file-scope symbols ---
renames = {
    r"\bpin_zero_index\b": "dr4m_pin_zero_index",
    r"\bpins\b": "dr4m_pins",
    r"\bLED_MATRIX_PORT0_MASK\b": "DR4M_LED_MATRIX_PORT0_MASK",
    r"\bLED_MATRIX_PORT2_MASK\b": "DR4M_LED_MATRIX_PORT2_MASK",
    r"\bturnLed\b": "dr4m_turnLed",
    r"\bframebuffer\b": "dr4m_framebuffer",
    r"\breverse\b": "dr4m_reverse",
    r"\bArduinoLEDMatrix\b": "DimmableR4Matrix",
}
for pat, rep in renames.items():
    text = re.sub(pat, rep, text)

# --- 3. timer frequency: 10 kHz -> 64 kHz ---
old_timer = "TIMER_MODE_PERIODIC, type, ch, 10000.0, 50.0, turnOnLedISR, this"
new_timer = "TIMER_MODE_PERIODIC, type, ch, 64000.0, 50.0, turnOnLedISR, this"
assert old_timer in text, "timer frequency line not found"
text = text.replace(old_timer, new_timer)

# --- 2. dim state, inserted right after the framebuffer declaration ---
fb_decl = "static uint8_t __attribute__((aligned)) dr4m_framebuffer[NUM_LEDS / 8];"
assert fb_decl in text, "framebuffer declaration not found"
dim_state = fb_decl + """

// ===== Brightness control (written by setBrightness, read by the ISR) =====
// dimPercent: % of scan cycles a lit LED is driven (100 = full brightness).
// dimAccum:   Bresenham accumulator; the LED is driven on the cycle where the
//             accumulator wraps, spreading on-cycles evenly across every dim
//             level so the pulse rate stays above the flicker-fusion threshold.
static volatile uint8_t dr4m_dimPercent = 100;
static volatile uint8_t dr4m_dimAccum   = 0;"""
text = text.replace(fb_decl, dim_state)

# --- 4. ISR gate: replace the bare drive line ---
old_isr = "        dr4m_turnLed(i_isr, ((dr4m_framebuffer[i_isr >> 3] & (1 << (i_isr % 8))) != 0));"
new_isr = """        if (i_isr == 0) {
            // start of a new scan cycle: advance the Bresenham accumulator
            dr4m_dimAccum += dr4m_dimPercent;
            if (dr4m_dimAccum >= 100) {
                dr4m_dimAccum -= 100;
            }
        }
        bool lit = ((dr4m_framebuffer[i_isr >> 3] & (1 << (i_isr % 8))) != 0)
                   && (dr4m_dimAccum < dr4m_dimPercent);
        dr4m_turnLed(i_isr, lit);"""
assert old_isr in text, "ISR drive line not found"
text = text.replace(old_isr, new_isr)

# --- 5. setBrightness / getBrightness methods ---
anchor = "    void off(size_t pin) {\n        dr4m_turnLed(pin, false);\n    }"
assert anchor in text, "off() method not found"
methods = anchor + """

    // ==== Brightness control ====
    // 0-100: % of scan cycles a lit LED is driven. 100 = full brightness
    // (identical to the original driver), 15 = night-dim, 0 = display off.
    // Instant, safe to call any time from the main loop.
    void setBrightness(uint8_t percent) {
        if (percent > 100) percent = 100;
        dr4m_dimPercent = percent;
    }
    uint8_t getBrightness() {
        return dr4m_dimPercent;
    }"""
text = text.replace(anchor, methods)

# --- header comment ---
header_comment = """// ============================================================================
// DimmableR4Matrix - a patched fork of Arduino's Arduino_LED_Matrix library
// (the LED-matrix driver bundled with the UNO R4 board package), adding
// software brightness control.
//
// Original work:
//   Arduino_LED_Matrix v1.1.0, distributed inside framework-arduinorenesas-uno
//   1.5.1; canonical source:
//   https://github.com/arduino/ArduinoCore-renesas (libraries/Arduino_LED_Matrix)
//   Copyright (c) 2022-2023 Arduino SA - MIT License
//   (see the LICENSE file in this directory for the full text).
//
// Modifications by Ryfire, all applied by this generator - regenerate rather
// than edit by hand, so the pin table stays byte-identical to the bundled lib:
//   * Scan timer 10 kHz -> 64 kHz (same LED duty cycle, 6.4x scan rate)
//   * Whole-scan-cycle dimming gated by a Bresenham accumulator, exposed as
//     setBrightness()/getBrightness() (0-100 %)
//   * Class renamed ArduinoLEDMatrix -> DimmableR4Matrix; file-scope symbols
//     prefixed dr4m_ (no linkage collisions with the bundled library)
//   * gallery.h include dropped (icon data the clock never uses; keeps the
//     fork self-contained)
//
// HOW THE ORIGINAL DRIVER LIGHTS LEDs
// The 96-LED matrix is charlieplexed: only one LED can be lit at a time.
// A timer ISR fires at 10 kHz and drives one LED per tick, so a full scan
// of all 96 LEDs takes 9.6 ms and each LED gets a 100 us pulse every 9.6 ms
// (1.04% duty at 104 Hz). Brightness is proportional to that duty cycle.
//
// HOW DIMMING WORKS HERE
// Raising the timer frequency 6.4x (10 kHz -> 64 kHz) shortens each pulse to
// 15.6 us and the scan cycle to 1.5 ms - the duty cycle is unchanged (1.04%),
// so full brightness is identical to the original driver, but the scan rate
// is now 667 Hz. Dimming then gates whole scan cycles with a Bresenham
// accumulator: at 15% brightness the LED is driven on only 15 of every 100
// scan cycles, evenly spread (one every ~6-7 cycles), which keeps the pulse
// rate at ~100 Hz - well above the flicker-fusion threshold - so dimming
// looks smooth instead of strobing. Consecutive-burst gating would flicker
// visibly at 1 Hz, which is why the accumulator spreads the on-cycles out.
//
// Usage: identical to the original library, plus:
//   matrix.setBrightness(percent);   // 0-100, whole-matrix, instant
//
// Generated by tools/make_dimmable_matrix.py - regenerate rather than edit
// by hand, so the pin table always stays byte-identical to the bundled lib.
// ============================================================================

"""
text = header_comment + text

DST.parent.mkdir(parents=True, exist_ok=True)
DST.write_text(text)
print(f"wrote {DST} ({len(text)} bytes)")
