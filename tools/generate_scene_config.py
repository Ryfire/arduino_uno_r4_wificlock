#!/usr/bin/env python3
"""Generate include/scene_config.h from scene_config.json.

scene_config.json (repo root) is the source of truth for the clock's
display sequence (order, durations, on/off), the scene intro length and
style default, and the hour -> scene schedule. This script validates it
and emits the C++ table the firmware walks.

Runs automatically before every PlatformIO build (see platformio.ini
`extra_scripts = pre:...`) and can also be run by hand:

    python tools/generate_scene_config.py
    python tools/generate_scene_config.py --json other.json --out other.h

The output is written only when its content changes, so untouched configs
do not trigger spurious rebuilds.
"""

import argparse
import json
import os
import sys

def project_root():
    """Repo root, resolved lazily: PlatformIO execs this file as a pre-script
    WITHOUT __file__ (it runs in the caller's globals), so fall back to the
    build environment's PROJECT_DIR, then the cwd."""
    path = globals().get("__file__")
    if path:
        return os.path.dirname(os.path.dirname(os.path.abspath(path)))
    env = globals().get("env")
    if env is not None and env.get("PROJECT_DIR"):
        return env["PROJECT_DIR"]
    return os.getcwd()


def default_json():
    return os.path.join(project_root(), "scene_config.json")


def default_out():
    return os.path.join(project_root(), "include", "scene_config.h")

# Phase names allowed in sequence steps -> generated enum ids (order must
# match the DisplayPhase enum in src/main.cpp).
PHASES = ["time", "minutes", "date", "year", "day", "wifi", "eye"]
PHASE_ENUM = {p: "GEN_PHASE_" + p.upper() for p in PHASES}

# Scene names allowed in scene_map entries -> index used by SceneKind in
# src/main.cpp (order must match the SceneKind enum).
SCENES = ["night", "sunrise", "morning", "day", "sunset", "dusk"]

# Per-phase detail knobs (page-INTERNAL animation timing), in ms.
# field name -> (min, max, default)
DETAIL_FIELDS = {
    "time": {"blink_ms": (100, 5000, 1000)},           # colon blink period
    "date": {"hold_ms": (250, 10000, 2000),            # month alone before flipping
             "month_ms": (100, 5000, 600),             # month state in the flip
             "slash_ms": (100, 2000, 300),             # '/' divider state
             "day_ms": (100, 5000, 900)},              # day state (longer: the useful half)
    "day":  {"scroll_ms": (20, 1000, 90)},             # day-name marquee speed
    "eye":  {"step_ms": (100, 5000, 700),              # pose hold per step
             "blink_ms": (50, 3000, 400)},             # closed-lid duration
    "wifi": {"bars_ms": (50, 3000, 350),               # fan build-up per step
             "bars_hold_ms": (0, 5000, 600),           # full-icon hold
             "count_ms": (100, 5000, 900),             # dBm count-up
             "num_ms": (200, 10000, 1600),             # number hold
             "pulse_ms": (100, 5000, 500)},            # fan re-pulse
}
# struct field name per detail key (PhaseStep members)
DETAIL_STRUCT_FIELD = {
    "blink_ms": "blinkMs", "hold_ms": "holdMs", "month_ms": "monthMs",
    "slash_ms": "slashMs", "day_ms": "dayMs",
    "scroll_ms": "scrollMs", "step_ms": "stepMs", "bars_ms": "barsMs",
    "bars_hold_ms": "barsHoldMs", "count_ms": "countMs",
    "num_ms": "numMs", "pulse_ms": "pulseMs",
}
# canonical field order for the emitted table rows
DETAIL_ORDER = ["blinkMs", "holdMs", "monthMs", "slashMs", "dayMs", "scrollMs",
                "stepMs", "barsMs", "barsHoldMs", "countMs", "numMs", "pulseMs"]

MIN_DURATION_MS = 500
MAX_DURATION_MS = 3600000


class ConfigError(Exception):
    """User-facing validation error: message points at the offending field."""


def fail(msg):
    raise ConfigError(msg)


def as_bool(value, where):
    if not isinstance(value, bool):
        fail("%s must be true or false" % where)
    return value


def as_int(value, where, lo, hi):
    if isinstance(value, bool) or not isinstance(value, int):
        fail("%s must be an integer" % where)
    if not lo <= value <= hi:
        fail("%s must be between %d and %d (got %d)" % (where, lo, hi, value))
    return value


def load_config(path):
    try:
        with open(path, "r", encoding="utf-8-sig") as fh:
            return json.load(fh)
    except FileNotFoundError:
        fail("config not found: %s" % path)
    except json.JSONDecodeError as exc:
        fail("%s is not valid JSON: %s (line %d)" % (path, exc.msg, exc.lineno))


