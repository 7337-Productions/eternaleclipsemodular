#!/usr/bin/env python3
"""Generate res/ConclaveOfHollowRadiance.svg (48 HP).

The Circle and Triangle of Solomon from the Goetia, turned so the Triangle
of Art points east. The Circle is a double ring with the serpent's band
between the two lines carrying the divine names (Cinzel, the face of the
Medallions of the Seventy-Two, baked to paths: Rack's renderer ignores SVG
text); the four quarter seals on the band are where the FEED lights sit.
Inside, the chain ring runs through the four LEVEL knobs. The Triangle holds
MASTER in its inner circle (the limiter's glow, phase 5), with
TETRAGRAMMATON, PRIMEUMATON and ANAPHAXETON along its sides and MI-CHA-EL
within. Input bay on the far left, outs and loop on the far right.

The header divider and brand mark are lifted verbatim from res/Syzygy.svg and
recentred so every panel shares one copy of that artwork. Geometry constants
mirror src/ConclaveOfHollowRadiance/ConclaveOfHollowRadiance.cpp.
Run from anywhere:  python tools/gen_conclave_panel.py
"""
import math
import pathlib
import re

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

ROOT = pathlib.Path(__file__).resolve().parents[1]
SYZYGY = (ROOT / "res" / "Syzygy.svg").read_text(encoding="utf-8")
FONT_PATH = pathlib.Path(r"C:\Users\nicho\Claude\RelicsOfTheEternalEclipse\medallions-of-the-seventy-two\fonts\Cinzel-Variable.ttf")

W, H = 243.84, 128.5
CX_PANEL = W / 2.0
SYZYGY_CX = 101.6
SHIFT = CX_PANEL - SYZYGY_CX
TOP, BOTTOM = 18.0, 122.9

# ---- geometry (mirrored in the .cpp) ----
BAY_L_X, BAY_L_W = 3.0, 38.0
BAY_R_X, BAY_R_W = 207.0, 33.84

CX, CY = 99.5, 70.45          # the Circle (the Coronal Annihilator's 53 mm ring); 5.5 mm off the left bay
R_OUT, R_IN = 53.0, 45.0      # the two rings; the serpent's band lies between
R_TEXT = 49.0                 # centre line of the band
R_TRIO = 39.5                 # SEND / FEED / TUNE knobs
R_LEVEL = 22.0                # LEVEL knobs; the chain ring runs through them
R_SEAL = 3.4                  # quarter seals (hexagrams) on the band
R_CORE = 6.5                  # the hexagram at the heart
SEAL_GAP_DEG = 6.5            # the band's names stop this far either side of a seal

TRI_BASE_X, TRI_HALF, TRI_APEX_X = 156.5, 26.0, 201.5   # the Triangle of Art, pointing east; apex 5.5 mm off the right bay
TRI_CX = TRI_BASE_X + (TRI_APEX_X - TRI_BASE_X) / 3.0   # centroid x (171.5)
TRI_CY = CY
R_TRI_INNER = 13.0

# Palette (panel standards)
COPPER = "#cd762b"
GOLD = "#ffc464"
LINE = "#6b4522"
HAIR = "#382110"
FRAME = "#140c06"
WELL = "#070302"

CAP_MM = 1.9   # cap height of the baked lettering, as the brand mark


def f(x):
    return f"{x:.3f}"


