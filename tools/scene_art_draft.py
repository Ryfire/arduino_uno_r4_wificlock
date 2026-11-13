#!/usr/bin/env python3
"""Simple time-of-day art set — single source of truth.

Six purpose-drawn stills plus six short keyframe animations for the clock's
scene intro, designed from the icon motifs in animation_drafts/ (dash-ray
sun, sun-on-horizon, crescent + stars). Fill style carries the time of day:

    filled silhouette  = evening/night  (sunset evening sun, night evening moon)
    open ring          = daytime        (sunrise, morning); day = bold disc

Canvas: 12 cols x 8 rows, '#' = lit pixel, '.' = off. Row 7 is the shared
ground line (every frame), matching the classic scenes' horizon. Every
animation totals exactly 2500 ms = the scene intro window.

Shared glyphs (drawn once, reused by several scenes):
    ring  4x4  ".##."/"#..#"/"#..#"/".##."   -> sunrise/morning open sun
    disc  4x4  ".##."/"####"/"####"/".##."   -> sunset/day filled sun

Consumed by:
    tools/preview_scene_art.py    -> scene_preview.html (design review)
    tools/generate_simple_scenes.py -> include/simple_scenes.h (firmware)

Do not hand-edit generated outputs; edit this file, then re-run both.

Quick look in the terminal:
    python tools/scene_art_draft.py
"""
import sys

W, H = 12, 8
GROUND = "#" * W

# Bucket order must match SceneKind in src/main.cpp:
# night, sunrise, morning, day, sunset, dusk
SCENES = [
    # ---------------------------------------------------------------- night
    {
        "key": "night",
        "label": "Night",
        "hours": "21:00-04:59",
        "concept": "evening moon — filled crescent left, four twinkling stars right",
        "loop": True,
        "still": [
            ".......#....",
            "..#.......#.",
            ".##.........",
            ".##.....#...",
            ".###........",
            "..###....#..",
            "............",
            GROUND,
        ],
        # The crescent holds; the stars twinkle in opposite pairs, then all
        # hold lit (frame 3 = the still).
        "frames": [
            ([
                ".......#....",
                "..#.........",
                ".##.........",
                ".##.....#...",
                ".###........",
                "..###.......",
                "............",
                GROUND,
            ], 800),
            ([
                "............",
                "..#.......#.",
                ".##.........",
                ".##.........",
                ".###........",
                "..###....#..",
                "............",
                GROUND,
            ], 800),
            ([
                ".......#....",
                "..#.......#.",
                ".##.........",
                ".##.....#...",
                ".###........",
                "..###....#..",
                "............",
                GROUND,
            ], 900),
        ],
    },
    # -------------------------------------------------------------- sunrise
    {
        "key": "sunrise",
        "label": "Sunrise",
        "hours": "05:00-06:59",
        "concept": "open ring sun climbs from below the ground and clears the horizon",
        "loop": False,
        "still": [
            "............",
            "...##.......",
            "...##.......",
            "..#..#......",
            "..#..#......",
            "...##.......",
            "............",
            GROUND,
        ],
        # Two stars wink out while the ring rises: peek -> sides on the
        # horizon -> full ring at the ground line -> cleared, first ray on.
        "frames": [
            ([
                "............",
                "........#...",
                "............",
                "............",
                ".#..........",
                "...##.......",
                "..#..#......",
                GROUND,
            ], 700),
            ([
                "............",
                "........#...",
                "............",
                "............",
                "...##.......",
                "..#..#......",
                "..#..#......",
                GROUND,
            ], 700),
            ([
                "............",
                "............",
                "............",
                "...##.......",
                "..#..#......",
                "..#..#......",
                "...##.......",
                GROUND,
            ], 500),
            ([
                "............",
                "...##.......",
                "...##.......",
                "..#..#......",
                "..#..#......",
                "...##.......",
                "............",
                GROUND,
            ], 600),
        ],
    },
    # -------------------------------------------------------------- morning
    {
        "key": "morning",
        "label": "Morning",
        "hours": "07:00-10:59",
        "concept": "open ring sun with pulsing dash rays, small cloud parked low-right",
        "loop": True,
        "still": [
            "...##.......",
            "...##.......",
            "#.#..#.#....",
            "#.#..#.#....",
            "...##.......",
            "...##...###.",
            ".......####.",
            GROUND,
        ],
        # Ray sets trade places: vertical dashes -> horizontal dashes -> all.
        "frames": [
            ([
                "...##.......",
                "...##.......",
                "..#..#......",
                "..#..#......",
                "...##.......",
                "...##...###.",
                ".......####.",
                GROUND,
            ], 800),
            ([
                "............",
                "...##.......",
                "#.#..#.#....",
                "#.#..#.#....",
                "...##.......",
                "........###.",
                ".......####.",
                GROUND,
            ], 800),
            ([
                "...##.......",
                "...##.......",
                "#.#..#.#....",
                "#.#..#.#....",
                "...##.......",
                "...##...###.",
                ".......####.",
                GROUND,
            ], 900),
        ],
    },
    # ------------------------------------------------------------------ day
    {
        "key": "day",
        "label": "Day",
        "hours": "11:00-16:59",
        "concept": "bold filled sun — diagonal and cardinal dash rays trade places",
        "loop": True,
        "still": [
            ".....##.....",
            "..#......#..",
            "....####....",
            ".#.######.#.",
            ".#.######.#.",
            "....####....",
            "..#..##..#..",
            GROUND,
        ],
        "frames": [
            ([
                "............",
                "..#......#..",
                "....####....",
                "...######...",
                "...######...",
                "....####....",
                "..#......#..",
                GROUND,
            ], 700),
            ([
                ".....##.....",
                "............",
                "....####....",
                ".#.######.#.",
                ".#.######.#.",
                "....####....",
                ".....##.....",
                GROUND,
            ], 700),
            ([
                ".....##.....",
                "..#......#..",
                "....####....",
                ".#.######.#.",
                ".#.######.#.",
                "....####....",
                "..#..##..#..",
                GROUND,
            ], 1100),
        ],
    },
    # --------------------------------------------------------------- sunset
    {
        "key": "sunset",
        "label": "Sunset",
        "hours": "17:00-18:59",
        "concept": "evening sun — filled disc sinks into the ground, clouds drift left",
        "loop": False,
        "still": [
            "............",
            "####..##....",
            ".......####.",
            "............",
            ".....##.....",
            "..#.####.#..",
            ".....##.....",
            GROUND,
        ],
        # Disc sinks one row-set per step (top 3 -> 4 -> 6), dash rays ride
        # along (diagonals + flanks, per animation_drafts/04.png) and go out
        # once the disc is under; the still holds the half-sunk moment.
        "frames": [
            ([
                "............",
                ".####..#....",
                "...#....####",
                ".....##.....",
                "..#.####.#..",
                "....####....",
                ".....##.....",
                GROUND,
            ], 800),
            ([
                "............",
                "####..##....",
                ".......####.",
                "............",
                ".....##.....",
                "..#.####.#..",
                ".....##.....",
                GROUND,
            ], 800),
            ([
                "............",
                "###.........",
                "......####..",
                "............",
                "............",
                "............",
                "....####....",
                GROUND,
            ], 900),
        ],
    },
    # ----------------------------------------------------------------- dusk
    {
        "key": "dusk",
        "label": "Dusk",
        "hours": "19:00-20:59",
        "concept": "afterglow dome recedes while four stars pop in one by one",
        "loop": False,
        "still": [
            ".......#....",
            "..#.........",
            "............",
            "....#.......",
            ".........#..",
            "....####....",
            "...######...",
            GROUND,
        ],
        # Dome full (3 rows) -> wide (2) -> small (2) with stars A,B,C,D
        # arriving one per frame; the still = the final state.
        "frames": [
            ([
                "............",
                "............",
                "............",
                "............",
                "....####....",
                "...######...",
                "..########..",
                GROUND,
            ], 500),
            ([
                "............",
                "..#.........",
                "............",
                "............",
                "............",
                "...######...",
                "..########..",
                GROUND,
            ], 600),
            ([
                "............",
                "..#.........",
                "............",
                "....#.......",
                "............",
                "....####....",
                "...######...",
                GROUND,
            ], 700),
            ([
                ".......#....",
                "..#.........",
                "............",
                "....#.......",
                ".........#..",
                "....####....",
                "...######...",
                GROUND,
            ], 700),
        ],
    },
]


