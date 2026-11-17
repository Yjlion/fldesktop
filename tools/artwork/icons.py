#!/usr/bin/env python3
"""Generates the fldesktop icon themes into data/icons/.

  fldesktop            full color, blue accent (folders, places)
  fldesktop-<accent>   only the accent colored icons, inherits fldesktop
  fldesktop-mono       flat monochrome icons for light backgrounds
  fldesktop-mono-dark  the same for dark backgrounds

Every theme also has the -symbolic icons (recolorable, #2e3436). The PNG
sizes are rendered with rsvg-convert. Run tools/artwork/build.sh.
"""
import colorsys
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glyphs import ALIASES, G  # noqa: E402

ACCENTS = {
    "blue": "#3584e4",
    "teal": "#2190a4",
    "green": "#3a944a",
    "orange": "#ed5b00",
    "red": "#e62d42",
    "purple": "#9141ac",
    "pink": "#d56199",
    "graphite": "#6f8396",
}
SIZES = [16, 22, 24, 32, 48, 64]
SCALED = {16: 32, 24: 48, 32: 64}  # size@2 -> rendered as this size (symlinked)
SYMBOLIC = "#2e3436"
ACTION = "#3d4046"
MONO_LIGHT = "#2e3436"
MONO_DARK = "#eeeeec"

# ---- color helpers ----------------------------------------------------------


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


def hexc(r, g, b):
    return "#%02x%02x%02x" % tuple(max(0, min(255, round(v * 255))) for v in (r, g, b))


def shade(h, f):
    """f > 0 lightens, f < 0 darkens (HLS lightness)."""
    hh, ll, ss = colorsys.rgb_to_hls(*rgb(h))
    ll = ll + (1 - ll) * f if f > 0 else ll * (1 + f)
    return hexc(*colorsys.hls_to_rgb(hh, ll, ss))


# ---- contexts and kinds -----------------------------------------------------