def validate_sequence(cfg):
    steps = cfg.get("sequence")
    if not isinstance(steps, list) or not steps:
        fail("'sequence' must be a non-empty list of steps")
    seen = set()
    for i, step in enumerate(steps):
        where = "sequence[%d]" % i
        if not isinstance(step, dict):
            fail("%s must be an object" % where)
        phase = step.get("phase")
        if phase not in PHASES:
            fail("%s.phase must be one of: %s (got %r)"
                 % (where, ", ".join(PHASES), phase))
        if phase in seen:
            fail("%s: phase %r appears more than once" % (where, phase))
        seen.add(phase)
        step["duration_ms"] = as_int(step.get("duration_ms"), where + ".duration_ms",
                                     MIN_DURATION_MS, MAX_DURATION_MS)
        step["enabled"] = as_bool(step.get("enabled", True), where + ".enabled")
        # per-phase detail knobs (page-internal animation timing)
        allowed = DETAIL_FIELDS.get(phase, {})
        for key, (lo, hi, dflt) in allowed.items():
            step[key] = as_int(step.get(key, dflt), where + "." + key, lo, hi)
        for key in step:
            if key in ("phase", "duration_ms", "enabled", "every_n_cycles"):
                continue
            if key not in allowed:
                fail("%s: field '%s' is not valid for phase '%s' (allowed: %s)"
                     % (where, key, phase,
                        ", ".join(allowed) if allowed else "(none)"))
        if phase == "eye":
            step["every_n_cycles"] = as_int(step.get("every_n_cycles", 5),
                                            where + ".every_n_cycles", 0, 100)
        elif "every_n_cycles" in step:
            fail("%s: 'every_n_cycles' is only valid on the 'eye' phase" % where)
    if not any(step["enabled"] for step in steps):
        fail("'sequence' must have at least one enabled step")
    return steps


def validate_scene_globals(cfg):
    intro = as_int(cfg.get("scene_intro_ms", 2500), "scene_intro_ms", 0, 60000)
    style = as_int(cfg.get("scene_style", 2), "scene_style", 0, 2)
    return intro, style


def build_hour_map(cfg):
    """Merge scene_map ranges into a 24-entry hour -> scene index table."""
    scene_map = cfg.get("scene_map")
    if not isinstance(scene_map, list) or not scene_map:
        fail("'scene_map' must be a non-empty list of ranges")
    hour_map = [-1] * 24
    for i, entry in enumerate(scene_map):
        where = "scene_map[%d]" % i
        if not isinstance(entry, dict):
            fail("%s must be an object" % where)
        scene = entry.get("scene")
        if scene not in SCENES:
            fail("%s.scene must be one of: %s (got %r)"
                 % (where, ", ".join(SCENES), scene))
        from_hour = as_int(entry.get("from_hour"), where + ".from_hour", 0, 23)
        to_hour = as_int(entry.get("to_hour"), where + ".to_hour", 0, 24)
        count = (to_hour - from_hour) % 24
        if count == 0:
            fail("%s covers zero hours (from_hour == to_hour)" % where)
        for offset in range(count):
            hour = (from_hour + offset) % 24
            if hour_map[hour] != -1:
                fail("%s: hour %d:00 is already claimed by another range"
                     % (where, hour))
            hour_map[hour] = SCENES.index(scene)
    gaps = [h for h, s in enumerate(hour_map) if s == -1]
    if gaps:
        fail("scene_map leaves hours uncovered: %s"
             % ", ".join("%d:00" % h for h in gaps))
    return hour_map


