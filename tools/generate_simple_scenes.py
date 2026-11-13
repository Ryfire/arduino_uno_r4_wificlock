#!/usr/bin/env python3
"""Generate include/simple_scenes.h from tools/scene_art_draft.py.

The draft file is the single source of truth for the simple time-of-day art
set (SCENE_STYLE 4 = keyframe animations, 5 = stills). This script re-uses
its SCENES data (already shape-validated at import) and emits the C++ tables
the firmware walks:

    SIMPLE_ART[6][8]      -- stills, one row of 12 bits per matrix row (MSB = col 0)
    SIMPLE_SCENES[6]      -- keyframe animations: rowBits[8] + duration per frame,
                             frameCount, loop flag

Runs automatically before every PlatformIO build (see platformio.ini
extra_scripts) and can also be run by hand:

    python tools/generate_simple_scenes.py

The output is written only when its content changes, so untouched art does
not trigger spurious rebuilds.
"""

import os

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


def default_out():
    return os.path.join(project_root(), "include", "simple_scenes.h")


# Keep the firmware struct small: animations are 3-4 frames today; raise only
# with a reason (each extra frame costs 20 bytes of flash per scene).
MAX_FRAMES = 4


def row_bits(rows):
    """One ASCII matrix row -> 12-bit int, MSB = column 0."""
    out = []
    for row in rows:
        v = 0
        for c, ch in enumerate(row):
            if ch == "#":
                v |= 1 << (11 - c)
        out.append(v)
    return out


def render_header(scenes, mix):
    lines = []
    lines.append("// GENERATED FILE — do not edit.")
    lines.append("// Source of truth: tools/scene_art_draft.py (art) + tools/scene_mix.py")
    lines.append("// (per-bucket flavors) — edit there, then re-run")
    lines.append("// tools/generate_simple_scenes.py; pio run does it automatically).")
    lines.append("// Consumed by src/main.cpp for SCENE_STYLE 4 (animations), 5 (stills)")
    lines.append("// and 6 (mixed preset). Bucket order matches SceneKind:")
    lines.append("// night, sunrise, morning, day, sunset, dusk.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append("// One purpose-drawn still per scene, 12 bits per row, MSB = column 0.")
    lines.append("const uint16_t SIMPLE_ART[6][8] = {")
    for sc in scenes:
        lines.append("  { // %s" % sc["key"])
        for v in row_bits(sc["still"]):
            lines.append("    0b" + format(v, "012b") + ",")
        lines.append("  },")
    lines.append("};")
    lines.append("")
    lines.append("// Keyframe animations: each frame is 8 row-words plus a duration in ms.")
    lines.append("// loop=true scenes wrap (twinkle-style); loop=false scenes play once and")
    lines.append("// hold the last frame until the intro window ends (same as custom art).")
    lines.append("struct SimpleFrame {")
    lines.append("  uint16_t rowBits[8];")
    lines.append("  unsigned long durationMs;")
    lines.append("};")
    lines.append("struct SimpleScene {")
    lines.append("  uint8_t frameCount;")
    lines.append("  bool loop;")
    lines.append("  SimpleFrame frames[%d];" % MAX_FRAMES)
    lines.append("};")
    lines.append("const SimpleScene SIMPLE_SCENES[6] = {")
    for sc in scenes:
        frames = sc["frames"]
        if len(frames) > MAX_FRAMES:
            raise ValueError("%s: %d frames, max %d" % (sc["key"], len(frames), MAX_FRAMES))
        lines.append("  { // %s (%d frames, %d ms total, loop=%s)"
                     % (sc["key"], len(frames),
                        sum(d for _f, d in frames), sc["loop"]))
        lines.append("    %d, %s," % (len(frames), "true" if sc["loop"] else "false"))
        lines.append("    {")
        for f, dur in frames:
            lines.append("      { // %d ms" % dur)
            lines.append("        {")
            for v in row_bits(f):
                lines.append("          0b" + format(v, "012b") + ",")
            lines.append("        },")
            lines.append("        %d," % dur)
            lines.append("      },")
        # zero-fill unused frame slots so the struct initializer is complete
        for _ in range(MAX_FRAMES - len(frames)):
            lines.append("      { { 0, 0, 0, 0, 0, 0, 0, 0 }, 0 },")
        lines.append("    },")
        lines.append("  },")
    lines.append("};")
    lines.append("")
    lines.append("// SCENE_STYLE=6 (mixed preset): each bucket plays its own flavor.")
    lines.append("// 1 = classic still, 2 = classic animated, 4 = simple animated,")
    lines.append("// 5 = simple still. Source of truth: tools/scene_mix.py.")
    lines.append("const uint8_t SCENE_MIX[6] = {")
    order = ["night", "sunrise", "morning", "day", "sunset", "dusk"]
    row = []
    for key in order:
        if key not in mix or mix[key] not in (1, 2, 4, 5):
            raise ValueError("scene_mix.py: %s must be one of 1, 2, 4, 5 (got %r)"
                             % (key, mix.get(key)))
        row.append("%d,  // %s" % (mix[key], key))
    for entry in row:
        lines.append("  " + entry)
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def generate(out_path):
    # import the draft + mix from the tools dir regardless of cwd
    sys_path = os.path.dirname(os.path.abspath(__file__)) if globals().get("__file__") \
        else os.path.join(project_root(), "tools")
    import sys
    if sys_path not in sys.path:
        sys.path.insert(0, sys_path)
    import scene_art_draft
    import scene_mix
    content = render_header(scene_art_draft.SCENES, scene_mix.MIX)

    if os.path.exists(out_path):
        with open(out_path, "r", encoding="utf-8") as fh:
            if fh.read() == content:
                return False  # unchanged: no rebuild churn
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(content)
    return True


def run(verbose=True):
    try:
        changed = generate(default_out())
    except Exception as exc:  # readable failure for the pre-build context
        print("simple_scenes error: %s" % exc)
        raise
    if verbose:
        print(("wrote %s" % default_out()) if changed
              else ("%s up to date" % default_out()))


if __name__ == "__main__":
    run()
else:
    # Exec'd by PlatformIO as a pre-build script: generate with defaults.
    run(verbose=False)