PLACES = {
    "folder", "folder-open", "folder-new", "folder-remote", "folder-saved-search", "user-home", "user-desktop",
    "folder-documents", "folder-download", "folder-music", "folder-pictures", "folder-videos", "folder-templates",
    "folder-publicshare", "inode-directory", "user-bookmarks", "system-file-manager",
}
TRASH = {"user-trash", "user-trash-full"}
# sheet icons: glyph color
MIMETYPES = {
    "text-x-generic": "#5e6670", "text-x-script": "#2f8a7a", "text-html": "#2a7fd4", "text-x-preview": "#5e6670",
    "application-x-generic": "#6f7680", "application-x-sharedlib": "#7a6a5a", "application-pdf": "#d0342c",
    "application-x-archive": "#a0772e", "x-office-document": "#2a6fc9", "x-office-spreadsheet": "#2e9150",
    "x-office-drawing": "#c4508f", "unknown": "#8a9099", "inode-symlink": "#6f7680",
}
# tile icons: tile color
TILES = {
    # mimetypes drawn as tiles
    "application-x-executable": ("#4b5563", "mimetypes"), "package-x-generic": ("#b07c3a", "mimetypes"),
    "application-x-addon": ("#5f7f3a", "mimetypes"), "image-x-generic": ("#c45b9c", "mimetypes"),
    "audio-x-generic": ("#e07a2a", "mimetypes"), "video-x-generic": ("#7a4fc2", "mimetypes"),
    "font-x-generic": ("#5c6a7a", "mimetypes"), "x-office-presentation": ("#d8662a", "mimetypes"),
    "x-office-calendar": ("#d0453c", "mimetypes"), "x-office-address-book": ("#4a6fa5", "mimetypes"),
    # devices
    "drive-harddisk": ("#5d6672", "devices"), "drive-removable-media": ("#5d6672", "devices"),
    "media-removable": ("#5d6672", "devices"), "media-flash": ("#5d6672", "devices"),
    "drive-optical": ("#7a8594", "devices"), "media-optical": ("#7a8594", "devices"),
    "computer": ("#3e5f8a", "devices"), "video-display": ("#3e5f8a", "devices"),
    "computer-laptop": ("#3e5f8a", "devices"), "phone": ("#3a6f6a", "devices"),
    "input-mouse": ("#5d6672", "devices"), "input-keyboard": ("#5d6672", "devices"),
    "input-gaming": ("#6d4fb0", "devices"), "input-tablet": ("#5d6672", "devices"),
    "input-touchpad": ("#5d6672", "devices"), "audio-card": ("#4f6a3c", "devices"),
    "audio-speakers": ("#4f5866", "devices"), "audio-headphones": ("#4f5866", "devices"),
    "audio-headset": ("#4f5866", "devices"), "audio-input-microphone": ("#4f5866", "devices"),
    "camera-photo": ("#4f5866", "devices"), "camera-web": ("#4f5866", "devices"),
    "camera-video": ("#4f5866", "devices"), "printer": ("#56606c", "devices"),
    "printer-network": ("#56606c", "devices"), "scanner": ("#56606c", "devices"),
    "modem": ("#56606c", "devices"), "multimedia-player": ("#c0462f", "devices"),
    "battery": ("#3f8a4a", "devices"), "bluetooth": ("#2a6fd4", "devices"),
    "network-wired": ("#3e6e9a", "devices"), "network-wireless": ("#3e6e9a", "devices"),
    "network-workgroup": ("#3e6e9a", "places"), "network-server": ("#4a5563", "places"),
    "start-here": ("ACCENT", "places"),
    # categories
    "applications-accessories": ("#8a6d3b", "categories"), "applications-utilities": ("#8a6d3b", "categories"),
    "applications-development": ("#30475e", "categories"), "applications-engineering": ("#866a2b", "categories"),
    "applications-games": ("#6d4fb0", "categories"), "applications-graphics": ("#c44f7c", "categories"),
    "applications-internet": ("#2a7fd4", "categories"), "applications-multimedia": ("#d6582d", "categories"),
    "applications-office": ("#b8742b", "categories"), "applications-science": ("#2f8a7a", "categories"),
    "applications-education": ("#3a7c3a", "categories"), "applications-other": ("#6f7680", "categories"),
    "applications-system": ("#56606c", "categories"), "preferences-desktop": ("#4a6fa5", "categories"),
    "preferences-system": ("#56606c", "categories"), "preferences-other": ("#6f7680", "categories"),
    "preferences-desktop-theme": ("#b8509a", "categories"), "preferences-desktop-display": ("#3e5f8a", "categories"),
    "preferences-desktop-keyboard": ("#5d6672", "categories"),
    "preferences-desktop-keyboard-shortcuts": ("#5d6672", "categories"),
    "preferences-desktop-locale": ("#2f7f9a", "categories"), "preferences-desktop-wallpaper": ("#3d8f6a", "categories"),
    "preferences-desktop-font": ("#6a5a8a", "categories"), "preferences-desktop-accessibility": ("#2a6fd4", "categories"),
    "preferences-desktop-screensaver": ("#3f4a5a", "categories"),
    "preferences-desktop-default-applications": ("#3e8a5a", "categories"),
    "preferences-system-network": ("#3e6e9a", "categories"), "preferences-system-notifications": ("#d0853a", "categories"),
    "preferences-system-time": ("#4a6fa5", "categories"), "preferences-system-privacy": ("#3f7a4a", "categories"),
    "preferences-color": ("#b8509a", "categories"),
    # applications with generic names
    "system-software-install": ("#c46a2b", "apps"), "system-software-update": ("#2e8a4a", "apps"),
    "utilities-terminal": ("#2b2f36", "apps"), "utilities-system-monitor": ("#2f6f5a", "apps"),
    "accessories-text-editor": ("#3e6e9a", "apps"), "accessories-calculator": ("#4f5866", "apps"),
    "accessories-character-map": ("#6a5a8a", "apps"), "accessories-screenshot": ("#4f5866", "apps"),
    "applets-screenshooter": ("#4f5866", "apps"), "web-browser": ("#2a7fd4", "apps"),
    "internet-mail": ("#2a6fc9", "apps"), "multimedia-video-player": ("#7a4fc2", "apps"),
    "audio-player": ("#e07a2a", "apps"), "image-viewer": ("#c45b9c", "apps"), "help-browser": ("#4a6fa5", "apps"),
    "system-help": ("#4a6fa5", "apps"), "system-users": ("#4a6fa5", "apps"),
}
# status icons: (shape, color)
STATUS = {
    "dialog-information": ("circle", "#3584e4"), "dialog-question": ("circle", "#8a4fc4"),
    "dialog-warning": ("triangle", "#f5b52e"), "dialog-error": ("circle", "#e01b24"),
    "dialog-password": ("circle", "#5d6672"), "emblem-important": ("triangle", "#f5b52e"),
    "emblem-ok": ("circle", "#2e9a4a"), "emblem-default": ("circle", "#2e9a4a"),
    "security-high": ("shield", "#2e9a4a"), "security-medium": ("shield", "#e5a50a"),
    "security-low": ("shield", "#e01b24"), "help-about": ("circle", "#3584e4"),
    "help-contents": ("circle", "#3584e4"), "help-faq": ("circle", "#3584e4"),
}
EMBLEMS = {n for n in G if n.startswith("emblem-")}
STATUS_CTX = {
    "audio-volume", "battery", "network-", "bluetooth-", "notifications", "user-", "printer-", "dialog-",
    "security-", "image-", "process-", "microphone-", "airplane", "display-brightness", "weather-", "avatar",
    "changes-", "software-update", "starred", "non-starred", "mark-location", "find-location", "alarm",
}

