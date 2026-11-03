#!/usr/bin/env python3
# Draws res/Randrum.svg. VCV Rack can't draw SVG <text>, so every label is
# turned into outline paths with fontTools. Positions are in mm and must match
# src/Randrum.cpp.
#   pip install fonttools
#   python3 res/make_panel.py
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")

W, H = 142.24, 128.5  # 28 HP
MAIN_W = 121.92       # the original 24 HP; the MEMORY column sits to the right

BG = "#27232C"
BG_DARK = "#1E1B22"
INK = "#EFE8DA"
SOFT = "#A79FB2"
HOT = "#F26B3A"
TEAL = "#4FA9B5"
OUTBOX = "#141217"

fonts = {}


def text(s, x, y, size, font=FONT_MED, fill=INK, anchor="middle", spacing=0.04):
    """Return an SVG path for string s; (x, y) is the baseline point in mm."""
    if font not in fonts:
        f = TTFont(font)
        fonts[font] = (f, f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    f, gs, cmap, upm = fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    if anchor == "middle":
        x0 = x - width * scale / 2
    elif anchor == "end":
        x0 = x - width * scale
    else:
        x0 = x
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        tp = TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y))
        gs[n].draw(tp)
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


parts = []
add = parts.append


def die(x, y, s, pips, fill=INK, pip=BG):
    """A small die face, top-left at (x, y), size s, showing 1-6 pips."""
    add(f'<rect x="{x}" y="{y}" width="{s}" height="{s}" rx="{s * 0.22:.2f}" fill="{fill}"/>')
    q = s / 4
    spots = {
        1: [(2, 2)], 2: [(1, 1), (3, 3)], 3: [(1, 1), (2, 2), (3, 3)],
        4: [(1, 1), (3, 1), (1, 3), (3, 3)], 5: [(1, 1), (3, 1), (2, 2), (1, 3), (3, 3)],
        6: [(1, 1), (3, 1), (1, 2), (3, 2), (1, 3), (3, 3)],
    }[pips]
    for (a, b) in spots:
        add(f'<circle cx="{x + a * q:.2f}" cy="{y + b * q:.2f}" r="{s * 0.09:.2f}" fill="{pip}"/>')


