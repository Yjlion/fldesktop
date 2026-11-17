#!/usr/bin/env python3
"""Checks the generated icon and cursor themes (tools/artwork/build.sh output).

 - every icon name the fldesktop programs ask for resolves in the fldesktop
   theme (or hicolor), and every accent / monochrome theme is complete
 - no SVG uses masks, filters or clip paths (FLTK's nanosvg ignores them)
 - every directory an index.theme lists exists; no dangling symlinks
 - every cursor file parses and the cursor names fl-wm and toolkits use exist
"""
import os
import re
import sys

top = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(top, "tools", "artwork"))
from xcursor import read_xcursor  # noqa: E402

icons = os.path.join(top, "data", "icons")
failures = []


def fail(msg):
    failures.append(msg)


def theme_names(theme):
    names = set()
    for d, _, files in os.walk(os.path.join(icons, theme)):
        for f in files:
            if f.endswith((".svg", ".png")):
                names.add(f.rsplit(".", 1)[0])
    return names


def theme_dirs(theme):
    idx = open(os.path.join(icons, theme, "index.theme")).read()
    dirs = []
    for key in ("Directories", "ScaledDirectories"):
        m = re.search(rf"^{key}=(.*)$", idx, re.M)
        if m:
            dirs += [d for d in m.group(1).split(",") if d]
    return idx, dirs


# 1. names used in the code
used = set()
pat = re.compile(r'(?:icon|symbolic_icon|icon_first|icon_name|set_window_icon|icon_path)\s*\(([^;]*)')
for root in ("apps", "shell", "common", "greeter", "portal"):
    for d, _, files in os.walk(os.path.join(top, root)):
        for f in files:
            if f.endswith(".cpp"):
                for m in pat.finditer(open(os.path.join(d, f), errors="replace").read()):
                    used.update(re.findall(r'"([a-z][a-z0-9.+-]*)"', m.group(1)))
for f in os.listdir(os.path.join(top, "data", "applications")):
    m = re.search(r"^Icon=(.*)$", open(os.path.join(top, "data", "applications", f)).read(), re.M)
    if m:
        used.add(m.group(1).strip())
# strings in the same call that are not icon names
not_icons = {"wifi", "ethernet"}
used = {u for u in used if not u.endswith("-symbolic") and u != "symbolic" and u not in not_icons}
have = theme_names("fldesktop") | theme_names("hicolor")
for n in sorted(used):
    if n not in have:
        fail(f"icon '{n}' is used but not in the fldesktop theme")
mono = theme_names("fldesktop-mono")
base_scalable = {f.rsplit(".", 1)[0] for _, _, fs in os.walk(os.path.join(icons, "fldesktop", "scalable")) for f in fs}
for n in sorted(base_scalable - mono):
    fail(f"fldesktop-mono lacks '{n}'")
symbolic = {f[:-len("-symbolic.svg")] for _, _, fs in os.walk(os.path.join(icons, "fldesktop", "symbolic")) for f in fs}
for n in sorted(base_scalable - symbolic):
    fail(f"fldesktop lacks '{n}-symbolic'")

# 2.-3. every theme
themes = sorted(t for t in os.listdir(icons) if t.startswith("fldesktop") and not t.startswith("fldesktop-cursors"))
for t in themes:
    if not os.path.exists(os.path.join(icons, t, "index.theme")):
        fail(f"{t}: no index.theme")
        continue
    idx, dirs = theme_dirs(t)
    for d in dirs:
        if not os.path.isdir(os.path.join(icons, t, d)):
            fail(f"{t}: index.theme lists {d}, which does not exist")
        elif f"[{d}]" not in idx:
            fail(f"{t}: no section for {d}")
    for d, _, files in os.walk(os.path.join(icons, t)):
        for f in files:
            p = os.path.join(d, f)
            if os.path.islink(p) and not os.path.exists(p):
                fail(f"dangling symlink {os.path.relpath(p, icons)}")
            elif f.endswith(".svg"):
                text = open(p).read()
                for bad in ("<mask", "<filter", "<clipPath"):
                    if bad in text:
                        fail(f"{os.path.relpath(p, icons)} uses {bad}>")
for acc in ("teal", "green", "orange", "red", "purple", "pink", "graphite"):
    t = f"fldesktop-{acc}"
    if "folder" not in theme_names(t):
        fail(f"{t} has no folder icon")

# 4. cursors
needed = ["default", "pointer", "text", "wait", "progress", "crosshair", "move", "grab", "grabbing", "not-allowed",
          "help", "context-menu", "copy", "alias", "no-drop", "cell", "vertical-text", "all-scroll", "zoom-in",
          "zoom-out", "e-resize", "n-resize", "ne-resize", "nw-resize", "s-resize", "se-resize", "sw-resize",
          "w-resize", "ew-resize", "ns-resize", "nesw-resize", "nwse-resize", "col-resize", "row-resize",
          "left_ptr", "xterm", "hand2", "watch", "fleur", "top_left_corner", "sb_h_double_arrow"]
for t in ("fldesktop-cursors", "fldesktop-cursors-light"):
    cdir = os.path.join(icons, t, "cursors")
    for n in needed:
        p = os.path.join(cdir, n)
        if not os.path.exists(p):
            fail(f"{t}: cursor {n} is missing")
            continue
        try:
            imgs = read_xcursor(p)
            sizes = sorted({i["nominal"] for i in imgs})
            if sizes[:1] != [24] or 48 not in sizes:
                fail(f"{t}/{n}: sizes {sizes}")
        except Exception as e:  # noqa: BLE001
            fail(f"{t}/{n}: {e}")

for f in failures:
    print("FAIL:", f)
print(f"artwork-check: {len(used)} icon names used, {len(themes)} icon themes, {len(failures)} failures")
sys.exit(1 if failures else 0)