# The fldesktop programs, drawn as glyphs in the monochrome themes.
APPS_MONO = {
    "fl-bluetooth": "bluetooth", "fl-calc": "accessories-calculator", "fl-charmap": "accessories-character-map",
    "fl-edit": "accessories-text-editor", "fl-files": "folder", "fl-firewall": "security-high",
    "fl-flatpak": "package-x-generic", "fl-mixer": "audio-volume-high", "fl-network": "network-workgroup",
    "fl-osk": "input-keyboard", "fl-packages": "system-software-install", "fl-printers": "printer",
    "fl-settings": "preferences-system", "fl-term": "utilities-terminal", "fl-view": "image-viewer",
    "fldesktop": "start-here", "fl-taskmgr": "utilities-system-monitor", "fl-video": "multimedia-video-player",
    "fl-lock": "system-lock-screen",
}


def context(name):
    if name in PLACES or name in TRASH:
        return "places"
    if name in TILES:
        return TILES[name][1]
    if name in MIMETYPES or name.startswith(("text-", "application-", "image-x", "audio-x", "video-x", "x-office",
                                              "inode-", "font-", "package-")):
        return "mimetypes"
    if name in EMBLEMS:
        return "emblems"
    if name in STATUS or name.startswith(tuple(STATUS_CTX)):
        return "status"
    if name.startswith(("applications-", "preferences-")):
        return "categories"
    if name.startswith(("drive-", "media-optical", "media-removable", "media-flash", "input-", "computer", "phone",
                        "camera-", "audio-card", "audio-speakers", "audio-head", "audio-input", "video-display",
                        "scanner", "modem", "multimedia-player")):
        return "devices"
    if name in APPS_MONO or name.startswith(("utilities-", "accessories-", "system-software")):
        return "apps"
    return "actions"


# ---- SVG building -----------------------------------------------------------


def glyph_svg(name, color, scale=1.0, dx=0.0, dy=0.0):
    body = "".join(e.replace("{c}", color) for e in G[name])
    if scale == 1 and dx == 0 and dy == 0:
        return body
    return f'<g transform="translate({dx:g} {dy:g}) scale({scale:g})">{body}</g>'


def doc(size, body, defs=""):
    d = f"<defs>{defs}</defs>" if defs else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
            f'viewBox="0 0 {size} {size}">{d}{body}</svg>\n')


def grad(gid, top, bottom):
    # percentages: nanosvg (FLTK) reads plain numbers as user units
    return (f'<linearGradient id="{gid}" x1="0%" y1="0%" x2="0%" y2="100%">'
            f'<stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient>')


def symbolic(name, color=SYMBOLIC):
    return doc(16, glyph_svg(name, color))


