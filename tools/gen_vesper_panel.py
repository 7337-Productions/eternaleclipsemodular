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

# Frames, in the Coronal Annihilator's proportions: a 40 mm controls column
# in two boxes and a 38 mm bay, both from y 18 to the screw line
TOP, BOTTOM = 18.0, 122.9
CTRL_X, CTRL_W = 118.0, 40.0
CTRL_SPLIT = 68.0               # top box ends here, the lower starts at +3
BAY_X, BAY_W = 162.2, 38.0

# The star of Ishtar: two four-pointed stars, one laid over the other at
# 45 degrees, each with nested bands following its edges, and a bullseye at
# the heart. The singers' buttons sit between the points (see
# VesperSwarm.cpp) so the points stay sharp.
STAR_CX, STAR_CY, STAR_R = 59.0, 64.0, 42.5
STAR_INNER = 0.42 * STAR_R      # radius of the notches of the eight-point silhouette
STAR_INSETS = (1.4, 2.8, 4.2)   # mm, the nested bands of each four-pointed star
STAR_EDGE_W, STAR_BAND_W = 0.35, 0.22  # mm; much under 0.25 fades at 100% zoom

# The crescent under the star: a thin moon cradling it, centred on the
# star's axis. Its inner edge is the circle through the star's tips, so
# every point but the top one touches it, and its horns end exactly on the
# upper diagonal points; the outer edge is a disc dropped below the star's
# centre, sized for MOON_THICK at the bottom. The eight phases are carved
# along it midway between the touching points.
MOON_IN_R = STAR_R + 0.3        # the strokes kiss rather than overlap
MOON_THICK = 12.0               # mm, at the bottom; ~4.6 at the side points
MOON_HORN_RISE = STAR_R * math.sin(math.radians(45.0))  # horns at the NW/NE tips
PHASE_R = 1.8                   # radius of each carved phase
# Angles (SVG, y down) from the star's centre, midway between the tips at
# 0, 45, 90, 135 and 180
PHASE_ANGLES = [11.25 + 22.5 * k for k in range(8)]


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


# The crescent between the moon's disc and the circle through the star's
# tips: outer arc the long way round from horn to horn under the star,
# then back along the inner circle.
# The moon disc: its centre sits `drop` below the star's so that the horns
# meet the inner circle MOON_HORN_RISE above it and the band is MOON_THICK
# at the bottom. With t the thickness, ri the inner radius and h the rise,
# (t + ri - d)^2 = (ri^2 - h^2) + (h + d)^2 gives d = (t^2 + 2 t ri) / (2 (t + ri + h)).
def moon_disc():
    t, ri, h = MOON_THICK, MOON_IN_R, MOON_HORN_RISE
    drop = (t * t + 2.0 * t * ri) / (2.0 * (t + ri + h))
    return drop, t + ri - drop


# Rack's nanosvg mis-fills the two-arc crescent (the whole disc came out
# solid), so it is emitted as a polygon: the outer edge sampled from horn
# to horn under the star, then the inner edge back.
def crescent_path():
    ri = MOON_IN_R
    drop, ro = moon_disc()
    h = MOON_HORN_RISE
    hx = math.sqrt(ri * ri - h * h)
    ox, oy = STAR_CX, STAR_CY + drop
    n = 120
    pts = []
    # Outer edge: from the left horn, anticlockwise on screen (angles
    # decreasing) round the bottom to the right horn
    a0 = math.atan2(-h - drop, -hx)
    a1 = math.atan2(-h - drop, hx) - 2.0 * math.pi
    for k in range(n + 1):
        a = a0 + (a1 - a0) * k / n
        pts.append((ox + ro * math.cos(a), oy + ro * math.sin(a)))
    # Inner edge: from the right horn, clockwise on screen round the bottom
    # back to the left horn
    b0 = math.atan2(-h, hx)
    b1 = math.atan2(-h, -hx) + 2.0 * math.pi
    for k in range(n + 1):
        b = b0 + (b1 - b0) * k / n
        pts.append((STAR_CX + ri * math.cos(b), STAR_CY + ri * math.sin(b)))
    return path_of(pts)


# One lunar phase glyph, p in [0, 1): 0 new, 0.25 first quarter, 0.5 full.
# The lit part is the limb on the sunward side bounded by the terminator,
# an ellipse whose x radius is r cos(2 pi p).
def phase_glyph(cx, cy, r, p):
    out = f'    <circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}"/>\n'
    if abs(p - 0.5) < 1e-6:
        out += f'    <circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}" fill="#ffc464"/>\n'
    elif p > 1e-6:
        waxing = p < 0.5
        a = r * math.cos(2.0 * math.pi * p)   # terminator half-axis, signed
        limb = 1 if waxing else 0             # sweep of the lit limb: right or left
        bow = (0 if a > 0 else 1) if waxing else (1 if a > 0 else 0)
        if abs(a) < 0.05:                     # a quarter: the terminator is straight
            back = f"L {f(cx)},{f(cy - r)}"
        else:
            back = f"A {f(abs(a))} {f(r)} 0 0 {bow} {f(cx)},{f(cy - r)}"
        out += (f'    <path d="M {f(cx)},{f(cy - r)} A {f(r)} {f(r)} 0 0 {limb} {f(cx)},{f(cy + r)} '
                f'{back} Z" fill="#ffc464"/>\n')
    return out


