#!/usr/bin/env python3
# Renders panel-preview.png: the panel SVG with Rack's stock knobs and jacks
# placed on top, and a mock-up of the display. Needs a Rack source checkout for
# the component SVGs and Playwright with Chromium.
#   RACK_RES=/path/to/Rack/res python3 tools/preview.py
import base64, math, os, re, subprocess, sys, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.environ.get("RACK_RES", "Rack/res")
LIB = os.path.join(RES, "ComponentLibrary")
PX = 75 / 25.4          # Rack pixels per mm
SCALE = 3.0             # output pixels per Rack pixel


def uri(path):
    return "data:image/svg+xml;base64," + base64.b64encode(open(path, "rb").read()).decode()


def size(path):
    s = open(path).read(3000)
    w = re.search(r'width="([\d.]+)(mm|px)?"', s)
    h = re.search(r'height="([\d.]+)(mm|px)?"', s)
    f = lambda m: float(m.group(1)) * (PX if m.group(2) == "mm" else 1)
    return f(w), f(h)


items = []


def place(name, x_mm, y_mm, angle=None, centered=True):
    p = os.path.join(LIB, name + ".svg")
    w, h = size(p)
    x, y = x_mm * PX, y_mm * PX
    if centered:
        x -= w / 2
        y -= h / 2
    rot = f"transform:rotate({angle}rad);" if angle is not None else ""
    items.append(f'<img src="{uri(p)}" style="position:absolute;left:{x * SCALE}px;top:{y * SCALE}px;'
                 f'width:{w * SCALE}px;height:{h * SCALE}px;{rot}">')


def knob(kind, x, y, v, amin, amax):
    place(kind + "_bg", x, y)
    place(kind, x, y, amin + (amax - amin) * v)


R = 0.83 * math.pi
T = 0.75 * math.pi
COL_X = [15.2, 45.7, 76.2, 106.7]
KNOB_Y = [57.5, 84.5]
SMALL_Y = [67.0, 94.0]
big = [0.35, 0.5, 0.55, 0.0, 0.8, 0.7071, 0.5, 0.15]
small = [0.6, 0.3, 0.4, 0.2, 0.15, 0.3, 0.5, 0.5]
for i in range(8):
    x, r = COL_X[i % 4], i // 4
    knob("RoundBlackKnob", x, KNOB_Y[r], big[i], -R, R)
    if i == 7:
        knob("RoundSmallBlackKnob", x - 6, SMALL_Y[r], small[i], -R, R)
    else:
        knob("Trimpot", x - 6, SMALL_Y[r], small[i], -T, T)
    place("PJ301M", x + 6, SMALL_Y[r])
place("CKSSThreeHorizontal_0", 18.0, 106.5)
place("CKSS_0", 45.7, 106.5)
place("VCVBezel", 63.5, 106.5)
knob("RoundSmallBlackKnob", 108.0, 106.5, 0.2, -R, R)
for y in [24.0, 32.5, 41.0, 49.5, 59.0]:
    place("VCVBezel", 134.6, y)
place("CKSS_0", 134.6, 74.0)
place("PJ301M", 134.6, 87.0)
knob("RoundSmallBlackKnob", 126.4, 108.0, 0.3, -R, R)
place("PJ301M", 135.8, 108.0)
for x in [14.5, 25.0, 35.5, 51.0, 61.5, 72.0, 82.5, 93.0, 103.5]:
    place("PJ301M", x, 120.5)
W_MM = 142.24
for (x, y) in [(15 / PX, 0), (W_MM - 30 / PX, 0), (15 / PX, 128.5 - 15 / PX), (W_MM - 30 / PX, 128.5 - 15 / PX)]:
    place("ScrewSilver", x, y, centered=False)

font = base64.b64encode(open(os.path.join(RES, "fonts", "ShareTechMono-Regular.ttf"), "rb").read()).decode()
panel = uri(os.path.join(ROOT, "res", "Randrum.svg"))
Wpx, Hpx = W_MM * PX * SCALE, 128.5 * PX * SCALE
disp = dict(x=4.6 * PX * SCALE, y=13.0 * PX * SCALE, w=112.7 * PX * SCALE, h=32.6 * PX * SCALE, mm=PX * SCALE)

