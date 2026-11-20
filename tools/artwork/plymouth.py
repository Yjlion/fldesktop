#!/usr/bin/env python3
"""Generates the images of the fldesktop Plymouth theme into data/plymouth/fldesktop/.

logo.png is the fldesktop logo (data/icons/hicolor/scalable/apps/fldesktop.svg)
at 256 px; the script scales it to the screen. spinner-0..11.png are the frames
of a ring of dots whose bright dot goes round; entry.png and bullet.png draw the
password field for encrypted disks. Needs rsvg-convert; deterministic output.
"""
import math
import os
import subprocess
import sys
import tempfile

ACCENT = "#3584e4"
HIGHLIGHT = "#7fb2e5"
FIELD = "#101622"  # the greeter's login card
FRAMES = 12


def spinner(frame, size=64):
    c = size / 2
    r = size * 0.36
    dots = []
    for i in range(FRAMES):
        a = 2 * math.pi * i / FRAMES - math.pi / 2
        # the lead dot is brightest, the ones behind it fade out
        age = (frame - i) % FRAMES
        op = max(0.18, 1.0 - age * 0.14)
        rad = size * (0.075 if age == 0 else 0.06)
        color = "#ffffff" if age == 0 else HIGHLIGHT
        dots.append(f'  <circle cx="{c + r * math.cos(a):.2f}" cy="{c + r * math.sin(a):.2f}" r="{rad:.2f}" '
                    f'fill="{color}" fill-opacity="{op:.2f}"/>\n')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
            f'viewBox="0 0 {size} {size}">\n{"".join(dots)}</svg>\n')


def entry(w=320, h=44):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">\n'
            f'  <rect x="1" y="1" width="{w - 2}" height="{h - 2}" rx="10" fill="{FIELD}" fill-opacity="0.92" '
            f'stroke="{ACCENT}" stroke-width="2"/>\n</svg>\n')


def bullet(size=12):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
            f'viewBox="0 0 {size} {size}">\n  <circle cx="{size / 2}" cy="{size / 2}" r="{size / 2 - 1}" '
            f'fill="#ffffff"/>\n</svg>\n')


def render(svg_text, png, size=None):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(svg_text)
        name = f.name
    try:
        args = ["rsvg-convert", "-o", png, name]
        if size:
            args[1:1] = ["-w", str(size), "-h", str(size)]
        subprocess.run(args, check=True)
    finally:
        os.unlink(name)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    top = os.path.abspath(os.path.join(here, "..", ".."))
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(top, "data", "plymouth", "fldesktop"))
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(top, "data", "icons", "hicolor", "scalable", "apps", "fldesktop.svg")) as f:
        render(f.read(), os.path.join(out, "logo.png"), 256)
    for i in range(FRAMES):
        render(spinner(i), os.path.join(out, f"spinner-{i}.png"))
    render(entry(), os.path.join(out, "entry.png"))
    render(bullet(), os.path.join(out, "bullet.png"))


if __name__ == "__main__":
    main()
