#!/usr/bin/env python3
"""Generate res/ConclaveOfHollowRadiance.svg (44 HP).

Phase 3 placeholder: background, the shared header divider and brand mark
(lifted verbatim from res/Syzygy.svg and recentred, so every panel shares one
copy of that artwork), the chamber glyph (the Triangle of Art with its inner
circle), and plain section frames behind the provisional strip layout in
src/ConclaveOfHollowRadiance/ConclaveOfHollowRadiance.cpp. Phase 4 draws the
Circle and Triangle of Solomon here.
Run from anywhere:  python tools/gen_conclave_panel.py
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
SYZYGY = (ROOT / "res" / "Syzygy.svg").read_text(encoding="utf-8")

W, H = 223.52, 128.5
CX = W / 2.0
SYZYGY_CX = 101.6
SHIFT = CX - SYZYGY_CX

TOP, BOTTOM = 18.0, 122.9


def f(x):
    return f"{x:.2f}"


def section(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


# The divider's hairlines carry absolute x coordinates centred on Syzygy's
# panel centre; shift every x1/x2 by the difference in panel centres.
divider = section(SYZYGY, "  <!-- Header divider", "  <!-- Chamber glyph")
divider = re.sub(r'x([12])="([0-9.]+)"', lambda m: f'x{m.group(1)}="{float(m.group(2)) + SHIFT:.2f}"', divider)

brand = section(SYZYGY, "  <!-- Brand mark", "</svg>")
brand = brand.replace(f"translate({SYZYGY_CX} 127.9)", f"translate({CX:.2f} 127.9)")

# Chamber glyph: the Triangle of Art pointing east, its circle inside
gx, gy, gr = CX, 10.8, 1.7
tri = f"M {f(gx - gr * 0.75)},{f(gy - gr)} L {f(gx - gr * 0.75)},{f(gy + gr)} L {f(gx + gr * 1.05)},{f(gy)} Z"
glyph = (
    "  <!-- Chamber glyph: the Triangle of Art, pointing east, its circle within -->\n"
    f'  <path d="{tri}" fill="none" stroke="#ffc464" stroke-width="0.3" stroke-opacity="0.9" stroke-linejoin="round"/>\n'
    f'  <circle cx="{f(gx - gr * 0.15)}" cy="{f(gy)}" r="0.5" fill="none" stroke="#ffc464" stroke-width="0.3" stroke-opacity="0.9"/>\n'
)


def frame(x, w, y0=TOP, y1=BOTTOM, fill="#140c06"):
    return f'  <rect x="{f(x)}" y="{f(y0)}" width="{f(w)}" height="{f(y1 - y0)}" rx="2" fill="{fill}" stroke="#382110" stroke-width="0.4"/>\n'


def brackets(x, w, y0=TOP, y1=BOTTOM):
    return (
        f'    <path d="M {f(x)},{f(y0 + 2.4)} L {f(x)},{f(y0)} L {f(x + 2.4)},{f(y0)}"/>\n'
        f'    <path d="M {f(x + w)},{f(y1 - 2.4)} L {f(x + w)},{f(y1)} L {f(x + w - 2.4)},{f(y1)}"/>\n'
    )


# Frames behind the provisional layout: input bay, channel strips, master
# column, loop/master bay. Geometry mirrors the constants in the .cpp.
FRAMES = [
    (3.0, 39.0),      # IN L / IN R / CV
    (45.0, 100.0),    # LEVEL SEND FEED TUNE, OUT L / OUT R
    (148.0, 36.0),    # RETURN, POST/LOOP, MATTER, MASTER
    (187.0, 33.5),    # SEND, RET, OUT
]

svg = []
svg.append('<?xml version="1.0" encoding="UTF-8"?>\n')
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">\n')
svg.append("  <!-- Panel background -->\n")
svg.append(f'  <rect x="0" y="0" width="{W}" height="{H}" fill="#000000"/>\n\n')
svg.append(divider)
svg.append(glyph)
svg.append("\n  <!-- Section frames (provisional layout) -->\n")
for x, w in FRAMES:
    svg.append(frame(x, w))
svg.append("\n  <!-- Corner brackets (Church card motif) -->\n")
svg.append('  <g fill="none" stroke="#cd762b" stroke-width="0.4" stroke-opacity="0.9">\n')
for x, w in FRAMES:
    svg.append(brackets(x, w))
svg.append("  </g>\n\n")
svg.append(brand)
svg.append("</svg>\n")

out = ROOT / "res" / "ConclaveOfHollowRadiance.svg"
out.write_text("".join(svg), encoding="utf-8", newline="\n")
print(f"wrote {out} ({W} x {H} mm)")