def render_header(steps, intro_ms, style, hour_map):
    lines = []
    lines.append("// GENERATED FILE — do not edit.")
    lines.append("// Source of truth: scene_config.json (repo root). Edit it by hand or via")
    lines.append("// tools/studio/index.html; `pio run` regenerates this file automatically")
    lines.append("// (tools/generate_scene_config.py runs as a PlatformIO pre-build script).")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append("// Display phase ids; order must match the DisplayPhase enum in src/main.cpp.")
    lines.append("enum GeneratedPhase : uint8_t {")
    for phase in PHASES:
        lines.append("  %s," % PHASE_ENUM[phase])
    lines.append("};")
    lines.append("")
    lines.append("// One step of the display rotation: what shows, for how long, whether it")
    lines.append("// plays at all, plus the page-INTERNAL animation timing (detail fields")
    lines.append("// apply only to their own phase; unused ones stay 0).")
    lines.append("// everyNCycles is only used by the eye phase (0 = never).")
    lines.append("struct PhaseStep {")
    lines.append("  uint8_t phase;")
    lines.append("  unsigned long durationMs;")
    lines.append("  bool enabled;")
    lines.append("  uint8_t everyNCycles;")
    lines.append("  unsigned int blinkMs;     // time: colon blink / eye: closed-lid")
    lines.append("  unsigned int holdMs;      // date: month alone before flipping")
    lines.append("  unsigned int monthMs;    // date: month state in the flip")
    lines.append("  unsigned int slashMs;    // date: '/' divider state")
    lines.append("  unsigned int dayMs;      // date: day state (the useful half)")
    lines.append("  unsigned int scrollMs;    // day: marquee speed")
    lines.append("  unsigned int stepMs;      // eye: pose hold per step")
    lines.append("  unsigned int barsMs;      // wifi: fan build-up per step")
    lines.append("  unsigned int barsHoldMs;  // wifi: full-icon hold before the number")
    lines.append("  unsigned int countMs;     // wifi: dBm count-up")
    lines.append("  unsigned int numMs;       // wifi: number hold between pulses")
    lines.append("  unsigned int pulseMs;     // wifi: fan re-pulse")
    lines.append("};")
    lines.append("")
    lines.append("const PhaseStep SCENE_SEQUENCE[] = {")
    for step in steps:
        fields = DETAIL_FIELDS.get(step["phase"], {})
        vals = {DETAIL_STRUCT_FIELD[k]: step[k] for k in fields}
        detail = ", ".join("%d" % vals.get(f, 0) for f in DETAIL_ORDER)
        lines.append("  { %-19s %6lu, %-5s %2d, %s },   // %s"
                     % (PHASE_ENUM[step["phase"]] + ",",
                        step["duration_ms"],
                        ("true" if step["enabled"] else "false") + ",",
                        step.get("every_n_cycles", 0),
                        detail,
                        step["phase"]))
    lines.append("};")
    lines.append("const uint8_t SCENE_SEQUENCE_LEN = sizeof(SCENE_SEQUENCE) / sizeof(SCENE_SEQUENCE[0]);")
    lines.append("")
    lines.append("// Scene intro length within the TIME step (ms).")
    lines.append("const unsigned long SCENE_INTRO_MS = %lu;" % intro_ms)
    lines.append("// Default scene flavor; a SCENE_STYLE define in secrets.h still wins.")
    lines.append("#ifndef CONFIG_SCENE_STYLE_DEFAULT")
    lines.append("#define CONFIG_SCENE_STYLE_DEFAULT %d" % style)
    lines.append("#endif")
    lines.append("")
    lines.append("// hour (0-23) -> scene index: %s." % " ".join(
        "%d=%s" % (i, s) for i, s in enumerate(SCENES)))
    lines.append("const int8_t SCENE_HOUR_MAP[24] = {")
    for start in range(0, 24, 12):
        lines.append("  " + ", ".join(str(hour_map[h]) for h in range(start, start + 12)) + ",")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def generate(json_path, out_path):
    cfg = load_config(json_path)
    steps = validate_sequence(cfg)
    intro_ms, style = validate_scene_globals(cfg)
    hour_map = build_hour_map(cfg)
    content = render_header(steps, intro_ms, style, hour_map)

    if os.path.exists(out_path):
        with open(out_path, "r", encoding="utf-8") as fh:
            if fh.read() == content:
                return False  # unchanged: no rebuild churn
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(content)
    return True


# ===== Boot logo (boot_logo.mpj -> include/boot_logo.h) =====
# Drop an LED Matrix Editor export named boot_logo.mpj into the repo root and
# the firmware plays it as a boot splash: it plays once at its own frame
# durations, holds a minimum total (~1.75 s) so a single-frame logo is
# actually visible, then loops until the WiFi gets the IP. No file -> no
# logo: the build is identical to before.
BOOT_LOGO_NAME = "boot_logo.mpj"
BOOT_LOGO_MAX_FRAMES = 96
BOOT_LOGO_MIN_DURATION_MS = 30
BOOT_LOGO_MAX_DURATION_MS = 3000
BOOT_LOGO_HOLD_MS = 1750   # firmware: the logo holds at least this long


def boot_logo_in():
    return os.path.join(project_root(), BOOT_LOGO_NAME)


def boot_logo_out():
    return os.path.join(project_root(), "include", "boot_logo.h")