def section(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


# ---- lettering ----
_font = TTFont(str(FONT_PATH))
_font = instancer.instantiateVariableFont(_font, {"wght": 600})
_cmap = _font.getBestCmap()
_glyphs = _font.getGlyphSet()
_hmtx = _font["hmtx"]
_UPM = _font["head"].unitsPerEm
_CAP = _font["OS/2"].sCapHeight
SCALE = CAP_MM / _CAP   # mm per font unit
TRACK = 0.03 * _UPM     # extra letter spacing, font units


def advance(ch):
    return _hmtx[_cmap[ord(ch)]][0] + TRACK


def text_width(s):
    return sum(advance(c) for c in s) * SCALE


def glyph_path(ch, transform):
    """SVG path data for one glyph under an affine transform (a b c d e f)."""
    name = _cmap[ord(ch)]
    pen = SVGPathPen(_glyphs)
    _glyphs[name].draw(TransformPen(pen, transform))
    return pen.getCommands()


def text_along_arc(s, cx, cy, r_baseline, centre_deg, clockwise=True):
    """Letters following a circle, baseline on r_baseline, centred on centre_deg
    (SVG angle: 0 east, 90 south). Reads clockwise seen from outside."""
    total = text_width(s)
    paths = []
    sweep = total / r_baseline  # radians
    a = math.radians(centre_deg) - sweep / 2.0
    for ch in s:
        w = advance(ch) * SCALE
        a_mid = a + (w / 2.0) / r_baseline
        # glyph origin at its own centre, baseline on the circle, upright at the top
        rot = a_mid + math.pi / 2.0
        cos, sin = math.cos(rot), math.sin(rot)
        px = cx + r_baseline * math.cos(a_mid)
        py = cy + r_baseline * math.sin(a_mid)
        # local: x right along tangent, y up (font units) -> flip y, scale, rotate, translate
        # matrix = T(px,py) . R(rot) . S(SCALE, -SCALE) . T(-w/2 in font units, 0)
        half = (w / 2.0) / SCALE
        a_ = SCALE * cos
        b_ = SCALE * sin
        c_ = SCALE * sin
        d_ = -SCALE * cos
        e_ = px - a_ * half
        f_ = py - b_ * half
        d = glyph_path(ch, (a_, b_, c_, d_, e_, f_))
        if d:
            paths.append(d)
        a += w / r_baseline
    return paths


def text_along_line(s, x0, y0, x1, y1, offset):
    """Letters along the segment (x0,y0)->(x1,y1), centred. The letters' tops
    point along the left-hand normal of the travel direction (for text running
    left to right on screen that is up). `offset` moves the baseline along
    that normal, in mm."""
    dx, dy = x1 - x0, y1 - y0
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    nx, ny = uy, -ux          # left-hand normal (screen coords, y down)
    total = text_width(s)
    sx = (x0 + x1) / 2.0 - ux * total / 2.0 + nx * offset
    sy = (y0 + y1) / 2.0 - uy * total / 2.0 + ny * offset
    paths = []
    run = 0.0
    for ch in s:
        w = advance(ch) * SCALE
        px = sx + ux * run
        py = sy + uy * run
        # glyph x (font units, along the baseline) -> u; glyph y (up) -> n
        d = glyph_path(ch, (SCALE * ux, SCALE * uy, SCALE * nx, SCALE * ny, px, py))
        if d:
            paths.append(d)
        run += w
    return paths


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return cx + r * math.cos(a), cy + r * math.sin(a)


def hexagram(cx, cy, r):
    pts_up = [polar(cx, cy, r, -90 + 120 * k) for k in range(3)]
    pts_dn = [polar(cx, cy, r, 90 + 120 * k) for k in range(3)]
    return ("M " + " L ".join(f"{f(x)},{f(y)}" for x, y in pts_up) + " Z "
            "M " + " L ".join(f"{f(x)},{f(y)}" for x, y in pts_dn) + " Z")


def frame(x, w, y0=TOP, y1=BOTTOM, fill=FRAME):
    return f'  <rect x="{f(x)}" y="{f(y0)}" width="{f(w)}" height="{f(y1 - y0)}" rx="2" fill="{fill}" stroke="{HAIR}" stroke-width="0.4"/>\n'


def brackets(x, w, y0=TOP, y1=BOTTOM):
    return (
        f'    <path d="M {f(x)},{f(y0 + 2.4)} L {f(x)},{f(y0)} L {f(x + 2.4)},{f(y0)}"/>\n'
        f'    <path d="M {f(x + w)},{f(y1 - 2.4)} L {f(x + w)},{f(y1)} L {f(x + w - 2.4)},{f(y1)}"/>\n'
    )


# ---- shared header and brand ----
divider = section(SYZYGY, "  <!-- Header divider", "  <!-- Chamber glyph")
divider = re.sub(r'x([12])="([0-9.]+)"', lambda m: f'x{m.group(1)}="{float(m.group(2)) + SHIFT:.2f}"', divider)
brand = section(SYZYGY, "  <!-- Brand mark", "</svg>")
brand = brand.replace(f"translate({SYZYGY_CX} 127.9)", f"translate({CX_PANEL:.2f} 127.9)")

gx, gy, gr = CX_PANEL, 10.8, 1.7
tri_glyph = f"M {f(gx - gr * 0.75)},{f(gy - gr)} L {f(gx - gr * 0.75)},{f(gy + gr)} L {f(gx + gr * 1.05)},{f(gy)} Z"
glyph = (
    "  <!-- Chamber glyph: the Triangle of Art, pointing east, its circle within -->\n"
    f'  <path d="{tri_glyph}" fill="none" stroke="{GOLD}" stroke-width="0.3" stroke-opacity="0.9" stroke-linejoin="round"/>\n'
    f'  <circle cx="{f(gx - gr * 0.15)}" cy="{f(gy)}" r="0.5" fill="none" stroke="{GOLD}" stroke-width="0.3" stroke-opacity="0.9"/>\n'
)

svg = []
svg.append('<?xml version="1.0" encoding="UTF-8"?>\n')
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">\n')
svg.append("  <!-- Panel background -->\n")
svg.append(f'  <rect x="0" y="0" width="{W}" height="{H}" fill="#000000"/>\n\n')
svg.append(divider)
svg.append(glyph)

# ---- bays ----
svg.append("\n  <!-- Bays: inputs and level CV on the left; outs, loop and master on the right -->\n")
svg.append(frame(BAY_L_X, BAY_L_W))
svg.append(frame(BAY_R_X, BAY_R_W))
svg.append(f'  <g fill="none" stroke="{COPPER}" stroke-width="0.4" stroke-opacity="0.9">\n')
svg.append(brackets(BAY_L_X, BAY_L_W))
svg.append(brackets(BAY_R_X, BAY_R_W))
svg.append("  </g>\n")

# ---- the Circle ----
svg.append("\n  <!-- The Circle: two rings, the serpent's band between them -->\n")
svg.append(f'  <circle cx="{f(CX)}" cy="{f(CY)}" r="{f(R_TEXT)}" fill="none" stroke="{FRAME}" stroke-width="{f(R_OUT - R_IN)}"/>\n')
svg.append(f'  <circle cx="{f(CX)}" cy="{f(CY)}" r="{f(R_OUT)}" fill="none" stroke="{COPPER}" stroke-width="0.45"/>\n')
svg.append(f'  <circle cx="{f(CX)}" cy="{f(CY)}" r="{f(R_IN)}" fill="none" stroke="{COPPER}" stroke-width="0.45"/>\n')

# The names, beginning at EHEIEH and ending at LEVANAH, around the band
# clockwise from the north seal, one run per quarter.
NAMES = [
    "EHEIEH · IAH · IEHOVAH ELOHIM",
    "EL · ELOHIM GIBOR · ELOAH VA-DAATH",
    "IEHOVAH TZABAOTH · ELOHIM TZABAOTH",
    "SHADDAI EL CHAI · LEVANAH",
]
QUARTER_CENTRES = [-45.0, 45.0, 135.0, 225.0]
arc_mm = math.radians(90.0 - 2 * SEAL_GAP_DEG) * (R_TEXT - CAP_MM / 2.0)
svg.append(f'  <g fill="{COPPER}">\n')
for name, centre in zip(NAMES, QUARTER_CENTRES):
    width = text_width(name)
    if width > arc_mm:
        raise SystemExit(f"'{name}' is {width:.1f} mm, longer than its {arc_mm:.1f} mm arc")
    for d in text_along_arc(name, CX, CY, R_TEXT - CAP_MM / 2.0, centre):
        svg.append(f'    <path d="{d}"/>\n')
svg.append("  </g>\n")

# Serpent: head before EHEIEH, tail after LEVANAH, both just inside the band
head_a = -90.0 + SEAL_GAP_DEG + 1.5
hx, hy = polar(CX, CY, R_TEXT, head_a)
tangent = math.radians(head_a + 90.0)
tx, ty = math.cos(tangent), math.sin(tangent)      # direction of travel (clockwise)
nx, ny = math.cos(math.radians(head_a)), math.sin(math.radians(head_a))  # outward
# a leaf-shaped head pointing against the travel direction (it leads the names)
head = (f"M {f(hx - tx * 3.6)},{f(hy - ty * 3.6)} "
        f"Q {f(hx - tx * 1.1 + nx * 2.1)},{f(hy - ty * 1.1 + ny * 2.1)} {f(hx + tx * 1.6)},{f(hy + ty * 1.6)} "
        f"Q {f(hx - tx * 1.1 - nx * 2.1)},{f(hy - ty * 1.1 - ny * 2.1)} {f(hx - tx * 3.6)},{f(hy - ty * 3.6)} Z")
ex, ey = hx - tx * 1.6 + nx * 0.8, hy - ty * 1.6 + ny * 0.8
svg.append("  <!-- The serpent: its head leads the names, its tail follows them -->\n")
svg.append(f'  <path d="{head}" fill="{FRAME}" stroke="{GOLD}" stroke-width="0.35"/>\n')
svg.append(f'  <circle cx="{f(ex)}" cy="{f(ey)}" r="0.35" fill="{GOLD}"/>\n')
tail_a = 270.0 - SEAL_GAP_DEG - 1.5
tx0, ty0 = polar(CX, CY, R_TEXT + 1.8, tail_a - 6.0)
tx1, ty1 = polar(CX, CY, R_TEXT - 1.8, tail_a - 6.0)
tpx, tpy = polar(CX, CY, R_TEXT, tail_a + 0.5)
svg.append(f'  <path d="M {f(tx0)},{f(ty0)} Q {f(tpx)},{f(tpy)} {f(tx1)},{f(ty1)}" fill="none" stroke="{GOLD}" stroke-width="0.35"/>\n')

# Quarter seals: hexagrams on the band at N E S W, each the place of a FEED light
svg.append("  <!-- Quarter seals: the four FEED lights sit at their hearts -->\n")
for deg in (-90.0, 0.0, 90.0, 180.0):
    sx, sy = polar(CX, CY, R_TEXT, deg)
    svg.append(f'  <circle cx="{f(sx)}" cy="{f(sy)}" r="{f(R_SEAL + 0.9)}" fill="#000000"/>\n')
    svg.append(f'  <path d="{hexagram(sx, sy, R_SEAL)}" fill="none" stroke="{GOLD}" stroke-width="0.35" stroke-linejoin="round"/>\n')

# The chain ring through the LEVEL knobs, with the direction of the chain
svg.append("  <!-- The chain: signal passes clockwise from channel to channel -->\n")
svg.append(f'  <circle cx="{f(CX)}" cy="{f(CY)}" r="{f(R_LEVEL)}" fill="none" stroke="{LINE}" stroke-width="0.65"/>\n')
for deg in (-45.0, 45.0, 135.0, 225.0):
    ax, ay = polar(CX, CY, R_LEVEL, deg)
    t = math.radians(deg + 90.0)
    ux, uy = math.cos(t), math.sin(t)
    px, py = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    svg.append(f'  <path d="M {f(ax - ux * 1.6 + px * 1.1)},{f(ay - uy * 1.6 + py * 1.1)} L {f(ax + ux * 0.9)},{f(ay + uy * 0.9)} L {f(ax - ux * 1.6 - px * 1.1)},{f(ay - uy * 1.6 - py * 1.1)}" fill="none" stroke="{LINE}" stroke-width="0.65" stroke-linejoin="round"/>\n')
svg.append(f'  <path d="{hexagram(CX, CY, R_CORE)}" fill="none" stroke="{LINE}" stroke-width="0.5" stroke-linejoin="round"/>\n')

# ---- the Triangle of Art ----
svg.append("\n  <!-- The Triangle of Art, east of the Circle: the spirit manifests in its circle -->\n")
v0 = (TRI_BASE_X, CY - TRI_HALF)
v1 = (TRI_BASE_X, CY + TRI_HALF)
v2 = (TRI_APEX_X, CY)
svg.append(f'  <path d="M {f(v0[0])},{f(v0[1])} L {f(v2[0])},{f(v2[1])} L {f(v1[0])},{f(v1[1])} Z" fill="none" stroke="{COPPER}" stroke-width="0.5" stroke-linejoin="round"/>\n')
svg.append(f'  <circle cx="{f(TRI_CX)}" cy="{f(TRI_CY)}" r="{f(R_TRI_INNER)}" fill="{WELL}" stroke="{GOLD}" stroke-width="0.4"/>\n')
svg.append(f'  <g fill="{COPPER}">\n')
# Every name sits outside its side, a millimetre off the line. The base
# reads upward with its tops to the west; the upper slant reads down toward
# the apex with its tops outward; the lower slant reads up toward the apex
# the right way round, hanging below the edge.
EDGE_GAP = 1.0
for d in text_along_line("TETRAGRAMMATON", v1[0], v1[1], v0[0], v0[1], EDGE_GAP):
    svg.append(f'    <path d="{d}"/>\n')
for d in text_along_line("PRIMEUMATON", v0[0], v0[1], v2[0], v2[1], EDGE_GAP):
    svg.append(f'    <path d="{d}"/>\n')
for d in text_along_line("ANAPHAXETON", v1[0], v1[1], v2[0], v2[1], -(EDGE_GAP + CAP_MM)):
    svg.append(f'    <path d="{d}"/>\n')
svg.append("  </g>\n")
svg.append(f'  <g fill="{GOLD}" fill-opacity="0.9">\n')
for word, centre in (("MI", -90.0), ("CHA", 150.0), ("EL", 30.0)):
    for d in text_along_arc(word, TRI_CX, TRI_CY, R_TRI_INNER - 2.4 - CAP_MM / 2.0, centre):
        svg.append(f'    <path d="{d}"/>\n')
svg.append("  </g>\n\n")

svg.append(brand)
svg.append("</svg>\n")

out = ROOT / "res" / "ConclaveOfHollowRadiance.svg"
out.write_text("".join(svg), encoding="utf-8", newline="\n")
print(f"wrote {out} ({W} x {H} mm); band arc {arc_mm:.1f} mm, names "
      + ", ".join(f"{text_width(n):.1f}" for n in NAMES) + " mm")