def folder_icon(name, accent):
    back, front_top, front_bot = shade(accent, -0.25), shade(accent, 0.28), accent
    emblem = shade(accent, -0.45)
    defs = grad("f", front_top, front_bot)
    body = (
        f'<path d="M6 14 a4 4 0 0 1 4 -4 h14 l6 6 h24 a4 4 0 0 1 4 4 v32 a4 4 0 0 1 -4 4 h-44 a4 4 0 0 1 -4 -4 z" fill="{back}"/>'
        f'<rect x="8" y="18" width="48" height="10" rx="2" fill="#ffffff" fill-opacity="0.92"/>'
        f'<path d="M4 26 a4 4 0 0 1 4 -4 h48 a4 4 0 0 1 4 4 v26 a4 4 0 0 1 -4 4 h-48 a4 4 0 0 1 -4 -4 z" fill="url(#f)"/>'
        f'<rect x="4" y="53" width="56" height="3" rx="1.5" fill="#000000" fill-opacity="0.12"/>'
    )
    if name not in ("folder", "inode-directory", "system-file-manager"):
        g = "folder" if name == "folder-open" else name
        if g == "folder":
            pass
        else:
            body += glyph_svg(g, emblem, 1.5, 20, 27) if g not in ("folder-new", "folder-remote", "folder-saved-search") \
                else glyph_svg({"folder-new": "list-add", "folder-remote": "text-html",
                                "folder-saved-search": "system-search"}[g], emblem, 1.4, 20.8, 28)
    return doc(64, body, defs)


def trash_icon(name):
    defs = grad("t", "#c9ced6", "#9aa1ab")
    body = (
        '<rect x="10" y="10" width="44" height="7" rx="3" fill="#6f7680"/>'
        '<rect x="25" y="5" width="14" height="6" rx="2" fill="#6f7680"/>'
        '<path d="M13 19 h38 l-3 38 a4 4 0 0 1 -4 3.5 h-24 a4 4 0 0 1 -4 -3.5 z" fill="url(#t)"/>'
        '<path d="M24 26 v26 M32 26 v26 M40 26 v26" stroke="#7d848e" stroke-width="3" stroke-linecap="round"/>'
    )
    if name == "user-trash-full":
        body = body.replace('<rect x="10" y="10"', '<path d="M14 12 l8 -8 l9 5 l8 -6 l10 9 z" fill="#f2f2f2"/>'
                                                     '<rect x="10" y="10"')
    return doc(64, body, defs)


def sheet_icon(name, color):
    body = (
        '<path d="M14 4 h26 l12 12 v42 a2 2 0 0 1 -2 2 h-36 a2 2 0 0 1 -2 -2 v-52 a2 2 0 0 1 2 -2 z" fill="#000000" fill-opacity="0.15" transform="translate(0 1.5)"/>'
        '<path d="M14 4 h26 l12 12 v42 a2 2 0 0 1 -2 2 h-36 a2 2 0 0 1 -2 -2 v-52 a2 2 0 0 1 2 -2 z" fill="#fbfbfc" stroke="#b9bec6" stroke-width="1"/>'
        '<path d="M40 4 v10 a2 2 0 0 0 2 2 h10 z" fill="#dfe2e6"/>'
        f'<path d="M12 50 h40 v8 a2 2 0 0 1 -2 2 h-36 a2 2 0 0 1 -2 -2 z" fill="{color}"/>'
    )
    g = name if name in G else "text-x-generic"
    # draw the glyph without its own sheet outline where it has one
    elems = [e for e in G[g] if 'M3.5 1.75 h6 l3 3' not in e]
    inner = "".join(e.replace("{c}", color) for e in elems)
    body += f'<g transform="translate(12 8) scale(2.5)">{inner}</g>'
    return doc(64, body)


def tile_icon(name, color):
    top, bot = shade(color, 0.22), shade(color, -0.12)
    defs = grad("b", top, bot)
    body = (
        '<rect x="5" y="7" width="54" height="54" rx="12" fill="#000000" fill-opacity="0.18"/>'
        '<rect x="5" y="4" width="54" height="54" rx="12" fill="url(#b)"/>'
        '<rect x="5" y="4" width="54" height="27" rx="12" fill="#ffffff" fill-opacity="0.06"/>'
        + glyph_svg(name, "#ffffff", 2.25, 14, 13)
    )
    return doc(64, body, defs)


