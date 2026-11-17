#!/usr/bin/env python3
"""Generates the fldesktop cursor themes into data/icons/:

  fldesktop-cursors        dark cursors with a white outline
  fldesktop-cursors-light  white cursors with a dark outline

Each cursor is drawn on a 32x32 grid, rendered with rsvg-convert at every
nominal size and packed into Xcursor files by xcursor.py. Alternative names
(CSS / cursor-shape-v1 names and the old X11 ones) are symlinks.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xcursor import write_xcursor  # noqa: E402

SIZES = [24, 32, 48, 64, 96]
OUTLINE_W = 1.6  # outline width on the 32 grid

ARROW = "M5 3 L5 25.5 L10.5 20.5 L14 28.5 L17.6 27 L14.2 19 L21.5 19 Z"
SMALL_ARROW = "M4 2.5 L4 20 L8.3 16.1 L11 22.3 L13.8 21.1 L11.2 15 L16.9 15 Z"


# A shape is (kind, data). kind: "fill" path, "stroke" (path, width),
# "circle" (cx, cy, r), "ring" (cx, cy, r, width), "detail" (path, width) -
# detail strokes are drawn on top in the outline color.
def fill(d):
    return ("fill", d)


def stroke(d, w):
    return ("stroke", (d, w))


def circle(cx, cy, r):
    return ("circle", (cx, cy, r))


def ring(cx, cy, r, w):
    return ("ring", (cx, cy, r, w))


def detail(d, w=1.6):
    return ("detail", (d, w))


def group(shapes, transform):
    return ("group", (shapes, transform))


def head(x, y, angle):
    """An arrow head with its tip at (x, y) pointing at `angle` degrees (0 = up)."""
    return group([fill("M0 0 L5.5 6.5 L-5.5 6.5 Z")], f"translate({x} {y}) rotate({angle})")


def double_arrow(angle, length=20):
    h = length / 2
    return [group([stroke(f"M16 {16 - h + 4} V{16 + h - 4}", 3), head(16, 16 - h, 0), head(16, 16 + h, 180)],
                  f"rotate({angle} 16 16)")]


def single_arrow(angle):
    # an arrow pointing to an edge or a corner, with a bar at the edge
    return [group([stroke("M16 9.5 V25", 3), head(16, 6.5, 0), stroke("M9 4 H23", 2.6)], f"rotate({angle} 16 16)")]


def badge(inner):
    return [circle(23.5, 23.5, 6.5)] + inner


HAND_POINT = [
    fill("M11 4.5 a2.5 2.5 0 0 1 5 0 V14 h0.5 a2.3 2.3 0 0 1 4.3 0.6 a2.3 2.3 0 0 1 4.1 1 a2.3 2.3 0 0 1 3.6 1.9 "
         "V22 c0 4 -2.5 7.5 -7 7.5 h-3 c-2.7 0 -4.2 -1.2 -5.7 -3.4 L6.2 20 a2.3 2.3 0 0 1 3.8 -2.6 L11 19 Z"),
    detail("M16 14.5 V19 M20.3 15.2 V19.5 M24.4 16.5 V20", 1.1),
]
HAND_OPEN = [
    fill("M10 9 a2.2 2.2 0 0 1 4.4 0 V15 V6.5 a2.2 2.2 0 0 1 4.4 0 V15 V8 a2.2 2.2 0 0 1 4.4 0 V16 V11.5 "
         "a2.2 2.2 0 0 1 4.4 0 V21 c0 4.5 -3 8.5 -8 8.5 h-2 c-3 0 -4.8 -1.3 -6.5 -3.7 L4.8 19.8 "
         "a2.2 2.2 0 0 1 3.6 -2.5 L10 19 Z"),
]
HAND_CLOSED = [
    fill("M8 15.5 a2.2 2.2 0 0 1 4.4 -0.5 a2.2 2.2 0 0 1 4.4 -0.5 a2.2 2.2 0 0 1 4.4 0 a2.2 2.2 0 0 1 4.4 0.8 "
         "V22 c0 4.5 -3 7.5 -8 7.5 h-2 c-3 0 -4.8 -1.3 -6.5 -3.7 L5.6 22.2 a2.2 2.2 0 0 1 2.4 -3.2 Z"),
    detail("M12.4 15 V18.5 M16.8 14.5 V18.5 M21.2 14.5 V18.5", 1.1),
]
IBEAM = [stroke("M12 5 h3 a1 1 0 0 1 1 1 a1 1 0 0 1 1 -1 h3 M16 6 V26 M12 27 h3 a1 1 0 0 0 1 -1 a1 1 0 0 0 1 1 h3 M13 16 h6", 2.2)]


def spinner_ring(cx, cy, r, w, angle, inner_detail=True):
    """A ring with a bright segment at `angle` (degrees)."""
    out = [ring(cx, cy, r, w)]
    seg = group([("arc", (cx, cy, r, w))], f"rotate({angle} {cx} {cy})")
    out.append(seg)
    return out


def spin_frames(base, cx, cy, r, w, n=8):
    return [base + spinner_ring(cx, cy, r, w, i * 360 / n) for i in range(n)]


# name -> (frames (list of shape lists), hotspot (x, y) on the 32 grid, frame delay ms)
CURSORS = {
    "default": ([[fill(ARROW)]], (5, 3), 0),
    "pointer": ([HAND_POINT], (13.5, 2.5), 0),
    "grab": ([HAND_OPEN], (16, 12), 0),
    "grabbing": ([HAND_CLOSED], (16, 16), 0),
    "text": ([IBEAM], (16, 16), 0),
    "vertical-text": ([[group(IBEAM, "rotate(90 16 16)")]], (16, 16), 0),
    "crosshair": ([[stroke("M16 3 V12.5 M16 19.5 V29 M3 16 H12.5 M19.5 16 H29", 2.4), circle(16, 16, 1.6)]], (16, 16), 0),
    "cell": ([[stroke("M16 5 V27 M5 16 H27", 6)]], (16, 16), 0),
    "move": ([[stroke("M16 7 V25 M7 16 H25", 2.6), head(16, 3, 0), head(16, 29, 180), head(3, 16, 270), head(29, 16, 90)]],
             (16, 16), 0),
    "not-allowed": ([[ring(16, 16, 10.5, 3.6), stroke("M8.6 8.6 L23.4 23.4", 3.6)]], (16, 16), 0),
    "wait": (spin_frames([], 16, 16, 10, 4.2), (16, 16), 80),
    "progress": (spin_frames([fill(SMALL_ARROW)], 23, 23, 5.5, 3), (4, 2.5), 80),
    "help": ([[fill(SMALL_ARROW)] + badge([detail("M21.3 21.6 a2.3 2.3 0 1 1 3.3 2.1 c-0.6 0.3 -1 0.8 -1 1.5", 1.7),
                                            ("dot", (23.6, 27.6, 1.05))])], (4, 2.5), 0),
    "context-menu": ([[fill(SMALL_ARROW), ("rect", (17, 16, 13, 14, 2)),
                       detail("M19.8 20 H27.2 M19.8 23 H27.2 M19.8 26 H27.2", 1.4)]], (4, 2.5), 0),
    "copy": ([[fill(SMALL_ARROW)] + badge([detail("M23.5 20 V27 M20 23.5 H27", 1.9)])], (4, 2.5), 0),
    "alias": ([[fill(SMALL_ARROW)] + badge([detail("M20.5 26.5 L26.2 20.8 M22 20.5 H26.5 V25", 1.7)])], (4, 2.5), 0),
    "no-drop": ([[fill(SMALL_ARROW)] + badge([detail("M23.5 23.5 m-3.3 0 a3.3 3.3 0 1 0 6.6 0 a3.3 3.3 0 1 0 -6.6 0 "
                                                     "M21.2 21.2 L25.8 25.8", 1.5)])], (4, 2.5), 0),
    "zoom-in": ([[ring(13, 13, 8, 3.2), stroke("M19 19 L27.5 27.5", 4.2), detail("M13 9.5 V16.5 M9.5 13 H16.5", 2)]],
                (13, 13), 0),
    "zoom-out": ([[ring(13, 13, 8, 3.2), stroke("M19 19 L27.5 27.5", 4.2), detail("M9.5 13 H16.5", 2)]], (13, 13), 0),
    "ns-resize": ([double_arrow(0)], (16, 16), 0),
    "ew-resize": ([double_arrow(90)], (16, 16), 0),
    "nesw-resize": ([double_arrow(45, 25)], (16, 16), 0),
    "nwse-resize": ([double_arrow(-45, 25)], (16, 16), 0),
    "col-resize": ([[stroke("M13.5 8 V24 M18.5 8 V24", 2.4), stroke("M4.5 16 H9.5 M22.5 16 H27.5", 2.4),
                     head(3, 16, 270), head(29, 16, 90)]], (16, 16), 0),
    "row-resize": ([[stroke("M8 13.5 H24 M8 18.5 H24", 2.4), stroke("M16 4.5 V9.5 M16 22.5 V27.5", 2.4),
                     head(16, 3, 0), head(16, 29, 180)]], (16, 16), 0),
    "n-resize": ([single_arrow(0)], (16, 4), 0),
    "s-resize": ([single_arrow(180)], (16, 28), 0),
    "e-resize": ([single_arrow(90)], (28, 16), 0),
    "w-resize": ([single_arrow(270)], (4, 16), 0),
    "ne-resize": ([single_arrow(45)], (24.5, 7.5), 0),
    "nw-resize": ([single_arrow(-45)], (7.5, 7.5), 0),
    "se-resize": ([single_arrow(135)], (24.5, 24.5), 0),
    "sw-resize": ([single_arrow(-135)], (7.5, 24.5), 0),
    "pencil": ([[group([fill("M0 0 L4 -1.5 L19 -1.5 a2.5 2.5 0 0 1 0 5 L4 3.5 Z")], "translate(4 28) rotate(-45)"),
                 detail("M5.5 26.5 l1.8 -1.8", 1.4)]], (4, 28), 0),
}

ALIASES = {
    "default": ["left_ptr", "arrow", "top_left_arrow", "left-arrow", "X_cursor", "x-cursor", "right_ptr",
                "draft_large", "draft_small"],
    "pointer": ["hand2", "hand1", "hand", "pointing_hand", "e29285e634086352946a0e7090d73106",
                "9d800788f1b08800ae810202380a0822"],
    "grab": ["openhand", "fleur-open", "5aca4d189052212118709018842178c0", "208530c400c041818281048008011002"],
    "grabbing": ["closedhand", "dnd-move", "dnd-none", "fcf21c00b30f7e3f83fe0dfd12e71cff", "4498f0e0c1937ffe01fd06f973665830",
                 "9081237383d90e509aa00f00170e968f"],
    "text": ["xterm", "ibeam"],
    "vertical-text": ["vertical_text"],
    "crosshair": ["cross", "tcross", "cross_reverse", "diamond_cross", "target"],
    "cell": ["plus"],
    "move": ["fleur", "all-scroll", "size_all", "all-resize", "4498f0e0c1937ffe01fd06f973665830"],
    "not-allowed": ["crossed_circle", "forbidden", "circle", "03b6e0fcb3499374a867c041f52298f0", "pirate"],
    "wait": ["watch"],
    "progress": ["left_ptr_watch", "half-busy", "00000000000000020006000e7e9ffc3f", "08e8e1c95fe2fc01f976f1e063a24ccd",
                 "3ecb610c1bf2410f44200f48c40d3599"],
    "help": ["question_arrow", "whats_this", "left_ptr_help", "dnd-ask", "5c6cd98b3f3ebcb1f9c7f1c204630408",
             "d9ce0ab605698f320427677b458ad60b"],
    "context-menu": [],
    "copy": ["dnd-copy", "1081e37283d90000800003c07f3ef6bf", "6407b0e94181790501fd1e167b474872",
             "b66166c04f8c3109214a4fbd64a50fc8"],
    "alias": ["dnd-link", "link", "0876e1c15ff2fc01f906f1c363074c0f", "3085a0e285430894940527032f8b26df",
              "640fb0e74195791501fd1ed57b41487f", "a2a266d0498c3104214a47bd64ab0fc8"],
    "no-drop": ["dnd-no-drop", "03b6e0fcb3499374a867d041f52298f0"],
    "zoom-in": ["zoom_in"],
    "zoom-out": ["zoom_out"],
    "ns-resize": ["sb_v_double_arrow", "v_double_arrow", "size_ver", "n-s-resize", "00008160000006810000408080010102",
                  "double_arrow"],
    "ew-resize": ["sb_h_double_arrow", "h_double_arrow", "size_hor", "e-w-resize", "028006030e0e7ebffc7f7070c0600140"],
    "nesw-resize": ["size_bdiag", "fd_double_arrow", "fcf1c3c7cd4491d801f1e1c78f100000"],
    "nwse-resize": ["size_fdiag", "bd_double_arrow", "c7088f0f3e6c8088236ef8e1e3e70000"],
    "col-resize": ["split_h", "sb_h_double_arrow_split", "14fef782d02440884392942c11205230"],
    "row-resize": ["split_v", "2870a09082c103050810ffdffffe0204"],
    "n-resize": ["top_side"],
    "s-resize": ["bottom_side"],
    "e-resize": ["right_side"],
    "w-resize": ["left_side"],
    "ne-resize": ["top_right_corner"],
    "nw-resize": ["top_left_corner", "ul_angle"],
    "se-resize": ["bottom_right_corner", "lr_angle"],
    "sw-resize": ["bottom_left_corner", "ll_angle"],
    "pencil": ["draft"],
}

THEMES = {
    "fldesktop-cursors": ("#1f2023", "#ffffff", "#3584e4"),
    "fldesktop-cursors-light": ("#ffffff", "#1f2023", "#3584e4"),
}


def shape_svg(shape, fg, ol, accent, outline_pass):
    kind, d = shape
    ow = OUTLINE_W * 2
    if kind == "group":
        shapes, tr = d
        return f'<g transform="{tr}">' + "".join(shape_svg(s, fg, ol, accent, outline_pass) for s in shapes) + "</g>"
    if kind == "fill":
        if outline_pass:
            return f'<path d="{d}" fill="{ol}" stroke="{ol}" stroke-width="{ow}" stroke-linejoin="round"/>'
        return f'<path d="{d}" fill="{fg}"/>'
    if kind == "stroke":
        p, w = d
        if outline_pass:
            return (f'<path d="{p}" fill="none" stroke="{ol}" stroke-width="{w + ow}" stroke-linecap="round" '
                    f'stroke-linejoin="round"/>')
        return f'<path d="{p}" fill="none" stroke="{fg}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"/>'
    if kind == "circle":
        cx, cy, r = d
        return f'<circle cx="{cx}" cy="{cy}" r="{r + (OUTLINE_W if outline_pass else 0)}" fill="{ol if outline_pass else fg}"/>'
    if kind == "rect":
        x, y, w, h, rx = d
        if outline_pass:
            return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{ol}" stroke="{ol}" '
                    f'stroke-width="{ow}"/>')
        return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fg}"/>'
    if kind == "ring":
        cx, cy, r, w = d
        col, sw = (ol, w + ow) if outline_pass else (fg, w)
        return f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="{col}" stroke-width="{sw}"/>'
    if kind == "arc":
        cx, cy, r, w = d
        if outline_pass:
            return ""
        # a quarter of the ring, in the accent color
        return (f'<path d="M{cx} {cy - r} A{r} {r} 0 0 1 {cx + r} {cy}" fill="none" stroke="{accent}" '
                f'stroke-width="{w}" stroke-linecap="round"/>')
    if kind == "detail":
        if outline_pass:
            return ""
        p, w = d
        return f'<path d="{p}" fill="none" stroke="{ol}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"/>'
    if kind == "dot":
        if outline_pass:
            return ""
        cx, cy, r = d
        return f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{ol}"/>'
    raise ValueError(kind)


def cursor_svg(shapes, fg, ol, accent):
    shadow = "".join(shape_svg(s, "#000000", "#000000", "#000000", True) for s in shapes)
    outline = "".join(shape_svg(s, fg, ol, accent, True) for s in shapes)
    body = "".join(shape_svg(s, fg, ol, accent, False) for s in shapes)
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">'
            f'<g transform="translate(0.6 0.9)" opacity="0.22">{shadow}</g>{outline}{body}</svg>\n')


def render(job):
    svg, png, size = job
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", png, svg], check=True)


def build(out, preview_dir=None):
    for theme, (fg, ol, accent) in THEMES.items():
        root = os.path.join(out, theme)
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(os.path.join(root, "cursors"))
        with tempfile.TemporaryDirectory() as tmp:
            jobs, plan = [], {}
            for name, (frames, hot, delay) in CURSORS.items():
                plan[name] = []
                for fi, shapes in enumerate(frames):
                    svg = os.path.join(tmp, f"{name}-{fi}.svg")
                    with open(svg, "w") as f:
                        f.write(cursor_svg(shapes, fg, ol, accent))
                    if preview_dir and fi == 0:
                        shutil.copy(svg, os.path.join(preview_dir, f"{theme}-{name}.svg"))
                    for s in SIZES:
                        png = os.path.join(tmp, f"{name}-{fi}-{s}.png")
                        jobs.append((svg, png, s))
                        plan[name].append((s, png, round(hot[0] * s / 32), round(hot[1] * s / 32), delay))
            with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
                list(ex.map(render, jobs))
            for name, images in plan.items():
                images.sort(key=lambda i: i[0])  # by size, frames in order within a size
                write_xcursor(os.path.join(root, "cursors", name), images)
                for a in ALIASES.get(name, []):
                    link = os.path.join(root, "cursors", a)
                    if not os.path.lexists(link):
                        os.symlink(name, link)
        light = theme.endswith("light")
        with open(os.path.join(root, "index.theme"), "w") as f:
            f.write(f"[Icon Theme]\nName=fldesktop{' Light' if light else ''} Cursors\n"
                    f"Comment={'Light' if light else 'Dark'} cursors for fldesktop\n")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "../../data/icons")
    preview = sys.argv[2] if len(sys.argv) > 2 else None
    build(os.path.abspath(out), preview)
    print(f"{len(CURSORS)} cursors x {len(THEMES)} themes written to {os.path.abspath(out)}")


if __name__ == "__main__":
    main()