add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')
# faint scattered pips as a texture (positions fixed so the panel never changes)
import random
rnd = random.Random(7)
for _ in range(70):
    x, y = rnd.uniform(2, MAIN_W - 2), rnd.uniform(48, H - 30)
    add(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{rnd.uniform(0.25, 0.55):.2f}" fill="{BG_DARK}"/>')

# title
add(text("RANDRUM", 6.0, 10.4, 4.6, FONT_BOLD, INK, anchor="start", spacing=0.12))
die(36.0, 6.5, 4.0, 5, HOT, BG)
die(41.0, 6.5, 4.0, 2, INK, BG)
add(text("a bag of drums, a handful of dice", MAIN_W - 6.0, 10.0, 2.1, FONT_MED, SOFT, anchor="end"))

# display window
add(f'<rect x="4.0" y="12.4" width="113.9" height="33.8" rx="1.6" fill="#0C0B0E"/>')

COL_X = [15.2, 45.7, 76.2, 106.7]
ROW_LABEL_Y = [51.0, 78.0]
ROW_SMALL_Y = [67.0, 94.0]
DX = 6.0
blocks = [
    ("SAMPLE", "SPREAD", "CV", "first", "last"),
    ("PITCH", "RND", "V/OCT", "-2 oct", "+2 oct"),
    ("DECAY", "RND", "CV", "short", "full"),
    ("START", "RND", "CV", "", ""),
    ("CHANCE", "REV", "CV", "never", "always"),
    ("LEVEL", "RND", "VEL", "", ""),
    ("PAN", "RND", "CV", "L", "R"),
    ("MUTATE", "LOOP", "CV", "locked", "wild"),
]
for i, (big, small, cv, lo, hi) in enumerate(blocks):
    x = COL_X[i % 4]
    r = i // 4
    hot = big in ("MUTATE",)
    add(text(big, x, ROW_LABEL_Y[r], 2.6, FONT_BOLD, HOT if hot else INK, spacing=0.07))
    ky = ROW_LABEL_Y[r] + 6.5
    if lo:
        add(text(lo, x - 7.6, ky + 5.2, 1.6, FONT_MED, SOFT))
    if hi:
        add(text(hi, x + 7.6, ky + 5.2, 1.6, FONT_MED, SOFT))
    sy = ROW_SMALL_Y[r]
    add(text(small, x - DX, sy + 6.0, 1.75, FONT_BOLD, TEAL, spacing=0.05))
    add(text(cv, x + DX, sy + 6.0, 1.75, FONT_BOLD, SOFT, spacing=0.05))

# separators
for y in (47.6, 101.2):
    add(f'<line x1="4" y1="{y}" x2="{MAIN_W - 3}" y2="{y}" stroke="{BG_DARK}" stroke-width="0.5"/>')

# ---- MEMORY / RATCHET column (positions match src/Randrum.cpp) ----
CX = 131.3
add(f'<rect x="{MAIN_W + 0.2}" y="12.4" width="{W - MAIN_W - 1.8}" height="108.4" rx="1.4" fill="{BG_DARK}"/>')
add(text("MEMORY", CX, 17.6, 2.3, FONT_BOLD, HOT, spacing=0.08))
for i, y in enumerate([24.0, 32.5, 41.0, 49.5]):
    add(text("ABCD"[i], 125.8, y + 1.2, 3.0, FONT_BOLD, INK))
add(text("SAVE", 126.2, 59.8, 1.9, FONT_BOLD, INK))
add(text("then A-D", CX, 64.9, 1.4, FONT_MED, SOFT))
add(text("morph", 126.8, 71.4, 1.6, FONT_MED, SOFT))
add(text("jump", 126.8, 77.0, 1.6, FONT_MED, SOFT))
add(text("PAT", 126.2, 86.3, 1.9, FONT_BOLD, INK))
add(text("CV A-D", 126.2, 89.0, 1.35, FONT_MED, SOFT))
add(f'<line x1="{MAIN_W + 2}" y1="94.6" x2="{W - 3.5}" y2="94.6" stroke="{BG}" stroke-width="0.5"/>')
add(text("RATCHET", CX, 100.0, 2.1, FONT_BOLD, HOT, spacing=0.06))
add(text("chance", 126.4, 116.2, 1.45, FONT_MED, SOFT))
add(text("CV", 135.8, 116.2, 1.6, FONT_BOLD, SOFT))

# controls row
CTRL_Y = 106.5
add(text("PICK", 7.0, CTRL_Y + 0.8, 2.0, FONT_BOLD, INK, anchor="start"))
add(text("rnd", 13.2, CTRL_Y + 4.6, 1.5, FONT_MED, SOFT))
add(text("walk", 18.0, CTRL_Y + 4.6, 1.5, FONT_MED, SOFT))
add(text("cycle", 22.9, CTRL_Y + 4.6, 1.5, FONT_MED, SOFT))
add(text("choke", 49.2, CTRL_Y - 1.8, 1.6, FONT_MED, SOFT, anchor="start"))
add(text("overlap", 49.2, CTRL_Y + 3.3, 1.6, FONT_MED, SOFT, anchor="start"))
add(text("REROLL", 68.5, CTRL_Y + 0.8, 2.0, FONT_BOLD, INK, anchor="start"))
add(text("new loop", 68.5, CTRL_Y + 3.6, 1.5, FONT_MED, SOFT, anchor="start"))
add(text("VARIETY", 102.5, CTRL_Y + 0.8, 2.0, FONT_BOLD, TEAL, anchor="end"))
add(text("sounds in loop", 102.5, CTRL_Y + 3.6, 1.5, FONT_MED, SOFT, anchor="end"))
add(text("1", 103.6, CTRL_Y + 4.6, 1.4, FONT_MED, SOFT))
add(text("all", 112.8, CTRL_Y + 4.6, 1.4, FONT_MED, SOFT))

# jacks
JACK_Y = 120.5
IN_X = [14.5, 25.0, 35.5]
OUT_X = [51.0, 61.5, 72.0, 82.5, 93.0, 103.5]
add(f'<rect x="45.0" y="112.0" width="65.0" height="14.8" rx="1.2" fill="{OUTBOX}"/>')
for x, lab in zip(IN_X, ["TRIG", "RESET", "REROLL"]):
    add(text(lab, x, 115.0, 1.95, FONT_BOLD, HOT if lab == "TRIG" else INK))
for x, lab in zip(OUT_X, ["OUT L", "OUT R", "ENV", "HIT", "SKIP", "IDX"]):
    add(text(lab, x, 115.0, 1.95, FONT_BOLD, INK))
add(text("mono if R empty", 56.25, 126.1, 1.35, FONT_MED, SOFT))

svg = (
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" '
    f'viewBox="0 0 {W} {H}">\n' + "\n".join(parts) + "\n</svg>\n"
)
with open(os.path.join(HERE, "Randrum.svg"), "w") as fh:
    fh.write(svg)
print("wrote Randrum.svg")
