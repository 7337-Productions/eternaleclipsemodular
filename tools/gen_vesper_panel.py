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

# The star of Ishtar: two four-pointed stars, one laid over the other at
# 45 degrees, each with nested bands following its edges, and a bullseye at
# the heart. The singers' buttons sit just beyond the tips (see
# VesperSwarm.cpp) so the points stay sharp.
STAR_CX, STAR_CY, STAR_R = 54.0, 69.25, 40.0
STAR_INNER = 0.42 * STAR_R      # radius of the notches of the eight-point silhouette
STAR_INSETS = (1.4, 2.8, 4.2)   # mm, the nested bands of each four-pointed star
RING_IN, RING_OUT = 41.0, 51.0  # the singers' ring: buttons at 46 sit between


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


# Notch radius of one four-pointed star such that its edges cross the other
# star's edges at STAR_INNER, keeping the eight-point silhouette
def four_star_notch():
    d = (math.cos(math.radians(22.5)), math.sin(math.radians(22.5)))
    P = (STAR_R, 0.0)
    lo, hi = 0.0, STAR_R
    for _ in range(60):
        r4 = 0.5 * (lo + hi)
        Q = (r4 * math.cos(math.radians(45.0)), r4 * math.sin(math.radians(45.0)))
        E = (Q[0] - P[0], Q[1] - P[1])
        u = -(P[0] * d[1] - P[1] * d[0]) / (E[0] * d[1] - E[1] * d[0])
        X = (P[0] + u * E[0], P[1] + u * E[1])
        # A deeper notch pulls the crossing inward
        if math.hypot(X[0], X[1]) > STAR_INNER:
            hi = r4
        else:
            lo = r4
    return 0.5 * (lo + hi)


# Eight-vertex four-pointed star: tips at STAR_R every 90 degrees from
# `rot`, notches between them. Inset bands follow the edges inward.
def star_polygon(rot=-90.0, inset=0.0):
    r4 = four_star_notch()
    pts = []
    for k in range(8):
        r = STAR_R if k % 2 == 0 else r4
        a = math.radians(rot + 45.0 * k)
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

svg.append("\n  <!-- The star of Ishtar: the eight singers stand beyond its points.\n"
           "       The diagonal four-pointed star lies under the upright one. -->\n")
for rot in (-45.0, -90.0):
    svg.append(f'  <path d="{path_of(star_polygon(rot))}" fill="#140c06" stroke="#ffc464" stroke-width="0.5" stroke-linejoin="miter" stroke-miterlimit="16"/>\n')
    for d in STAR_INSETS:
        svg.append(f'  <path d="{path_of(star_polygon(rot, d))}" fill="none" stroke="#ffc464" stroke-width="0.35" stroke-linejoin="miter" stroke-miterlimit="16"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="11.6" fill="#000000" stroke="#ffc464" stroke-width="0.5"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="7.0" fill="none" stroke="#ffc464" stroke-width="0.4"/>\n')
svg.append("  <!-- The singers' ring, just outside the points -->\n")
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="{f(RING_IN)}" fill="none" stroke="#ffc464" stroke-width="0.4"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="{f(RING_OUT)}" fill="none" stroke="#ffc464" stroke-width="0.4"/>\n')

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
