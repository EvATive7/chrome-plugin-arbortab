"""Generates the ArborTab logo ("sprout tab").  uv run python assets/logo/make.py

Writes logo.svg (48 px and up) and logo-small.svg (16/32 px).

Geometry on a 128 grid. The tab is a Chrome tab silhouette (rounded top, flared foot) sitting
on a strip line; it is deliberately low and wide so it reads as a tab, not a pot. The sprout is
the hero: a slightly leaning stem with two asymmetric leaves, optically centred in the tile.
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Tab: 48 wide, 16 high, flared foot, rounded top. Kept small: it is a base, not a pot.
TAB = ("M33 100 C37 100 40 98 40 95 V91 C40 86.6 43.6 84 48 84 H80 C84.4 84 88 86.6 88 91 "
       "V95 C88 98 91 100 95 100 Z")
TAB_SMALL = ("M31 102 C35.5 102 38 99.5 38 96 V90 C38 85 42 82 47 82 H81 C86 82 90 85 90 90 "
             "V96 C90 99.5 92.5 102 97 102 Z")
STRIP = "M14 100 H114"
# Drawn over the leaf bases and ends where they meet, so both leaves grow out of its tip.
STEM = "M64 85 C64 74 64.3 67 64.9 62"
# Left leaf low and broad, right leaf higher and longer: gives the sprout a lean to the right.
LEAF_L = "M64.5 66 C59 52 46 44.5 30 46 C31.5 60 45 69.5 64.5 66 Z"
LEAF_R = "M62.4 59.6 C66.5 39 82.5 28.5 101 28 C100.5 46 86.5 59.5 67.8 60.4 Z"
RIB_L = "M62 64 C54 57 46 52.5 37 50"
RIB_R = "M68 54.5 C75 46 84 39 94 34"
# Small sizes: fuller, more symmetric leaves; the full-size pair reads as a check mark at 16 px.
LEAF_L_SMALL = "M64 66 C57 46 41 36 21 37 C22 58 40.5 71.5 64 66 Z"
LEAF_R_SMALL = "M62 60 C64.5 38 82 25 105 25 C105 47 89 61.5 68 61.5 Z"

VARIANTS = {
    # name: background stops, tab, strip, stem, left leaf, right leaf, rib
    "light": (("#f3fbf6", "#dff2e6"), "#1e9a5a", "#1e9a5a", "#1e9a5a", "#3dbb74", "#7fd9a6", "#ffffff"),
}


def svg(name, small=False):
    (bg0, bg1), tab, strip, stem, leaf_l, leaf_r, rib = VARIANTS[name]
    # Small sizes: no ribs, no strip line, thicker stem, a slightly larger tab.
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">',
        f'  <defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
        f'<stop offset="0" stop-color="{bg0}"/><stop offset="1" stop-color="{bg1}"/></linearGradient></defs>',
        '  <rect width="128" height="128" rx="28" fill="url(#bg)"/>',
    ]
    if name == "light":
        parts.append('  <rect x="0.75" y="0.75" width="126.5" height="126.5" rx="27.25" fill="none" stroke="#1e9a5a" '
                     'stroke-opacity=".2" stroke-width="1.5"/>')
    if not small:
        parts.append(f'  <path d="{STRIP}" stroke="{strip}" stroke-opacity=".35" stroke-width="3" stroke-linecap="round"/>')
    # Leaves first, stem on top: the leaf bases tuck in behind the stem.
    parts += [
        f'  <path d="{LEAF_L_SMALL if small else LEAF_L}" fill="{leaf_l}"/>',
        f'  <path d="{LEAF_R_SMALL if small else LEAF_R}" fill="{leaf_r}"/>',
    ]
    if not small:
        parts += [
            f'  <path d="{RIB_L}" fill="none" stroke="{rib}" stroke-opacity=".45" stroke-width="2" stroke-linecap="round"/>',
            f'  <path d="{RIB_R}" fill="none" stroke="{rib}" stroke-opacity=".45" stroke-width="2" stroke-linecap="round"/>',
        ]
    stem_width = 8 if small else 5.5
    parts.append(f'  <path d="{STEM}" fill="none" stroke="{stem}" stroke-width="{stem_width}" stroke-linecap="round"/>')
    parts += [f'  <path d="{TAB_SMALL if small else TAB}" fill="{tab}"/>', "</svg>", ""]
    return "\n".join(parts)


(HERE / "logo.svg").write_text(svg("light"), encoding="utf-8")
# The settings page header uses the same mark.
(HERE.parents[1] / "extension" / "icons" / "logo.svg").write_text(svg("light"), encoding="utf-8")
(HERE / "logo-small.svg").write_text(svg("light", small=True), encoding="utf-8")
print("ok")