def status_icon(name, shape, color):
    top, bot = shade(color, 0.18), shade(color, -0.1)
    defs = grad("s", top, bot)
    if shape == "triangle":
        base = '<path d="M32 5 a5 5 0 0 1 4.4 2.6 l24 43 a5 5 0 0 1 -4.4 7.4 h-48 a5 5 0 0 1 -4.4 -7.4 l24 -43 a5 5 0 0 1 4.4 -2.6 z" fill="url(#s)"/>'
        mark = ('<path d="M32 22 v16" stroke="#3a2a00" stroke-width="6" stroke-linecap="round"/>'
                '<circle cx="32" cy="48" r="3.5" fill="#3a2a00"/>')
        return doc(64, base + mark, defs)
    if shape == "shield":
        base = '<path d="M32 4 l24 9 v16 c0 15 -10 25 -24 31 c-14 -6 -24 -16 -24 -31 v-16 z" fill="url(#s)"/>'
    else:
        base = '<circle cx="32" cy="33" r="27" fill="#000000" fill-opacity="0.15"/><circle cx="32" cy="31" r="27" fill="url(#s)"/>'
    # the glyph minus its own outline
    elems = [e for e in G[name] if not e.startswith('<circle cx="8" cy="8" r="6.25"') and "M8 1.5 l5.75 2.25" not in e]
    inner = "".join(e.replace("{c}", "#ffffff") for e in elems)
    return doc(64, base + f'<g transform="translate(9.5 8.5) scale(2.8)">{inner}</g>', defs)


def action_icon(name, color=ACTION):
    return doc(16, glyph_svg(name, color))


def color_icon(name, accent):
    if name in PLACES:
        return folder_icon(name, accent)
    if name in TRASH:
        return trash_icon(name)
    if name in MIMETYPES:
        return sheet_icon(name, MIMETYPES[name])
    if name in TILES:
        c = TILES[name][0]
        return tile_icon(name, accent if c == "ACCENT" else c)
    if name in STATUS:
        return status_icon(name, *STATUS[name])
    return action_icon(name)


ACCENTED = PLACES | {"start-here"}

# ---- writing themes ---------------------------------------------------------

jobs = []  # (svg path, png path, size)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def emit(root, ctx, name, svg, pngs=True):
    p = os.path.join(root, "scalable", ctx, name + ".svg")
    write(p, svg)
    if pngs:
        for s in SIZES:
            jobs.append((p, os.path.join(root, f"{s}x{s}", ctx, name + ".png"), s))


def link_scaled(root, contexts):
    for s, real in SCALED.items():
        for ctx in contexts:
            src_dir = os.path.join(root, f"{real}x{real}", ctx)
            if not os.path.isdir(src_dir):
                continue
            dst_dir = os.path.join(root, f"{s}x{s}@2", ctx)
            os.makedirs(dst_dir, exist_ok=True)
            for f in sorted(os.listdir(src_dir)):
                os.symlink(f"../../{real}x{real}/{ctx}/{f}", os.path.join(dst_dir, f))


def index_theme(root, name, comment, inherits, contexts):
    dirs, scaled, sections = [], [], []
    for s in SIZES:
        for ctx in contexts:
            if os.path.isdir(os.path.join(root, f"{s}x{s}", ctx)):
                d = f"{s}x{s}/{ctx}"
                dirs.append(d)
                sections.append(f"[{d}]\nContext={ctx.capitalize()}\nSize={s}\nType=Fixed\n")
    for s in SCALED:
        for ctx in contexts:
            if os.path.isdir(os.path.join(root, f"{s}x{s}@2", ctx)):
                d = f"{s}x{s}@2/{ctx}"
                scaled.append(d)
                sections.append(f"[{d}]\nContext={ctx.capitalize()}\nSize={s}\nScale=2\nType=Fixed\n")
    for kind in ("scalable", "symbolic"):
        for ctx in contexts:
            if os.path.isdir(os.path.join(root, kind, ctx)):
                d = f"{kind}/{ctx}"
                dirs.append(d)
                sections.append(f"[{d}]\nContext={ctx.capitalize()}\nSize={16 if kind == 'symbolic' else 64}\n"
                                f"MinSize=8\nMaxSize=512\nType=Scalable\n")
    head = (f"[Icon Theme]\nName={name}\nComment={comment}\nInherits={inherits}\nExample=folder\n"
            f"Directories={','.join(dirs)}\n")
    if scaled:
        head += f"ScaledDirectories={','.join(scaled)}\n"
    write(os.path.join(root, "index.theme"), head + "\n" + "\n".join(sections))


