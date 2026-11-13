"""Render exact frames for the README mockups from the real font data."""

COMPACT = {
    '0': ["111", "101", "101", "101", "111"],
    '1': ["010", "110", "010", "010", "111"],
    '2': ["111", "001", "111", "100", "111"],
    '3': ["111", "001", "111", "001", "111"],
    '4': ["101", "101", "111", "001", "001"],
    '5': ["111", "100", "111", "001", "111"],
    '6': ["111", "100", "111", "101", "111"],
    '7': ["111", "001", "010", "010", "010"],
    '8': ["111", "101", "111", "101", "111"],
    '9': ["111", "101", "111", "001", "111"],
    'A': ["010", "101", "111", "101", "101"],
    'M': ["101", "111", "101", "101", "101"],
    'P': ["110", "101", "110", "100", "100"],
    ' ': ["000", "000", "000", "000", "000"],
}

LARGE = {   # 7 rows x 5 cols (subset needed for examples)
    '0': ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    '5': ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    '4': ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
}


def blank():
    return [[0] * 12 for _ in range(8)]


def put_compact(frame, ch, col):
    for r in range(5):
        for c in range(3):
            frame[2 + r][col + c] = int(COMPACT[ch][r][c])


def to_str(frame):
    return ["".join("#" if p else "." for p in row) for row in frame]


def show(title, frame):
    print(f"--- {title} ---")
    for row in to_str(frame):
        print(row)
    print()


# Time page "11 P": chars at cols 0,3,6,9; colon dots at col 7 (rows 3,5)
f = blank()
for ch, col in zip("11 P", (0, 3, 6, 9)):
    put_compact(f, ch, col)
f[3][7] = 1
f[5][7] = 1
show('TIME "11 P" (colon blinking on)', f)

# Time page "9 AM": colon dots at col 4
f = blank()
for ch, col in zip("9 AM", (0, 3, 6, 9)):
    put_compact(f, ch, col)
f[3][4] = 1
f[5][4] = 1
show('TIME "9 AM"', f)

# Time page 24h "2352": tick pixel [1][5] shown lit
f = blank()
for ch, col in zip("2352", (0, 3, 6, 9)):
    put_compact(f, ch, col)
f[1][5] = 1
show('TIME 24h "2352" (tick pixel lit)', f)

# Minutes "05": large tens at cols 0-4, ones at cols 7-11, rows 0-6
f = blank()
for r in range(7):
    for c in range(5):
        f[r][c] = int(LARGE['0'][r][c])
        f[r][7 + c] = int(LARGE['5'][r][c])
show('MINUTES "05"', f)

# Date "11" and "08": chars at cols 2,7, rows 2-6
f = blank()
put_compact(f, '1', 2)
put_compact(f, '1', 7)
show('DATE month half "11"', f)

f = blank()
put_compact(f, '0', 2)
put_compact(f, '8', 7)
show('DATE day half "08"', f)

# Year "26": chars at cols 2,7, inside a 1-px border on the top row and
# both side columns (displayYear's drawBorder); the bottom row stays open
f = blank()
put_compact(f, '2', 2)
put_compact(f, '6', 7)
for c in range(12):
    f[0][c] = 1
for r in range(8):
    f[r][0] = 1
    f[r][11] = 1
show('YEAR "2 6" (bordered)', f)

# WiFi page "-57": 2-px minus bar at row 4 cols 0-1, 2-col gap, digits at
# cols 4 and 8 with a 1-col space between them (displayWifiRssi layout)
f = blank()
f[4][0] = 1
f[4][1] = 1
put_compact(f, '5', 4)
put_compact(f, '7', 8)
show('WIFI "-57"', f)


# ===== Day-name scroll (large 7x5 letters, marquee) =====
# Parses largeChars directly from glyphFonts.h so mockups can never drift
# from the firmware, and replicates buildDayScroll()/displayDayScroll()
# exactly: 12 lead-in columns, 5-col glyphs packed at rows 0-6 with a
# 2-col gap between letters, 12 trail-out columns, window = buffer[pos..pos+12).

import re
import os


