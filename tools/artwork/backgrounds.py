#!/usr/bin/env python3
"""Generates the fldesktop wallpapers into data/backgrounds/.

Only gradients, paths and opacity are used so that FLTK's SVG renderer
(nanosvg) draws them like any other renderer does. The output is fully
deterministic (seeded random numbers).
"""
import math
import os
import random
import sys

W, H = 1920, 1080


def svg(defs, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">\n'
            f'  <defs>\n{defs}  </defs>\n{body}</svg>\n')


def lin(gid, stops, x1=0, y1=0, x2=0, y2=1):
    out = ""
    for st in stops:
        o, c = st[0], st[1]
        a = f' stop-opacity="{st[2]:g}"' if len(st) > 2 else ""
        out += f'<stop offset="{o:g}" stop-color="{c}"{a}/>'
    # percentages: nanosvg (FLTK) reads plain numbers as user units
    return (f'    <linearGradient id="{gid}" x1="{x1 * 100:g}%" y1="{y1 * 100:g}%" x2="{x2 * 100:g}%" '
            f'y2="{y2 * 100:g}%">{out}</linearGradient>\n')


def rad(gid, stops, cx=0.5, cy=0.5, r=0.5):
    s = "".join(f'<stop offset="{o:g}" stop-color="{c}" stop-opacity="{a:g}"/>' for o, c, a in stops)
    return f'    <radialGradient id="{gid}" cx="{cx * 100:g}%" cy="{cy * 100:g}%" r="{r * 100:g}%">{s}</radialGradient>\n'


def rect(fill, opacity=None):
    o = f' fill-opacity="{opacity}"' if opacity is not None else ""
    return f'  <rect width="{W}" height="{H}" fill="{fill}"{o}/>\n'


def path(d, fill, opacity=None):
    o = f' fill-opacity="{opacity:g}"' if opacity is not None else ""
    return f'  <path d="{d}" fill="{fill}"{o}/>\n'


def wave(y, amp, phase, freq, segs=8):
    """A smooth wavy band from y down to the bottom edge."""
    pts = []
    for i in range(segs + 1):
        x = W * i / segs
        pts.append((x, y + amp * math.sin(phase + freq * i / segs * 2 * math.pi)))
    d = f"M0 {pts[0][1]:.0f}"
    for i in range(1, len(pts)):
        x0, y0 = pts[i - 1]
        x1, y1 = pts[i]
        cx = (x0 + x1) / 2
        d += f" C {cx:.0f} {y0:.0f} {cx:.0f} {y1:.0f} {x1:.0f} {y1:.0f}"
    return d + f" L{W} {H} L0 {H} Z"


def hills(dark):
    sky = [(0, "#0b1430"), (0.5, "#33295a"), (0.82, "#b0566a"), (1, "#f2a46c")] if dark else \
          [(0, "#5a8fd0"), (0.55, "#9cb8e0"), (0.85, "#f3c6a5"), (1, "#ffdca8")]
    defs = lin("sky", sky) + rad("sun", [(0, "#ffe7b0", 0.95), (0.25, "#ffd08a", 0.5), (1, "#ffc070", 0)], 0.68, 0.62, 0.35)
    body = rect("url(#sky)") + rect("url(#sun)")
    layers = ["#3b2c5c", "#2b2148", "#1c1734", "#120f22"] if dark else ["#7c93c4", "#5a73a8", "#3f5788", "#2a3d66"]
    rnd = random.Random(7)
    for i, c in enumerate(layers):
        body += path(wave(640 + i * 105, 40 + 15 * i, rnd.uniform(0, 6), 1.2 + 0.4 * i), c)
    return svg(defs, body)


def waves(dark):
    base = [(0, "#062a3a"), (1, "#0d4d5c")] if dark else [(0, "#d7f0f2"), (1, "#8fd3d6")]
    defs = lin("bg", base, 0, 0, 1, 1)
    body = rect("url(#bg)")
    cols = ["#0f6b7a", "#138494", "#1b9aa8", "#2bb3b5", "#5ccbc0"] if dark else \
           ["#5ec2c6", "#3aa9b4", "#24909f", "#167687", "#0d5d6f"]
    for i, c in enumerate(cols):
        body += path(wave(420 + i * 120, 60, i * 0.9, 1.5), c, 0.55 + 0.09 * i)
    return svg(defs, body)


def lowpoly(dark):
    rnd = random.Random(42)
    cols = 16
    rows = 9
    pts = [[(W * x / cols + (rnd.uniform(-45, 45) if 0 < x < cols else 0),
             H * y / rows + (rnd.uniform(-45, 45) if 0 < y < rows else 0)) for x in range(cols + 1)] for y in range(rows + 1)]
    a = (0x14, 0x1e, 0x3a) if dark else (0x3a, 0x7b, 0xd5)
    b = (0x6a, 0x2a, 0x6e) if dark else (0x9b, 0xd8, 0xe6)
    body = ""
    for y in range(rows):
        for x in range(cols):
            p00, p10, p01, p11 = pts[y][x], pts[y][x + 1], pts[y + 1][x], pts[y + 1][x + 1]
            for tri in ((p00, p10, p11), (p00, p11, p01)):
                cx = sum(p[0] for p in tri) / 3 / W
                cy = sum(p[1] for p in tri) / 3 / H
                t = max(0, min(1, 0.6 * cx + 0.4 * cy + rnd.uniform(-0.06, 0.06)))
                light = rnd.uniform(-10, 10)
                col = "#%02x%02x%02x" % tuple(max(0, min(255, int(a[i] + (b[i] - a[i]) * t + light))) for i in range(3))
                d = "M" + " L".join(f"{p[0]:.0f} {p[1]:.0f}" for p in tri) + " Z"
                body += f'  <path d="{d}" fill="{col}" stroke="{col}" stroke-width="1"/>\n'
    return svg("", body)


