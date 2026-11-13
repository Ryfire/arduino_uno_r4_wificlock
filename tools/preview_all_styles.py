#!/usr/bin/env python3
"""Render an all-styles scene picker into a self-contained HTML page.

For each of the six scene buckets (night, sunrise, morning, day, sunset,
dusk) the page shows every intro flavor side by side:

    1  classic still        (SCENE_ART in src/main.cpp)
    2  classic animated     (sceneNight..sceneDusk, sampled off main.cpp)
    4  simple animated      (tools/scene_art_draft.py keyframes)
    5  simple still         (tools/scene_art_draft.py stills)

Click one card per bucket — the picks persist (localStorage) and the page
builds the matching tools/scene_mix.py config for the mixed preset
(SCENE_STYLE=6). Style 3 (custom .mpj art) is studio-branch only and is not
previewed here.

    python tools/preview_all_styles.py
    start preview_all.html          (repo root, gitignored)

The classic mirror below is ported from src/main.cpp — update it if the
firmware scenes change.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_art_draft as simple  # noqa: E402

W, H = 12, 8
GROUND = "#" * W
STEP_MS = 417          # classic animations sampled at 6 points of the 2500 ms intro


def blank():
    return [[0] * W for _ in range(H)]


def to_rows(f):
    return ["".join("#" if p else "." for p in row) for row in f]


def ground(f):
    for c in range(W):
        f[7][c] = 1


def star(f, row, col, phase, t):
    if ((t // 350) + phase) % 3 != 0:
        f[row][col] = 1


SUN_DISC = [(0, 1, 1, 0), (1, 1, 1, 1), (1, 1, 1, 1), (0, 1, 1, 0)]


def sun(f, top):
    for r in range(4):
        fr = top + r
        if 0 <= fr <= 6:
            for c in range(4):
                if SUN_DISC[r][c]:
                    f[fr][4 + c] = 1


MOON = [(0, 1, 1, 0, 0), (1, 1, 0, 0, 0), (1, 1, 0, 0, 0),
        (1, 1, 0, 0, 0), (0, 1, 1, 0, 0)]


def scene_night(t):
    f = blank()
    ground(f)
    for r in range(5):
        for c in range(5):
            if MOON[r][c]:
                f[1 + r][7 + c] = 1
    star(f, 1, 2, 0, t)
    star(f, 3, 4, 1, t)
    star(f, 5, 1, 2, t)
    star(f, 0, 4, 3, t)
    return f


def scene_sunrise(p, t):
    f = blank()
    ground(f)
    if p < 0.45: star(f, 5, 1, 2, t)
    if p < 0.60: star(f, 1, 2, 0, t)
    if p < 0.75: star(f, 3, 4, 1, t)
    if p < 0.90: star(f, 0, 4, 3, t)
    sun(f, 7 - int(p * 5.5))
    return f


def scene_morning(p, t):
    f = blank()
    ground(f)
    for r in range(1, 4):
        for c in range(1, 4):
            f[r][c] = 1
    if (t // 300) % 2 == 0:
        f[0][2] = 1; f[4][2] = 1; f[2][0] = 1; f[2][4] = 1
    cloud = [(0, 1, 1, 0), (1, 1, 1, 1)]
    cx = int(p * 16.0) - 4
    for r in range(2):
        for c in range(4):
            fc = cx + c
            if cloud[r][c] and 0 <= fc < W:
                f[4 + r][fc] = 1
    return f


def scene_day(t):
    f = blank()
    ground(f)
    sun(f, 1)
    f[0][4] = 1; f[0][7] = 1; f[5][4] = 1; f[5][7] = 1
    if (t // 300) % 2 == 0:
        f[0][5] = 1; f[0][6] = 1; f[5][5] = 1; f[5][6] = 1
        f[2][3] = 1; f[3][3] = 1; f[2][8] = 1; f[3][8] = 1
    return f


def scene_sunset(p):
    f = blank()
    ground(f)
    sun(f, 2 + int(p * 5.0))
    drift = int(p * 3.0)
    for c in range(4):
        fc = c - drift
        if fc >= 0: f[1][fc] = 1
        fc = 8 + c - drift
        if fc < W: f[2][fc] = 1
    return f


def scene_dusk(p, t):
    f = blank()
    ground(f)
    if p < 0.35:
        for c in range(4, 8): f[4][c] = 1
        for c in range(3, 9): f[5][c] = 1
        for c in range(2, 10): f[6][c] = 1
    elif p < 0.65:
        for c in range(3, 9): f[5][c] = 1
        for c in range(2, 10): f[6][c] = 1
    elif p < 0.85:
        for c in range(4, 8): f[6][c] = 1
    if p > 0.15: star(f, 1, 2, 0, t)
    if p > 0.35: star(f, 3, 4, 1, t)
    if p > 0.55: star(f, 5, 1, 2, t)
    if p > 0.75: star(f, 0, 4, 3, t)
    return f


# SCENE_ART stills, ported verbatim from src/main.cpp (style 1).
CLASSIC_STILLS = {
    "night": [
        "....#....#..",
        "..#......##.",
        ".......##...",
        "....#..##...",
        ".......##...",
        ".#......##..",
        "............",
        GROUND,
    ],
    "sunrise": [
        "............",
        "..#.........",
        "............",
        "....#.......",
        "............",
        ".....##.....",
        "....####....",
        GROUND,
    ],
    "morning": [
        "..#.........",
        ".###........",
        "#####.......",
        ".###........",
        "..#.....##..",
        "......####..",
        "............",
        GROUND,
    ],
    "day": [
        "....####....",
        "....####....",
        "...########.",
        "...########.",
        "....####....",
        "....####....",
        "............",
        GROUND,
    ],
    "sunset": [
        "............",
        "####....####",
        "........####",
        "............",
        "............",
        ".....##.....",
        "....####....",
        GROUND,
    ],
    "dusk": [
        "....#....#..",
        "..#.........",
        "............",
        ".#........#.",
        "....####....",
        "...######...",
        "..########..",
        GROUND,
    ],
}

BUCKETS = [
    ("night", "Night", "21:00-04:59"),
    ("sunrise", "Sunrise", "05:00-06:59"),
    ("morning", "Morning", "07:00-10:59"),
    ("day", "Day", "11:00-16:59"),
    ("sunset", "Sunset", "17:00-18:59"),
    ("dusk", "Dusk", "19:00-20:59"),
]

CONCEPTS = {
    "night": "crescent moon + four twinkling stars",
    "sunrise": "sun climbs out of the ground, stars wink out",
    "morning": "small sun pulses rays, cloud drifts by",
    "day": "full sunburst — cardinal rays pulse",
    "sunset": "sun sinks behind the ground, clouds creep left",
    "dusk": "afterglow dome recedes, stars pop in",
}


def classic_frames(fn):
    """Sample a classic scene at 6 points of the 2500 ms intro."""
    frames = []
    for i in range(6):
        p = i / 5.0
        t = i * STEP_MS
        frames.append([to_rows(fn(p, t)), STEP_MS])
    return frames


def build_data():
    buckets = []
    for key, label, hours in BUCKETS:
        classic_anim = {
            "night": classic_frames(lambda p, t: scene_night(t)),
            "sunrise": classic_frames(scene_sunrise),
            "morning": classic_frames(scene_morning),
            "day": classic_frames(lambda p, t: scene_day(t)),
            "sunset": classic_frames(lambda p, t: scene_sunset(p)),
            "dusk": classic_frames(scene_dusk),
        }[key]
        sc = next(s for s in simple.SCENES if s["key"] == key)
        buckets.append({
            "key": key, "label": label, "hours": hours,
            "concept": CONCEPTS[key],
            "options": [
                {"style": 1, "name": "classic still",
                 "note": "current static art", "kind": "still",
                 "still": CLASSIC_STILLS[key]},
                {"style": 2, "name": "classic animated",
                 "note": "procedural, samples of the real motion", "kind": "anim",
                 "frames": classic_anim, "loop": True},
                {"style": 4, "name": "simple animated",
                 "note": sc["concept"], "kind": "anim",
                 "frames": [[f, d] for f, d in sc["frames"]], "loop": sc["loop"]},
                {"style": 5, "name": "simple still",
                 "note": "matching still of the simple set", "kind": "still",
                 "still": sc["still"]},
            ],
        })
    return buckets


HTML = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Scene styles — pick your mix</title>
<style>
  :root {
    --cell: 16px; --gap: 1px; --lit: #ffcf5c; --off: #262a30;
    --card: #1e2126; --edge: #2e333b; --text: #e8eaed; --dim: #9aa3ad;
    --pick: #7fd6a2;
  }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 24px 30px 80px; background: #14161a; color: var(--text);
         font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
  h1 { font-size: 21px; margin: 0 0 4px; }
  .note { color: var(--dim); margin: 0 0 4px; font-size: 13px; }
  h2 { font-size: 16px; margin: 30px 0 4px; }
  .hours { color: var(--dim); font-size: 12px; margin: 0 0 10px; }
  .row { display: flex; flex-wrap: wrap; gap: 14px; }
  .card { background: var(--card); border: 2px solid var(--edge); border-radius: 10px;
          padding: 12px 14px 14px; cursor: pointer; position: relative; }
  .card:hover { border-color: #4a5462; }
  .card.chosen { border-color: var(--pick); }
  .card.chosen::after { content: "\\2714 chosen"; position: absolute; top: -10px; right: 10px;
          background: var(--pick); color: #10241a; font-size: 11px; font-weight: 700;
          padding: 1px 8px; border-radius: 8px; }
  .card h3 { margin: 0; font-size: 14px; }
  .card .style { color: var(--dim); font-weight: 400; }
  .card .sub { color: var(--dim); font-size: 12px; margin: 4px 0 10px; min-height: 30px; }
  .grid { display: grid; grid-template-columns: repeat(12, var(--cell));
          grid-auto-rows: var(--cell); gap: var(--gap); background: #0e1013;
          padding: 3px; border-radius: 4px; width: max-content; }
  .cell { background: var(--off); border-radius: 2px; }
  .cell.on { background: var(--lit); box-shadow: 0 0 6px rgba(255,207,92,.45); }
  .meta { display: flex; gap: 10px; align-items: center; margin-top: 8px;
          color: var(--dim); font-size: 11px; }
  .badge { background: #2a2f37; border-radius: 4px; padding: 1px 7px; }
  .badge.loop { color: #7fd6a2; } .badge.once { color: #8fb8ff; }
  button { background: #2a2f37; color: var(--text); border: 1px solid var(--edge);
           border-radius: 5px; padding: 3px 10px; font-size: 12px; cursor: pointer; }
  button:hover { background: #343a44; }
  .film { display: flex; gap: 5px; margin-top: 8px; }
  .film .mini { display: flex; flex-direction: column; gap: 3px; align-items: center; }
  .film .grid { --cell: 4px; padding: 2px; }
  .film .cell { border-radius: 1px; } .film .cell.on { box-shadow: none; }
  #mixbar { position: fixed; left: 0; right: 0; bottom: 0; background: #10131a;
            border-top: 1px solid var(--edge); padding: 10px 30px; display: flex;
            gap: 14px; align-items: center; font-size: 13px; }
  #mixbar .code { font-family: Consolas, monospace; font-size: 12px; color: var(--pick);
                  white-space: pre; overflow-x: auto; flex: 1; }
  #mixbar .done { color: var(--dim); font-size: 11px; }
  .legend b { color: var(--lit); }
</style>
</head>
<body>
<h1>Scene styles — pick your mix</h1>
<p class="note">Every bucket, every intro flavor (styles 1/2/4/5). Click the card you
want per bucket — picks persist, and the bar at the bottom builds your
<code>scene_mix.py</code> for the mixed preset (<code>SCENE_STYLE=6</code>).</p>
<p class="note legend">Style <b>3</b> (your own imported .mpj flipbook art) lives on the
studio branch and isn't previewed here. Intro window 2500&nbsp;ms, ground line row 7.</p>
<div id="sections"></div>
<div id="mixbar">
  <span class="code" id="mixcode"></span>
  <button id="copybtn">copy</button>
  <button id="resetbtn">reset</button>
  <span class="done" id="copied"></span>
</div>
<script>
const BUCKETS = __DATA__;

function gridEl(rows) {
  const g = document.createElement("div");
  g.className = "grid";
  for (const row of rows) for (const ch of row) {
    const c = document.createElement("div");
    c.className = "cell" + (ch === "#" ? " on" : "");
    g.appendChild(c);
  }
  return g;
}
function paint(g, rows) {
  const cells = g.children; let i = 0;
  for (const row of rows) for (const ch of row)
    cells[i++].className = "cell" + (ch === "#" ? " on" : "");
}
const total = fr => fr.reduce((s, f) => s + f[1], 0);
function frameIndex(frames, t, loop) {
  const tot = total(frames);
  let x = loop ? t % tot : Math.min(t, tot - 1);
  for (let i = 0; i < frames.length; i++) {
    if (x < frames[i][1]) return i;
    x -= frames[i][1];
  }
  return frames.length - 1;
}

const picks = JSON.parse(localStorage.getItem("sceneMixPicks") || "{}");
const players = [];
const secs = document.getElementById("sections");

for (const b of BUCKETS) {
  const h2 = document.createElement("h2");
  h2.textContent = b.label + " ";
  const hrs = document.createElement("span");
  hrs.className = "hours"; hrs.textContent = b.hours + " — " + b.concept;
  secs.appendChild(h2); secs.appendChild(hrs);

  const row = document.createElement("div"); row.className = "row";
  for (const opt of b.options) {
    const card = document.createElement("div");
    card.className = "card";
    card.dataset.key = b.key; card.dataset.style = opt.style;
    const title = document.createElement("h3");
    title.innerHTML = '<span class="style">style ' + opt.style + " · </span>" + opt.name;
    card.appendChild(title);
    const sub = document.createElement("div");
    sub.className = "sub"; sub.textContent = opt.note;
    card.appendChild(sub);

    if (opt.kind === "still") {
      card.appendChild(gridEl(opt.still));
    } else {
      const g = gridEl(opt.frames[0][0]);
      card.appendChild(g);
      const meta = document.createElement("div"); meta.className = "meta";
      meta.innerHTML = '<span class="badge ' + (opt.loop ? "loop'>loops" : "once'>plays once") +
        "</span><span>" + opt.frames.length + " frames · " + total(opt.frames) + " ms</span>";
      card.appendChild(meta);
      players.push({ frames: opt.frames, loop: opt.loop, g, t0: performance.now() });
      // filmstrip
      const film = document.createElement("div"); film.className = "film";
      for (const f of opt.frames) {
        const m = document.createElement("div"); m.className = "mini";
        m.appendChild(gridEl(f[0]));
        const t = document.createElement("div"); t.className = "t";
        t.style.cssText = "font-size:9px;color:var(--dim)";
        t.textContent = f[1] + "ms";
        m.appendChild(t); film.appendChild(m);
      }
      card.appendChild(film);
    }
    card.addEventListener("click", () => {
      picks[b.key] = opt.style;
      localStorage.setItem("sceneMixPicks", JSON.stringify(picks));
      refresh();
    });
    row.appendChild(card);
  }
  secs.appendChild(row);
}

function refresh() {
  document.querySelectorAll(".card").forEach(c =>
    c.classList.toggle("chosen", +c.dataset.style === picks[c.dataset.key]));
  const done = BUCKETS.every(b => picks[b.key] !== undefined);
  const body = BUCKETS.map(b =>
    '  "' + b.key + '": ' + (picks[b.key] !== undefined ? picks[b.key] : "?") + ",").join("\\n");
  document.getElementById("mixcode").textContent =
    "# tools/scene_mix.py — your custom mix (flashed as SCENE_STYLE=6)\\nMIX = {\\n" +
    body + "\\n}" + (done ? "" : "   \\u2190 pick a card for every bucket");
}

document.getElementById("copybtn").addEventListener("click", async () => {
  await navigator.clipboard.writeText(document.getElementById("mixcode").textContent);
  document.getElementById("copied").textContent = "copied \\u2713";
  setTimeout(() => document.getElementById("copied").textContent = "", 1500);
});
document.getElementById("resetbtn").addEventListener("click", () => {
  for (const b of BUCKETS) delete picks[b.key];
  localStorage.removeItem("sceneMixPicks");
  refresh();
});

function tick(now) {
  for (const p of players)
    paint(p.g, p.frames[frameIndex(p.frames, now - p.t0, p.loop)][0]);
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
refresh();
</script>
</body>
</html>
"""


def main():
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "preview_all.html")
    html = HTML.replace("__DATA__", json.dumps(build_data()))
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(html)
    print("wrote %s" % out)


if __name__ == "__main__":
    main()