def parse_large_chars():
    hdr = os.path.join(os.path.dirname(__file__), "..", "include", "glyphFonts.h")
    src = open(os.path.abspath(hdr)).read()
    m = re.search(r"largeChars\[36\]\[7\]\[5\]\s*(?:PROGMEM\s*)?=\s*\{", src)
    if not m:
        raise SystemExit("largeChars not found in glyphFonts.h")
    # strip line comments, then read the block
    block = src[m.end():]
    end = block.find("};")
    block = re.sub(r"//[^\n]*", "", block[:end])
    nums = [int(n) for n in re.findall(r"\d+", block)]
    if len(nums) != 36 * 7 * 5:
        raise SystemExit(f"expected 1260 values, got {len(nums)}")
    glyphs = {}
    names = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    for i in range(36):
        g = []
        for r in range(7):
            g.append("".join(str(nums[i * 35 + r * 5 + c]) for c in range(5)))
        glyphs[names[i]] = g
    return glyphs


LARGE = parse_large_chars()


def build_scroll_buffer(name):
    """Replicates main.cpp's buildDayScroll(): 12 lead-in columns, 2-col gap
    between letters, glyphs packed rows 0-6, 12 trail-out columns."""
    buf = []
    for _ in range(12):
        buf.append(0)
    for i, ch in enumerate(name):
        if i > 0:
            buf.append(0)          # 2-col gap (minutes-page convention)
            buf.append(0)
        for col in range(5):
            b = 0
            for row in range(7):
                if LARGE[ch][row][col] == "1":
                    b |= (1 << row)
            buf.append(b)
    for _ in range(12):
        buf.append(0)
    return buf


def scroll_window(buf, pos):
    """Screen col c shows buffer[pos + c]; the word slides leftward as pos grows."""
    f = [[0] * 12 for _ in range(8)]
    for c in range(12):
        idx = pos + c
        if idx >= len(buf):
            break
        b = buf[idx]
        for r in range(8):
            if b & (1 << r):
                f[r][c] = 1
    return f


for name in ("MONDAY", "WEDNESDAY"):
    buf = build_scroll_buffer(name)
    total = len(buf)
    last = total - 12                     # scrollPos stops here
    word_cols = 5 * len(name) + 2 * (len(name) - 1)
    print(f"{name}: {len(name)} letters, {word_cols} word cols, "
          f"{total} buffer cols, {last} ticks at 90 ms = {last*90/1000:.1f}s")
    # word col k sits at buffer[12 + k]; a letter pair (a,b) is fully visible
    # when the window starts at word col 0 of letter a (cols 0-4 + gap 5-6 + b)
    # minus 2 lead-in so the pair is centered on screen.
    starts = []
    if name == "MONDAY":
        starts = [("MO", 0), ("ND", 14), ("AY", 28)]
    else:
        # WEDNESDAY: W=0 O=7 E=14 D=21 N=28 E=35 S=42 D=49 A=56 Y=63
        starts = [("WE", 0), ("ED", 14), ("NE", 28), ("SD", 42), ("AY", 56)]
    for pair, wc in starts:
        show(f'DAY SCROLL "{name}" (screen shows "{pair}")', scroll_window(buf, 12 + wc))


# ===== Two-eyed face (intermittent) =====

def face_frame(pupil=None, lowered=False):
    """Replicates drawEyes/drawEyePupils/drawEyeLidsLowered/drawEyeLids
    exactly: two largeDigits[0] "0" glyphs at cols 0-4 / 7-11, rows 0-6.
    pupil: col offset within each eye's 3-wide interior
           (1=LEFT, 2=CENTER, 3=RIGHT); a single px at row 4 in both
           states, so it never jumps when the lids droop.
    lowered=True fills the interior's top two rows (rows 1-2) with the
    drooped lid, leaving rows 3-5 open.
    pupil=None means blink: a single seam across each eye's interior
    (row 3)."""
    f = [[0] * 12 for _ in range(8)]
    glyph = LARGE['0']
    for r in range(7):
        for c in range(5):
            f[r][c] = int(glyph[r][c])         # left eye
            f[r][c + 7] = int(glyph[r][c])     # right eye
    if pupil is None:
        # drawEyeLids: one row across each eye's interior
        for c in range(1, 4):
            f[3][c] = 1
            f[3][c + 7] = 1
    else:
        if lowered:
            # drawEyeLidsLowered: interior rows 1-2
            for r in (1, 2):
                for c in range(1, 4):
                    f[r][c] = 1
                    f[r][c + 7] = 1
        # drawEyePupils: one px at row 4
        f[4][pupil] = 1
        f[4][pupil + 7] = 1
    return f