def validate():
    """Every frame is H rows x W chars of '#'/''.' — catch typos at import."""
    for sc in SCENES:
        frames = [("still", sc["still"])] + [
            ("frame %d" % i, f) for i, (f, _d) in enumerate(sc["frames"])
        ]
        for where, rows in frames:
            loc = "%s/%s" % (sc["key"], where)
            if len(rows) != H:
                raise ValueError("%s: %d rows, want %d" % (loc, len(rows), H))
            for r, row in enumerate(rows):
                if len(row) != W:
                    raise ValueError("%s row %d: %d cols, want %d (%r)"
                                     % (loc, r, len(row), W, row))
                bad = set(row) - {"#", "."}
                if bad:
                    raise ValueError("%s row %d: bad chars %s" % (loc, r, bad))
        if not sc["frames"]:
            raise ValueError("%s: no animation frames" % sc["key"])


validate()


def total_ms(sc):
    return sum(d for _f, d in sc["frames"])


def _ascii(rows):
    return "\n".join(rows)


if __name__ == "__main__":
    for sc in SCENES:
        print("%s (%s) — %s" % (sc["label"], sc["hours"], sc["concept"]))
        print("  still%s" % (" [loop]" if sc["loop"] else ""))
        for line in _ascii(sc["still"]).splitlines():
            print("    " + line)
        for i, (f, d) in enumerate(sc["frames"]):
            print("  frame %d (%d ms)%s" % (i, d, " <- last" if i == len(sc["frames"]) - 1 else ""))
            for line in _ascii(f).splitlines():
                print("    " + line)
        print("  total %d ms, loop=%s" % (total_ms(sc), sc["loop"]))
        print()
    sys.exit(0)