CONTEXTS = ["actions", "apps", "categories", "devices", "emblems", "mimetypes", "places", "status"]


def all_names():
    names = dict((n, n) for n in G)
    for a, target in ALIASES.items():
        if target in G and a not in names:
            names[a] = target
    return names


def link_aliases(root):
    """Alternative names become symlinks to the real icon in each directory."""
    for a, g in ALIASES.items():
        if a in G or g not in G:
            continue
        for d, _, files in os.walk(root):
            for suffix in (".svg", ".png", "-symbolic.svg"):
                if g + suffix in files and not os.path.lexists(os.path.join(d, a + suffix)):
                    os.symlink(g + suffix, os.path.join(d, a + suffix))


def build(out):
    names = {n: n for n in G}
    # --- the full color theme
    root = os.path.join(out, "fldesktop")
    for n, g in names.items():
        ctx = context(g)
        emit(root, ctx, n, color_icon(g, ACCENTS["blue"]).replace('id="', f'id="{n}-').replace('url(#', f'url(#{n}-'))
        write(os.path.join(root, "symbolic", ctx, n + "-symbolic.svg"), symbolic(g))
    # --- accent variants: only the accented icons
    for acc, col in ACCENTS.items():
        if acc == "blue":
            continue
        r = os.path.join(out, f"fldesktop-{acc}")
        for n, g in names.items():
            if g in ACCENTED:
                emit(r, context(g), n, color_icon(g, col))
    # --- monochrome
    for theme, color, comment, inherit in (("fldesktop-mono", MONO_LIGHT, "Monochrome fldesktop icons", "fldesktop"),
                                           ("fldesktop-mono-dark", MONO_DARK,
                                            "Monochrome fldesktop icons for dark backgrounds", "fldesktop-mono")):
        r = os.path.join(out, theme)
        for n, g in list(names.items()) + list(APPS_MONO.items()):
            emit(r, context(g) if n not in APPS_MONO else "apps", n, doc(16, glyph_svg(g, color)))
            if theme == "fldesktop-mono":
                write(os.path.join(r, "symbolic", context(g), n + "-symbolic.svg"), symbolic(g))
    # PNGs, then the base theme's index (after the directories exist)
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        list(ex.map(render, jobs))
    for t in os.listdir(out):
        if t.startswith("fldesktop") and t != "fldesktop-cursors" and not t.startswith("fldesktop-cursors"):
            link_aliases(os.path.join(out, t))
    for t in ["fldesktop", "fldesktop-mono", "fldesktop-mono-dark"] + [f"fldesktop-{a}" for a in ACCENTS if a != "blue"]:
        link_scaled(os.path.join(out, t), CONTEXTS)
    index_theme(root, "fldesktop", "The fldesktop icon theme", "hicolor,Adwaita", CONTEXTS)
    for t in ["fldesktop-mono", "fldesktop-mono-dark"]:
        r = os.path.join(out, t)
        inherit = "fldesktop" if t == "fldesktop-mono" else "fldesktop-mono"
        index_theme(r, "fldesktop Mono" + (" Dark" if t.endswith("dark") else ""),
                    "Monochrome fldesktop icons" + (" for dark backgrounds" if t.endswith("dark") else ""),
                    inherit + ",hicolor", CONTEXTS)
    for acc in ACCENTS:
        if acc != "blue":
            r = os.path.join(out, f"fldesktop-{acc}")
            index_theme(r, f"fldesktop {acc.capitalize()}", f"fldesktop icons with {acc} folders",
                        "fldesktop", CONTEXTS)


def render(job):
    svg, png, size = job
    os.makedirs(os.path.dirname(png), exist_ok=True)
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", png, svg], check=True)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "../../data/icons")
    out = os.path.abspath(out)
    for t in ["fldesktop", "fldesktop-mono", "fldesktop-mono-dark"] + [f"fldesktop-{a}" for a in ACCENTS]:
        shutil.rmtree(os.path.join(out, t), ignore_errors=True)
    build(out)
    print(f"{len(all_names())} icon names, {len(jobs)} PNGs written to {out}")


if __name__ == "__main__":
    main()