show('FACE center (pupil mid)', face_frame(2))
show('FACE lids lowered (pupil mid)', face_frame(2, lowered=True))
show('FACE left (pupil left, lids lowered)', face_frame(1, lowered=True))
show('FACE right (pupil right, lids lowered)', face_frame(3, lowered=True))
show('FACE blink (one-row seam)', face_frame(None))


# ===== Static image art (SCENE_STYLE=1 -> SCENE_ART in main.cpp) =====
# Purpose-drawn stills indexed by SceneKind (night, sunrise, morning, day,
# sunset, dusk): the most expressive pose of each bucket with every star lit
# and every ray on. Shared ground line on row 7, same family as the animated
# scenes. Values are ported verbatim into the SCENE_ART array.

STATIC_ART = [
    ('STATIC night', [
        "....#....#..",
        "..#.....##..",
        ".......##...",
        "....#..##...",
        ".......##...",
        ".#......##..",
        "............",
        "############",
    ]),
    ('STATIC sunrise', [
        "............",
        "..#.........",
        "............",
        "....#.......",
        "............",
        ".....##.....",
        "....####....",
        "############",
    ]),
    ('STATIC morning', [
        "..#.........",
        ".###........",
        "#####.......",
        ".###........",
        "..#....##...",
        "......####..",
        "............",
        "############",
    ]),
    ('STATIC day', [
        "....####....",
        "....####....",
        "...######...",
        "...######...",
        "....####....",
        "....####....",
        "............",
        "############",
    ]),
    ('STATIC sunset', [
        "............",
        "####....####",
        "........####",
        "............",
        "............",
        ".....##.....",
        "....####....",
        "############",
    ]),
    ('STATIC dusk', [
        "....#....#..",
        "..#.........",
        "............",
        ".#.......#..",
        "....####....",
        "...######...",
        "..########..",
        "############",
    ]),
]

for title, rows in STATIC_ART:
    assert all(len(r) == 12 for r in rows) and len(rows) == 8, title
    show(title, [[1 if ch == '#' else 0 for ch in r] for r in rows])


# ===== Time-of-day scene intro (displayScene) =====
# Mirrors main.cpp's sceneNight/sceneSunrise/sceneDusk at fixed p values.
# Stars are drawn lit (they twinkle at runtime); ground = row 7.

SUN_DISC = [
    "0110",
    "1111",
    "1111",
    "0110",
]

MOON = [
    "01100",
    "11000",
    "11000",
    "11000",
    "01100",
]


def scene_ground(f):
    for c in range(12):
        f[7][c] = 1


def put_sun(f, top):
    for r in range(4):
        fr = top + r
        if fr < 0 or fr > 6:
            continue
        for c in range(4):
            if SUN_DISC[r][c] == "1":
                f[fr][4 + c] = 1


# Scene "night": ground + crescent moon (cols 7-11, rows 1-5) + 4 stars
f = blank()
scene_ground(f)
for r in range(5):
    for c in range(5):
        if MOON[r][c] == "1":
            f[1 + r][7 + c] = 1
for r, c in ((1, 2), (3, 4), (5, 1), (0, 4)):
    f[r][c] = 1
show('SCENE night', f)

# Scene "sunrise" at p=0.5: stars with p<0.60/0.75/0.90 lit ((5,1) is
# gone by p=0.45), sun top = 7 - int(0.5 * 5.5) = 5 (half risen)
f = blank()
scene_ground(f)
for r, c in ((1, 2), (3, 4), (0, 4)):
    f[r][c] = 1
put_sun(f, 7 - int(0.5 * 5.5))
show('SCENE sunrise (mid-rise)', f)

# Scene "dusk" at p=0.6: mid dome (p in [0.35,0.65): row5 cols3-8 + row6
# cols2-9), stars with p>0.15/0.35/0.55 lit ((0,4) needs p>0.75)
f = blank()
scene_ground(f)
for c in range(3, 9):
    f[5][c] = 1
for c in range(2, 10):
    f[6][c] = 1
for r, c in ((1, 2), (3, 4), (5, 1)):
    f[r][c] = 1
show('SCENE dusk (stars popping in)', f)