def aurora(dark):
    defs = lin("bg", [(0, "#020814"), (0.6, "#071a2c"), (1, "#0c2a3a")] if dark else
               [(0, "#1a2f55"), (0.6, "#2d5480"), (1, "#5f8fb0")])
    bands = [("#3cf0a0", 0.35, 300), ("#26c6da", 0.3, 380), ("#9b6cf0", 0.22, 250), ("#3cf0a0", 0.18, 460)]
    for i, (c, a, _) in enumerate(bands):
        defs += lin(f"a{i}", [(0, c, 0), (0.5, c, a), (1, c, 0)])
    body = rect("url(#bg)")
    rnd = random.Random(3)
    for i, (c, a, y) in enumerate(bands):
        top = []
        for k in range(9):
            top.append((W * k / 8, y + 120 * math.sin(k * 0.9 + i) + rnd.uniform(-30, 30)))
        d = f"M{top[0][0]:.0f} {top[0][1]:.0f}"
        for k in range(1, len(top)):
            (x0, y0), (x1, y1) = top[k - 1], top[k]
            d += f" C {(x0 + x1) / 2:.0f} {y0:.0f} {(x0 + x1) / 2:.0f} {y1:.0f} {x1:.0f} {y1:.0f}"
        d += f" L{W} {top[-1][1] + 260:.0f}"
        for k in range(len(top) - 1, 0, -1):
            (x0, y0), (x1, y1) = top[k], top[k - 1]
            d += f" C {(x0 + x1) / 2:.0f} {y0 + 260:.0f} {(x0 + x1) / 2:.0f} {y1 + 260:.0f} {x1:.0f} {y1 + 260:.0f}"
        d += " Z"
        body += f'  <path d="{d}" fill="url(#a{i})"/>\n'
    # stars and a dark ridge
    for _ in range(140):
        x, y, r = rnd.uniform(0, W), rnd.uniform(0, H * 0.7), rnd.uniform(0.6, 1.8)
        body += f'  <circle cx="{x:.0f}" cy="{y:.0f}" r="{r:.1f}" fill="#ffffff" fill-opacity="{rnd.uniform(0.3, 0.9):.2f}"/>\n'
    body += path(wave(900, 50, 1.3, 2.2, 12), "#01060d" if dark else "#0e1d33")
    return svg(defs, body)


def minimal(dark):
    if dark:
        defs = lin("bg", [(0, "#1e2126"), (1, "#121417")], 0, 0, 1, 1) + \
            rad("g", [(0, "#3584e4", 0.28), (1, "#3584e4", 0)], 0.2, 0.85, 0.6)
        ring = "#ffffff"
    else:
        defs = lin("bg", [(0, "#f4f5f7"), (1, "#dfe3e9")], 0, 0, 1, 1) + \
            rad("g", [(0, "#3584e4", 0.22), (1, "#3584e4", 0)], 0.85, 0.15, 0.6)
        ring = "#1e2126"
    body = rect("url(#bg)") + rect("url(#g)")
    for i, r in enumerate((180, 300, 420, 540, 660)):
        body += (f'  <circle cx="{W * 0.72:.0f}" cy="{H * 0.55:.0f}" r="{r}" fill="none" stroke="{ring}" '
                 f'stroke-opacity="{0.08 - i * 0.012:.3f}" stroke-width="{26 - i * 4}"/>\n')
    return svg(defs, body)


def blobs(dark):
    defs = lin("bg", [(0, "#14162b"), (1, "#24143a")] if dark else [(0, "#fff4ea"), (1, "#eaf1ff")], 0, 0, 1, 1)
    spots = [("#3584e4", 0.30, 0.30, 0.45), ("#ed5b00", 0.75, 0.70, 0.45), ("#9141ac", 0.80, 0.20, 0.35),
             ("#2190a4", 0.20, 0.85, 0.35)]
    for i, (c, x, y, r) in enumerate(spots):
        defs += rad(f"s{i}", [(0, c, 0.75 if dark else 0.55), (1, c, 0)], x, y, r)
    body = rect("url(#bg)") + "".join(rect(f"url(#s{i})") for i in range(len(spots)))
    return svg(defs, body)


def stripes(dark):
    rnd = random.Random(11)
    palette = ["#3584e4", "#2190a4", "#3a944a", "#e5a50a", "#ed5b00", "#e62d42", "#9141ac"]
    defs = lin("bg", [(0, "#16181d"), (1, "#22262e")] if dark else [(0, "#f6f6f4"), (1, "#e9e9e5")])
    body = rect("url(#bg)")
    x = -400
    i = 0
    while x < W + 400:
        w = rnd.choice((60, 90, 140))
        c = palette[i % len(palette)]
        body += path(f"M{x} {H} L{x + 600} 0 L{x + 600 + w} 0 L{x + w} {H} Z", c, 0.7 if dark else 0.6)
        x += w + rnd.choice((90, 160, 260))
        i += 1
    # (no translucent gradient overlay: nanosvg ignores the opacity of gradient fills)
    return svg(defs, body)


WALLPAPERS = {
    "dusk-hills": hills, "waves": waves, "low-poly": lowpoly, "aurora": aurora, "rings": minimal,
    "color-blobs": blobs, "stripes": stripes,
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "../../data/backgrounds")
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    n = 0
    for name, fn in WALLPAPERS.items():
        for dark in (False, True):
            p = os.path.join(out, f"{name}{'-dark' if dark else ''}.svg")
            with open(p, "w") as f:
                f.write(fn(dark))
            n += 1
    print(f"{n} wallpapers written to {out}")


if __name__ == "__main__":
    main()
