#!/usr/bin/env python3
"""Generate res/VesperSwarm.svg (40 HP).

The header divider and the bottom brand mark are lifted verbatim from
res/Syzygy.svg so every panel shares one copy of that artwork. Geometry
constants here mirror the ones in src/VesperSwarm/VesperSwarm.cpp.
Run from anywhere:  python tools/gen_vesper_panel.py
"""
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
SYZYGY = (ROOT / "res" / "Syzygy.svg").read_text(encoding="utf-8")

W, H = 203.2, 128.5
CX_PANEL = 101.6

# Frames
TOP, FRAME_H = 16.0, 106.5
STAR_X, STAR_W = 4.0, 100.0
CTRL_X, CTRL_W = 108.0, 46.0
BAY_X, BAY_W = 158.0, 41.2

# The star
STAR_CX, STAR_CY, STAR_R = 54.0, 69.25, 42.0


def f(x):
    return f"{x:.2f}"


def star_point(k, r):
    a = math.radians(-90.0 + 45.0 * k)
    return (STAR_CX + r * math.cos(a), STAR_CY + r * math.sin(a))


def section(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


divider = section(SYZYGY, "  <!-- Header divider", "  <!-- Chamber glyph")
brand = section(SYZYGY, "  <!-- Brand mark", "</svg>")

# Chamber glyph: a small eight-pointed star at the divider centre
glyph_pts = []
for k in range(16):
    r = 1.6 if k % 2 == 0 else 0.7
    a = math.radians(-90.0 + 22.5 * k)
    glyph_pts.append(f"{CX_PANEL + r * math.cos(a):.3f},{10.8 + r * math.sin(a):.3f}")
glyph = (
    "  <!-- Chamber glyph: the evening star, eight-pointed -->\n"
    f'  <path d="M {" L ".join(glyph_pts)} Z" fill="none" stroke="#ffc464" '
    'stroke-width="0.3" stroke-opacity="0.9" stroke-linejoin="round"/>\n'
)

# Octagram {8/3}: every point joined to the third one along
order = [0, 3, 6, 1, 4, 7, 2, 5]
star_path = "M " + " L ".join(f"{f(star_point(k, STAR_R)[0])},{f(star_point(k, STAR_R)[1])}" for k in order) + " Z"
spokes = []
for k in range(8):
    x1, y1 = star_point(k, 8.0)
    x2, y2 = star_point(k, 36.0)
    spokes.append(f'    <line x1="{f(x1)}" y1="{f(y1)}" x2="{f(x2)}" y2="{f(y2)}"/>')


def frame(x, w):
    return f'  <rect x="{f(x)}" y="{f(TOP)}" width="{f(w)}" height="{f(FRAME_H)}" rx="2" fill="#140c06" stroke="#382110" stroke-width="0.4"/>\n'


def brackets(x, w):
    y0, y1 = TOP, TOP + FRAME_H
    return (
        f'    <path d="M {f(x)},{f(y0 + 2.4)} L {f(x)},{f(y0)} L {f(x + 2.4)},{f(y0)}"/>\n'
        f'    <path d="M {f(x + w)},{f(y1 - 2.4)} L {f(x + w)},{f(y1)} L {f(x + w - 2.4)},{f(y1)}"/>\n'
    )


svg = []
svg.append('<?xml version="1.0" encoding="UTF-8"?>\n')
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">\n')
svg.append("  <!-- Panel background -->\n")
svg.append(f'  <rect x="0" y="0" width="{W}" height="{H}" fill="#000000"/>\n\n')
svg.append(divider)
svg.append(glyph)
svg.append("\n  <!-- Star frame: the eight singers stand on the points -->\n")
svg.append(frame(STAR_X, STAR_W))
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="{f(STAR_R)}" fill="none" stroke="#cd762b" stroke-width="0.3" stroke-opacity="0.35"/>\n')
svg.append('  <g stroke="#6b4522" stroke-width="0.65" stroke-linecap="round">\n')
svg.append("\n".join(spokes) + "\n")
svg.append("  </g>\n")
svg.append(f'  <path d="{star_path}" fill="none" stroke="#ffc464" stroke-width="0.5" stroke-linejoin="round"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="7" fill="#000000" stroke="#cd762b" stroke-width="0.3" stroke-opacity="0.6"/>\n')

svg.append("\n  <!-- Controls frame: pitch, swarm, vowel, breath, filter, envelope -->\n")
svg.append(frame(CTRL_X, CTRL_W))
svg.append(f'  <line x1="{f(CTRL_X + 3)}" y1="72" x2="{f(CTRL_X + CTRL_W - 3)}" y2="72" stroke="#382110" stroke-width="0.4"/>\n')
svg.append(f'  <line x1="{f(CTRL_X + 3)}" y1="95.5" x2="{f(CTRL_X + CTRL_W - 3)}" y2="95.5" stroke="#382110" stroke-width="0.4"/>\n')

svg.append("\n  <!-- Patch bay frame: CV inputs, then the stereo outputs -->\n")
svg.append(frame(BAY_X, BAY_W))
svg.append(f'  <line x1="{f(BAY_X + 3)}" y1="97" x2="{f(BAY_X + BAY_W - 3)}" y2="97" stroke="#382110" stroke-width="0.4"/>\n')

svg.append("\n  <!-- Corner brackets (Church card motif) -->\n")
svg.append('  <g fill="none" stroke="#cd762b" stroke-width="0.4" stroke-opacity="0.9">\n')
svg.append(brackets(STAR_X, STAR_W))
svg.append(brackets(CTRL_X, CTRL_W))
svg.append(brackets(BAY_X, BAY_W))
svg.append("  </g>\n\n")
svg.append(brand)
svg.append("</svg>\n")

out = ROOT / "res" / "VesperSwarm.svg"
out.write_text("".join(svg), encoding="utf-8")
print(f"wrote {out}")