# The phases sit midway through the crescent's thickness along each angle
def moon_phases():
    ri = MOON_IN_R
    d, ro = moon_disc()
    out = ['  <g fill="none" stroke="#ffc464" stroke-width="0.25">\n']
    for k in range(len(PHASE_ANGLES)):
        a = math.radians(PHASE_ANGLES[k])
        phi = a - 0.5 * math.pi             # from straight down, where the disc's centre lies
        outer = d * math.cos(phi) + math.sqrt(ro * ro - (d * math.sin(phi)) ** 2)
        rmid = 0.5 * (ri + outer)
        out.append(phase_glyph(STAR_CX + rmid * math.cos(a), STAR_CY + rmid * math.sin(a), PHASE_R, k / 8.0))
    out.append("  </g>\n")
    return "".join(out)


def frame(x, w, y0=TOP, y1=BOTTOM):
    return f'  <rect x="{f(x)}" y="{f(y0)}" width="{f(w)}" height="{f(y1 - y0)}" rx="2" fill="#140c06" stroke="#382110" stroke-width="0.4"/>\n'


def brackets(x, w, y0=TOP, y1=BOTTOM):
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

svg.append("\n  <!-- The crescent moon the star rests in, its points touching the inner\n"
           "       edge, the eight phases carved along it -->\n")
svg.append(f'  <path d="{crescent_path()}" fill="#140c06" stroke="#ffc464" stroke-width="{STAR_EDGE_W}" stroke-linejoin="round"/>\n')
svg.append(moon_phases())

svg.append("\n  <!-- The star of Ishtar: the eight singers stand between its points.\n"
           "       The diagonal four-pointed star lies under the upright one. -->\n")
for rot in (-45.0, -90.0):
    svg.append(f'  <path d="{path_of(star_polygon(rot))}" fill="#140c06" stroke="#ffc464" stroke-width="{STAR_EDGE_W}" stroke-linejoin="miter" stroke-miterlimit="16"/>\n')
    for d in STAR_INSETS:
        svg.append(f'  <path d="{path_of(star_polygon(rot, d))}" fill="none" stroke="#ffc464" stroke-width="{STAR_BAND_W}" stroke-linejoin="miter" stroke-miterlimit="16"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="11.6" fill="#000000" stroke="#ffc464" stroke-width="0.5"/>\n')
svg.append(f'  <circle cx="{f(STAR_CX)}" cy="{f(STAR_CY)}" r="7.0" fill="none" stroke="#ffc464" stroke-width="0.4"/>\n')

svg.append("\n  <!-- Controls: voice box (pitch, glide, swarm, vowel) over filter + envelope box -->\n")
svg.append(frame(CTRL_X, CTRL_W, TOP, CTRL_SPLIT))
svg.append(frame(CTRL_X, CTRL_W, CTRL_SPLIT + 3.0, BOTTOM))
svg.append(f'  <line x1="{f(CTRL_X + 3)}" y1="93" x2="{f(CTRL_X + CTRL_W - 3)}" y2="93" stroke="#382110" stroke-width="0.4"/>\n')

svg.append("\n  <!-- Patch bay frame: CV inputs, then the stereo outputs -->\n")
svg.append(frame(BAY_X, BAY_W))
svg.append(f'  <line x1="{f(BAY_X + 3)}" y1="97" x2="{f(BAY_X + BAY_W - 3)}" y2="97" stroke="#382110" stroke-width="0.4"/>\n')

svg.append("\n  <!-- Corner brackets (Church card motif) -->\n")
svg.append('  <g fill="none" stroke="#cd762b" stroke-width="0.4" stroke-opacity="0.9">\n')
svg.append(brackets(CTRL_X, CTRL_W, TOP, CTRL_SPLIT))
svg.append(brackets(CTRL_X, CTRL_W, CTRL_SPLIT + 3.0, BOTTOM))
svg.append(brackets(BAY_X, BAY_W))
svg.append("  </g>\n\n")
svg.append(brand)
svg.append("</svg>\n")

out = ROOT / "res" / "VesperSwarm.svg"
out.write_text("".join(svg), encoding="utf-8")
print(f"wrote {out}")
