#!/usr/bin/env python3
"""Render tools/scene_art_draft.py into a self-contained HTML preview.

Design-review tool: big pixel grids for the six stills, plus all six
animations playing at their real frame timings. No dependencies, no network
— open the file in any browser:

    python tools/preview_scene_art.py
    start scene_preview.html          (repo root, gitignored)

The HTML embeds the frame data at generation time; re-run after editing
scene_art_draft.py. Nothing here touches the firmware.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_art_draft as draft  # noqa: E402

HTML = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Simple time-of-day art — preview</title>
<style>
  :root {
    --cell: 18px;
    --gap: 1px;
    --lit: #ffcf5c;
    --off: #262a30;
    --card: #1e2126;
    --edge: #2e333b;
    --text: #e8eaed;
    --dim: #9aa3ad;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 28px 32px 60px;
    background: #14161a; color: var(--text);
    font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif;
  }
  h1 { font-size: 22px; margin: 0 0 4px; }
  h2 { font-size: 15px; margin: 34px 0 14px; color: var(--dim);
       text-transform: uppercase; letter-spacing: .08em; }
  .note { color: var(--dim); margin: 0 0 6px; }
  .legend { color: var(--dim); margin: 0; font-size: 13px; }
  .legend b { color: var(--lit); font-weight: 600; }
  .row { display: flex; flex-wrap: wrap; gap: 18px; }
  .card {
    background: var(--card); border: 1px solid var(--edge);
    border-radius: 10px; padding: 14px 16px 16px;
  }
  .card h3 { margin: 0 0 2px; font-size: 15px; }
  .card .hours { color: var(--dim); font-size: 12px; }
  .card .concept { color: var(--dim); font-size: 12.5px; margin: 6px 0 10px;
                   min-height: 32px; }
  .grid {
    display: grid;
    grid-template-columns: repeat(12, var(--cell));
    grid-auto-rows: var(--cell);
    gap: var(--gap);
    background: #0e1013; padding: 3px; border-radius: 4px;
    width: max-content;
  }
  .cell { background: var(--off); border-radius: 2px; }
  .cell.on { background: var(--lit); box-shadow: 0 0 6px rgba(255,207,92,.45); }
  .meta { display: flex; gap: 10px; align-items: center; margin-top: 10px;
          color: var(--dim); font-size: 12px; }
  .badge { background: #2a2f37; border-radius: 4px; padding: 1px 7px; }
  .badge.loop { color: #7fd6a2; }
  .badge.once { color: #8fb8ff; }
  button {
    background: #2a2f37; color: var(--text); border: 1px solid var(--edge);
    border-radius: 5px; padding: 3px 10px; font-size: 12px; cursor: pointer;
  }
  button:hover { background: #343a44; }
  .film { display: flex; gap: 6px; margin-top: 10px; }
  .film .mini { display: flex; flex-direction: column; gap: 3px; align-items: center; }
  .film .grid { --cell: 5px; padding: 2px; }
  .film .cell { border-radius: 1px; }
  .film .cell.on { box-shadow: none; }
  .film .t { font-size: 10px; color: var(--dim); }
  .now { color: var(--lit); }
</style>
</head>
<body>
<h1>Simple time-of-day art — preview</h1>
<p class="note">Source: <code>tools/scene_art_draft.py</code> · 12&times;8 matrix ·
row 7 is the ground line · intro window 2500&nbsp;ms</p>
<p class="legend">Fill style marks the time of day:
<b>filled silhouette</b> = evening/night (sunset evening sun, night evening moon) ·
open outline = daytime (sunrise, morning) · day = bold full sun.</p>

<h2>Stills &mdash; SCENE_STYLE=5</h2>
<div class="row" id="stills"></div>

<h2>Animations &mdash; SCENE_STYLE=4 (real timings)</h2>
<div class="row" id="anims"></div>

<script>
const DATA = __DATA__;

function gridEl(rows) {
  const g = document.createElement("div");
  g.className = "grid";
  for (const row of rows)
    for (const ch of row) {
      const c = document.createElement("div");
      c.className = "cell" + (ch === "#" ? " on" : "");
      g.appendChild(c);
    }
  return g;
}

function paint(g, rows) {
  const cells = g.children;
  let i = 0;
  for (const row of rows)
    for (const ch of row) cells[i++].className = "cell" + (ch === "#" ? " on" : "");
}

function card(sc) {
  const el = document.createElement("div");
  el.className = "card";
  el.innerHTML = "<h3>" + sc.label + "</h3>" +
    '<div class="hours">' + sc.hours + "</div>" +
    '<div class="concept">' + sc.concept + "</div>";
  return el;
}

// --- stills ---
const stillsBox = document.getElementById("stills");
for (const sc of DATA) {
  const el = card(sc);
  el.appendChild(gridEl(sc.still));
  stillsBox.appendChild(el);
}

// --- animations ---
const total = sc => sc.frames.reduce((s, f) => s + f[1], 0);
const animes = [];
const animsBox = document.getElementById("anims");
for (const sc of DATA) {
  const el = card(sc);
  const g = gridEl(sc.frames[0][0]);
  el.appendChild(g);

  const meta = document.createElement("div");
  meta.className = "meta";
  const kind = sc.loop ? '<span class="badge loop">loops</span>'
                       : '<span class="badge once">plays once</span>';
  meta.innerHTML = kind +
    '<span>' + sc.frames.length + " frames · " + total(sc) + " ms</span>" +
    '<span class="now"></span>';
  const btn = document.createElement("button");
  btn.textContent = "\\u21bb replay";
  meta.appendChild(btn);
  el.appendChild(meta);

  // static filmstrip: every frame at a glance
  const film = document.createElement("div");
  film.className = "film";
  sc.frames.forEach((f, i) => {
    const m = document.createElement("div");
    m.className = "mini";
    m.appendChild(gridEl(f[0]));
    const t = document.createElement("div");
    t.className = "t";
    t.textContent = f[1] + "ms";
    m.appendChild(t);
    film.appendChild(m);
  });
  el.appendChild(film);

  animsBox.appendChild(el);
  const st = { sc, g, t0: performance.now(), now: meta.querySelector(".now") };
  btn.addEventListener("click", () => { st.t0 = performance.now(); });
  animes.push(st);
}

function frameIndex(sc, t) {
  const tot = total(sc);
  let x = sc.loop ? t % tot : Math.min(t, tot - 1);
  for (let i = 0; i < sc.frames.length; i++) {
    if (x < sc.frames[i][1]) return i;
    x -= sc.frames[i][1];
  }
  return sc.frames.length - 1;
}

function tick(now) {
  for (const st of animes) {
    const idx = frameIndex(st.sc, now - st.t0);
    paint(st.g, st.sc.frames[idx][0]);
    st.now.textContent = "frame " + (idx + 1) + "/" + st.sc.frames.length;
  }
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
</script>
</body>
</html>
"""


def build():
    data = [
        {
            "key": sc["key"],
            "label": sc["label"],
            "hours": sc["hours"],
            "concept": sc["concept"],
            "loop": sc["loop"],
            "still": sc["still"],
            "frames": [[rows, dur] for rows, dur in sc["frames"]],
        }
        for sc in draft.SCENES
    ]
    return HTML.replace("__DATA__", json.dumps(data))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "scene_preview.html"),
        help="output HTML (default: repo root/scene_preview.html)")
    args = ap.parse_args()
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(build())
    print("wrote %s (%d scenes)" % (args.out, len(draft.SCENES)))


if __name__ == "__main__":
    main()
