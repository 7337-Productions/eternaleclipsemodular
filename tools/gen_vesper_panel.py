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
CTRL_X, CTRL_W = 108.0, 46.0
BAY_X, BAY_W = 158.0, 41.2

# The star of Ishtar: eight slender points on a ring, two inset outlines
# following the edges, a bullseye at the heart. The singers' buttons sit
# just beyond the tips (see VesperSwarm.cpp) so the points stay sharp.
STAR_CX, STAR_CY, STAR_R = 54.0, 69.25, 40.0
STAR_INNER = 0.42 * STAR_R      # radius of the notches between points
STAR_INSETS = (1.6, 3.2)        # mm, the nested outlines


def f(x):
    return f"{x:.2f}"


def section(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


divider = section(SYZYGY, "  <!-- Header divider", "  <!-- Chamber glyph")
brand = section(SYZYGY, "  <!-- Brand mark", "</svg>")

# Chamber glyph: a small eight-pointed star at the divider centre
glyph_pts = []
for k in range(16):
    r = 1.6 if k % 2 == 0 else 0.42 * 1.6
    a = math.radians(-90.0 + 22.5 * k)
    glyph_pts.append(f"{CX_PANEL + r * math.cos(a):.3f},{10.8 + r * math.sin(a):.3f}")
glyph = (
    "  <!-- Chamber glyph: the evening star, eight-pointed -->\n"
    f'  <path d="M {" L ".join(glyph_pts)} Z" fill="none" stroke="#ffc464" '
    'stroke-width="0.3" stroke-opacity="0.9" stroke-linejoin="round"/>\n'
)


# Sixteen-vertex star polygon: tips at STAR_R, notches at STAR_INNER
def star_polygon(inset=0.0):
    pts = []
    for k in range(16):
        r = STAR_R if k % 2 == 0 else STAR_INNER
        a = math.radians(-90.0 + 22.5 * k)
        pts.append((STAR_CX + r * math.cos(a), STAR_CY + r * math.sin(a)))
    if inset <= 0.0:
        return pts
    # Offset every edge inward by `inset` and mitre the neighbours
    n = len(pts)
    out = []
    for i in range(n):
        p0 = pts[i - 1]
        p1 = pts[i]
        p2 = pts[(i + 1) % n]
        e1 = (p1[0] - p0[0], p1[1] - p0[1])
        e2 = (p2[0] - p1[0], p2[1] - p1[1])
        l1 = math.hypot(e1[0], e1[1])
        l2 = math.hypot(e2[0], e2[1])
        # Vertices run clockwise on screen (y down), so the inward normal
        # of an edge (dx, dy) is (-dy, dx)
        n1 = (-e1[1] / l1, e1[0] / l1)
        n2 = (-e2[1] / l2, e2[0] / l2)
        dot = n1[0] * n2[0] + n1[1] * n2[1]
        scale = inset / (1.0 + dot)
        out.append((p1[0] + (n1[0] + n2[0]) * scale, p1[1] + (n1[1] + n2[1]) * scale))
    return out


def path_of(pts):
    return "M " + " L ".join(f"{f(x)},{f(y)}" for x, y in pts) + " Z"


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

svg.append("\n  <!-- The star of Ishtar: the eight singers stand beyond its points -->\n")
svg.append(f'  <path d="{path_of(star_polygon())}" fill="#140c06" stroke="#ffc464" stroke-width="0.5" stroke-linejoin="miter" stroke-miterlimit="12"/>\n')
for d in STAR_INSETS:
    svg.append(f'  <path d="{path_of(star_polygon(d))}" fill="none" stroke="#cd762b" stroke-width="0.35" stroke-opacity="0.9" stroke-linejoin="miter" stroke-miterlimit="12"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="11.6" fill="#000000" stroke="#ffc464" stroke-width="0.5"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="9.6" fill="none" stroke="#cd762b" stroke-width="2.2"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="7.0" fill="none" stroke="#ffc464" stroke-width="0.4"/>\n')

svg.append("\n  <!-- Controls frame: pitch, swarm, vowel, breath, filter, envelope -->\n")
svg.append(frame(CTRL_X, CTRL_W))
svg.append(f'  <line x1="{f(CTRL_X + 3)}" y1="72" x2="{f(CTRL_X + CTRL_W - 3)}" y2="72" stroke="#382110" stroke-width="0.4"/>\n')
svg.append(f'  <line x1="{f(CTRL_X + 3)}" y1="95.5" x2="{f(CTRL_X + CTRL_W - 3)}" y2="95.5" stroke="#382110" stroke-width="0.4"/>\n')

svg.append("\n  <!-- Patch bay frame: CV inputs, then the stereo outputs -->\n")
svg.append(frame(BAY_X, BAY_W))
svg.append(f'  <line x1="{f(BAY_X + 3)}" y1="97" x2="{f(BAY_X + BAY_W - 3)}" y2="97" stroke="#382110" stroke-width="0.4"/>\n')

svg.append("\n  <!-- Corner brackets (Church card motif) -->\n")
svg.append('  <g fill="none" stroke="#cd762b" stroke-width="0.4" stroke-opacity="0.9">\n')
svg.append(brackets(CTRL_X, CTRL_W))
svg.append(brackets(BAY_X, BAY_W))
svg.append("  </g>\n\n")
svg.append(brand)
svg.append("</svg>\n")

out = ROOT / "res" / "VesperSwarm.svg"
out.write_text("".join(svg), encoding="utf-8")
print(f"wrote {out}")