def validate_boot_logo(data):
    """Validate a .mpj frame list and pack it to 3 uint32 words per frame."""
    if not isinstance(data, list):
        fail("%s must be a JSON array of frames" % BOOT_LOGO_NAME)
    if not data:
        fail("%s must hold at least one frame" % BOOT_LOGO_NAME)
    if len(data) > BOOT_LOGO_MAX_FRAMES:
        fail("%s: %d frames, max %d" % (BOOT_LOGO_NAME, len(data),
                                        BOOT_LOGO_MAX_FRAMES))
    frames = []
    for i, f in enumerate(data):
        where = "%s[%d]" % (BOOT_LOGO_NAME, i)
        if not isinstance(f, dict):
            fail("%s must be an object with 'matrix' and 'duration'" % where)
        matrix = f.get("matrix")
        if not isinstance(matrix, list) or len(matrix) != 8:
            fail("%s.matrix must be 8 rows" % where)
        for r, row in enumerate(matrix):
            if not isinstance(row, list) or len(row) != 12:
                fail("%s.matrix row %d must be 12 columns" % (where, r))
            for c, v in enumerate(row):
                if v not in (0, 1):
                    fail("%s.matrix[%d][%d] must be 0 or 1" % (where, r, c))
        duration = f.get("duration")
        if not isinstance(duration, int) or not (
                BOOT_LOGO_MIN_DURATION_MS <= duration <= BOOT_LOGO_MAX_DURATION_MS):
            fail("%s.duration must be %d..%d ms" % (where,
                                                    BOOT_LOGO_MIN_DURATION_MS,
                                                    BOOT_LOGO_MAX_DURATION_MS))
        # 8x12 -> 3 words, row-major MSB-first (same packing as the studio art)
        words = [0, 0, 0]
        for r in range(8):
            for c in range(12):
                if matrix[r][c]:
                    bit = r * 12 + c
                    words[bit // 32] |= 1 << (31 - (bit % 32))
        frames.append((words, duration))
    return frames


def render_boot_logo(frames):
    lines = []
    lines.append("// GENERATED FILE — do not edit.")
    lines.append("// Generated from %s in the repo root (LED Matrix Editor"
                 % BOOT_LOGO_NAME)
    lines.append("// export). Delete boot_logo.mpj to build without a boot logo.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append("#define CONFIG_BOOT_LOGO_ENABLED 1")
    lines.append("#define CONFIG_BOOT_LOGO_HOLD_MS %du" % BOOT_LOGO_HOLD_MS)
    lines.append("#define CONFIG_BOOT_LOGO_FRAME_COUNT %du" % len(frames))
    lines.append("static const uint32_t CONFIG_BOOT_LOGO_FRAMES[%du][3] = {"
                 % len(frames))
    for words, _ in frames:
        lines.append("  { 0x%08xu, 0x%08xu, 0x%08xu }," % tuple(words))
    lines.append("};")
    lines.append("static const unsigned int CONFIG_BOOT_LOGO_FRAME_MS[%du] = {"
                 % len(frames))
    for _, d in frames:
        lines.append("  %du," % d)
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def render_boot_logo_disabled():
    lines = []
    lines.append("// GENERATED FILE — do not edit.")
    lines.append("// No boot_logo.mpj in the repo root: the firmware builds")
    lines.append("// without a boot logo. Drop a LED Matrix Editor export named")
    lines.append("// boot_logo.mpj into the repo root and rebuild to add one.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#define CONFIG_BOOT_LOGO_ENABLED 0")
    lines.append("")
    return "\n".join(lines)


def generate_boot_logo():
    path = boot_logo_in()
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8-sig") as fh:
                data = json.load(fh)
        except ValueError as exc:
            fail("%s is not valid JSON: %s" % (BOOT_LOGO_NAME, exc))
        frames = validate_boot_logo(data)
        content = render_boot_logo(frames)
    else:
        content = render_boot_logo_disabled()
    out = boot_logo_out()
    if os.path.exists(out):
        with open(out, "r", encoding="utf-8") as fh:
            if fh.read() == content:
                return False   # unchanged: no rebuild churn
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(content)
    return True


def run(config_path, out_path, verbose):
    try:
        changed = generate(config_path, out_path)
        logo_changed = generate_boot_logo()
    except ConfigError as exc:
        # Readable error for both direct runs and pre-build failures.
        print("scene_config error: %s" % exc, file=sys.stderr)
        sys.exit(1)
    if verbose:
        print(("wrote %s" % out_path) if changed else ("%s up to date" % out_path))
        print(("wrote %s" % boot_logo_out()) if logo_changed
              else ("%s up to date" % boot_logo_out()))


if globals().get("__name__", "") == "__main__":
    # Standalone: allow overriding paths; argparse is unsafe in the SCons
    # pre-script context (sys.argv belongs to the build tool there).
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", default=default_json(), help="config JSON (default: scene_config.json)")
    parser.add_argument("--out", default=default_out(), help="output header (default: include/scene_config.h)")
    _args = parser.parse_args()
    run(_args.json, _args.out, verbose=True)
else:
    # Exec'd by PlatformIO as a pre-build script: generate with defaults.
    # globals().get (not a bare __name__ reference) keeps this safe even if
    # the exec namespace omits __name__ entirely.
    run(default_json(), default_out(), verbose=False)