html = f"""<!doctype html><html><head><style>
@font-face {{ font-family: stm; src: url(data:font/ttf;base64,{font}); }}
body {{ margin:0; background:#111; }}
#p {{ position:relative; width:{Wpx}px; height:{Hpx}px; background:url({panel}); background-size:100% 100%; }}
canvas {{ position:absolute; }}
</style></head><body><div id="p">
<canvas id="d" width="{disp['w']}" height="{disp['h']}" style="left:{disp['x']}px;top:{disp['y']}px"></canvas>
{''.join(items)}
<div style="position:absolute;left:{134.6 * PX * SCALE - 11}px;top:{24.0 * PX * SCALE - 11}px;width:22px;height:22px;border-radius:11px;background:#f5d040;opacity:0.9;box-shadow:0 0 12px #f5d040"></div>
<div style="position:absolute;left:{132.5 * PX * SCALE - 11}px;top:{0}px;display:none"></div>
<div style="position:absolute;left:{(82.5 + 4.4) * PX * SCALE - 6}px;top:{(120.5 - 5.4) * PX * SCALE - 6}px;width:12px;height:12px;border-radius:6px;background:#f5d040;box-shadow:0 0 10px #f5d040"></div>
</div>
<script>
const D = {json.dumps(disp)};
document.fonts.load('20px stm').then(draw);
function draw() {{
  const c = document.getElementById('d').getContext('2d');
  const mm = D.mm, W = D.w, H = D.h;
  const C = {{bg:'#121216', ink:'#EEE8DC', soft:'#86827A', dim:'#34333A', hot:'#F26B3A', win:'#3F8E9A', env:'#F2D050', wave:'#B8B2A6', skip:'#6C5CA8'}};
  c.fillStyle = C.bg; c.beginPath(); c.roundRect(0,0,W,H,1.2*mm); c.fill();
  const pad = 1.8*mm, fs = 2.6*mm, fss = 2.2*mm;
  const t = (x,y,s,col,al,size) => {{ c.font = size+'px stm'; c.textAlign = al; c.fillStyle = col; c.fillText(s,x,y); }};
  const ringX0 = W*0.72, leftW = ringX0 - 2*pad;
  t(pad, pad+fs, '808 kit  24', C.ink, 'left', fs);
  t(ringX0-pad, pad+fs, 'RANDOM', C.soft, 'right', fss);
  const y2 = pad + fs*2.25;
  t(pad, y2, '09 rim_tight_02', C.hot, 'left', fss);
  t(ringX0-pad, y2, '+2.4st 180ms -3dB L35 x3', C.soft, 'right', fss);
  const wy = y2 + 1.4*mm, wh = H - wy - 6.2*mm, wx = pad, ww = leftW;
  c.fillStyle = '#1A1A20'; c.fillRect(wx, wy, ww, wh);
  c.fillStyle = C.wave;
  for (let i = 0; i < ww; i++) {{
    const f = i/ww; const a = Math.min(1, Math.exp(-f*5)*(0.75+0.25*Math.sin(i*0.9)*Math.sin(i*0.13)) + 0.02) * wh*0.48;
    c.fillRect(wx+i, wy+wh/2-a, 1, Math.max(0.6, 2*a));
  }}
  const st = 0.06; c.fillStyle = 'rgba(18,18,22,0.75)'; c.fillRect(wx, wy, ww*st, wh);
  c.strokeStyle = C.env; c.lineWidth = 3; c.beginPath();
  for (let i = 0; i <= 64; i++) {{ const fx = st + (1-st)*i/64; const e = Math.exp(-6.9*(fx-st)*0.9/0.18*0.25); const px = wx+ww*fx, py = wy+wh-e*wh; i ? c.lineTo(px,py) : c.moveTo(px,py); }}
  c.stroke();
  c.fillStyle = C.hot; c.fillRect(wx+ww*st, wy, 3, wh);
  c.fillStyle = C.ink; c.fillRect(wx+ww*0.21, wy, 2.5, wh);
  const n = 24, lo = 4, size = 14, idx = 8, cw = ww/n, sy = H - 4.6*mm, sh = 2.6*mm;
  for (let i = 0; i < n; i++) {{ const off = ((i-lo)%n+n)%n; c.fillStyle = i==idx ? C.hot : (off < size ? C.win : C.dim); c.fillRect(wx+i*cw+1.5, sy, cw-3, sh); }}
  c.fillStyle = C.dim; c.fillRect(ringX0, pad*0.5, 3, H-pad);
  const rcx = (ringX0+W)/2, rcy = H*0.54, rr = Math.min(W-ringX0, H)*0.34*0.82;
  const plays = [1,1,0,1,1,1,0,1], lev = [1,0.5,0.8,0.7,1,0.4,0.9,0.6];
  for (let i = 0; i < 8; i++) {{
    const a = -Math.PI/2 + 2*Math.PI*i/8, x = rcx+rr*Math.cos(a), y = rcy+rr*Math.sin(a);
    const r = mm*(0.55+0.75*lev[i]), cur = i==2+1;
    const lr = rr + 2.2*mm; c.textBaseline='middle'; t(rcx+lr*Math.cos(a), rcy+lr*Math.sin(a), 'ABACBCAB'[i], cur ? C.hot : C.win, 'center', fss*0.8); c.textBaseline='alphabetic';
    c.beginPath(); c.arc(x,y,r,0,2*Math.PI);
    if (plays[i]) {{ c.fillStyle = cur ? C.hot : C.ink; c.fill(); }}
    if (i==4) {{ c.beginPath(); c.arc(x,y,r+0.55*mm,0,2*Math.PI); c.strokeStyle=C.env; c.lineWidth=2.4; c.stroke(); }} else {{ c.strokeStyle = cur ? C.hot : C.soft; c.lineWidth = 3; c.stroke(); }}
  }}
  c.textBaseline = 'middle';
  t(rcx, rcy-fs*0.15, '8', C.ink, 'center', fs*1.3);
  t(rcx, rcy+fs*0.95, 'mut 15%', C.soft, 'center', fss*0.9);
  t(rcx, rcy+fs*1.85, '3 sounds', C.win, 'center', fss*0.9);
  c.textBaseline = 'alphabetic';
  t(rcx, pad+fs, 'LOOP  A~', C.ink, 'center', fss);
  document.title = 'done';
}}
</script></body></html>"""
out_html = os.path.join(os.environ.get("TMPDIR", "/tmp"), "randrum_preview.html")
open(out_html, "w").write(html)
js = f"""
const {{ chromium }} = require('playwright');
(async () => {{
  const b = await chromium.launch();
  const p = await b.newPage({{ viewport: {{ width: {int(Wpx)}, height: {int(Hpx)} }} }});
  await p.goto('file://{out_html}');
  await p.waitForFunction(() => document.title === 'done');
  await p.locator('#p').screenshot({{ path: '{os.path.join(ROOT, "panel-preview.png")}' }});
  await b.close();
}})();
"""
subprocess.run(["node", "-e", js], check=True)
print("wrote panel-preview.png")
