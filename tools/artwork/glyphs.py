"""Monochrome glyphs for the fldesktop icon theme.

Every glyph is drawn on a 16x16 grid as a list of SVG elements in which the
string "{c}" stands for the drawing color. The color icons place the glyphs
on tiles, folders and sheets; the monochrome icons and the -symbolic icons
are the glyphs alone.

Only plain shapes, strokes and fills are used: FLTK's SVG renderer (nanosvg)
ignores masks, filters and clip paths.
"""

W = 1.5  # default stroke width


def S(d, w=W, extra=""):
    """A stroked path."""
    return (f'<path d="{d}" fill="none" stroke="{{c}}" stroke-width="{w}" '
            f'stroke-linecap="round" stroke-linejoin="round"{extra}/>')


def F(d, opacity=None, rule=None):
    """A filled path."""
    o = f' fill-opacity="{opacity}"' if opacity is not None else ""
    r = f' fill-rule="{rule}"' if rule else ""
    return f'<path d="{d}" fill="{{c}}"{o}{r}/>'


def C(cx, cy, r, fill=False, w=W, opacity=None):
    if fill:
        o = f' fill-opacity="{opacity}"' if opacity is not None else ""
        return f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{{c}}"{o}/>'
    return (f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="{{c}}" '
            f'stroke-width="{w}"/>')


def R(x, y, w, h, rx=1, fill=False, sw=W, opacity=None):
    if fill:
        o = f' fill-opacity="{opacity}"' if opacity is not None else ""
        return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{{c}}"{o}/>'
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="none" '
            f'stroke="{{c}}" stroke-width="{sw}" stroke-linejoin="round"/>')


# ---- reusable pieces --------------------------------------------------------

SHEET = S("M3.5 1.75 h6 l3 3 v9.5 h-9 z M9.5 1.75 v3 h3")
FOLDER = S("M1.75 4 v8.5 h12.5 v-7.5 h-6.5 l-1.5 -1.75 h-4.5 z")
GEAR = [
    S("M8 1.75 v2 M8 12.25 v2 M1.75 8 h2 M12.25 8 h2 M3.6 3.6 l1.4 1.4 M11 11 l1.4 1.4 "
      "M3.6 12.4 l1.4 -1.4 M11 5 l1.4 -1.4", w=2),
    C(8, 8, 3.6),
    C(8, 8, 1.2, fill=True),
]
MAGNIFIER = [C(6.75, 6.75, 4.25), S("M10 10 l4.25 4.25", w=2)]
SPEAKER = F("M2 5.5 h2.5 l3.5 -3 v11 l-3.5 -3 h-2.5 z")
LOCK = [R(3, 7, 10, 7.5, 1.5, fill=True), S("M5 7 v-2 a3 3 0 0 1 6 0 v2")]
BELL = [S("M4 11.5 v-4.5 a4 4 0 0 1 8 0 v4.5 l1.25 1.25 h-10.5 z"), F("M6.5 14 h3 a1.5 1.5 0 0 1 -3 0 z")]
PERSON = [C(8, 5, 2.75), S("M2.75 14.25 a5.25 5.25 0 0 1 10.5 0")]
MONITOR = [R(1.75, 2.25, 12.5, 8.5, 1), S("M5 14 h6 M8 10.75 v3.25")]
PLAY = F("M4.5 2.5 v11 l9 -5.5 z")
DOWN_ARROW = S("M8 2 v9 M4 7.5 l4 4 l4 -4")
UP_ARROW = S("M8 14 v-9 M4 8.5 l4 -4 l4 4")
CHECK = S("M3 8.5 l3.25 3.25 l6.75 -7", w=2)
CROSS = S("M4 4 l8 8 M12 4 l-8 8", w=2)


def signal_bars(n):
    """Wireless strength: n of 4 arcs drawn solid, the rest faint."""
    arcs = [
        "M7 13.5 a1 1 0 0 0 2 0 a1 1 0 0 0 -2 0",
        "M5.25 11.25 a3.75 3.75 0 0 1 5.5 0",
        "M3.25 9 a6.5 6.5 0 0 1 9.5 0",
        "M1.25 6.75 a9.25 9.25 0 0 1 13.5 0",
    ]
    out = []
    for i, a in enumerate(arcs):
        if i == 0:
            out.append(C(8, 13.25, 1.25, fill=True, opacity=None if n > 0 else 0.35))
        else:
            out.append(S(a, extra='' if i < n else ' stroke-opacity="0.35"'))
    return out


def battery(level, charging=False):
    out = [R(1.75, 4.5, 11, 7, 1.25), R(13, 6.5, 1.5, 3, 0.5, fill=True)]
    if level > 0:
        out.append(R(3.25, 6, max(0.75, 8 * level), 4, 0.5, fill=True))
    if charging:
        out.append(S("M8.5 4.5 l-2.5 3.5 h3 l-2.5 3.5", w=1.25))
    return out


def volume(level):
    out = [SPEAKER]
    if level < 0:
        out.append(S("M10 6 l4 4 M14 6 l-4 4"))
        return out
    waves = ["M10 6 a2.5 2.5 0 0 1 0 4", "M11.5 4 a5 5 0 0 1 0 8", "M13 2.25 a7.5 7.5 0 0 1 0 11.5"]
    for i, wv in enumerate(waves):
        out.append(S(wv, extra='' if i < level else ' stroke-opacity="0.35"'))
    return out


# ---- the glyphs -------------------------------------------------------------

G = {}

# places and folders
G["folder"] = [FOLDER]
G["folder-open"] = [S("M1.75 12.5 v-8.5 h4.5 l1.5 1.75 h5 v1.5 M1.75 12.5 l2 -5.5 h11 l-2 5.5 z")]
G["folder-new"] = [FOLDER, S("M8 6.75 v4 M6 8.75 h4")]
G["folder-remote"] = [FOLDER, C(8, 9, 2.25), S("M5.75 9 h4.5 M8 6.75 c-1 1.4 -1 3.1 0 4.5 c1 -1.4 1 -3.1 0 -4.5")]
G["folder-saved-search"] = [FOLDER, C(7.5, 8.5, 1.75), S("M8.75 9.75 l1.75 1.75")]
G["user-home"] = [S("M2 8 l6 -5.75 l6 5.75 M3.75 6.5 v7.75 h8.5 v-7.75"), R(6.5, 10, 3, 4.25, 0.5, fill=True)]
G["go-home"] = G["user-home"]
G["user-desktop"] = MONITOR
G["folder-documents"] = [SHEET, S("M5.5 8 h5 M5.5 10.5 h5 M5.5 13 h3", w=1.25)]
G["folder-download"] = [DOWN_ARROW, S("M2.5 14 h11")]
G["folder-music"] = [S("M6 12.5 v-9.5 l7 -1.5 v9.5"), C(4.25, 12.5, 1.75, fill=True), C(11.25, 11, 1.75, fill=True)]
G["folder-pictures"] = [R(1.75, 2.75, 12.5, 10.5, 1.25), S("M1.75 11 l3.5 -3.5 l3 3 l2 -2 l4 4"), C(10.5, 6, 1.25, fill=True)]
G["folder-videos"] = [R(1.75, 3.25, 9, 9.5, 1.25), F("M11.5 6.5 l3 -2 v7 l-3 -2 z")]
G["folder-templates"] = [S("M3.5 1.75 h6 l3 3 v9.5 h-9 z", extra=' stroke-dasharray="2 1.5"')]
G["folder-publicshare"] = [C(4, 8, 1.75), C(12, 3.75, 1.75), C(12, 12.25, 1.75), S("M5.5 7.25 l5 -2.75 M5.5 8.75 l5 2.75")]
G["user-trash"] = [S("M2.5 4 h11 M6 4 v-1.75 h4 v1.75 M3.75 4 l0.75 10.25 h7 l0.75 -10.25 M6.5 6.5 v5.5 M9.5 6.5 v5.5")]
G["user-trash-full"] = [S("M2.5 4 h11 M3.75 4 l0.75 10.25 h7 l0.75 -10.25"), F("M4 3.25 l1.5 -2 l2.5 1.25 l2 -1.5 l2.25 2.25 z"),
                        S("M6.5 6.5 v5.5 M9.5 6.5 v5.5")]
G["user-bookmarks"] = [S("M4 1.75 h8 v12.5 l-4 -3.5 l-4 3.5 z")]
G["bookmark-new"] = [S("M4 1.75 h5 M4 1.75 v12.5 l4 -3.5 l4 3.5 v-6"), S("M12 1.25 v4 M10 3.25 h4")]
G["network-workgroup"] = [R(5.25, 1.75, 5.5, 4, 0.75), R(1.25, 10.25, 5.5, 4, 0.75), R(9.25, 10.25, 5.5, 4, 0.75),
                          S("M8 5.75 v2.25 M4 10.25 v-2.25 h8 v2.25")]
G["network-server"] = [R(2.25, 1.75, 11.5, 5.25, 1), R(2.25, 9, 11.5, 5.25, 1), C(11, 4.4, 0.9, fill=True), C(11, 11.6, 0.9, fill=True),
                       S("M4.5 4.4 h3.5 M4.5 11.6 h3.5")]
G["start-here"] = [R(1.75, 1.75, 12.5, 12.5, 3), S("M5.5 11.5 v-7 h5 M5.5 8 h3.5", w=2)]  # the fldesktop "F"
G["fldesktop"] = G["start-here"]

# devices
G["drive-harddisk"] = [R(1.75, 4, 12.5, 8, 1.5), C(11.25, 9.25, 0.9, fill=True), S("M3.75 9.25 h4.5"), S("M2 7 h12", w=1)]
G["drive-removable-media"] = [R(4, 1.75, 8, 12.5, 1.5), S("M6 4.5 h4 M6 6.5 h4", w=1.25), C(8, 11, 1, fill=True)]
G["media-removable"] = [S("M5 7 v-5.25 h6 v5.25"), R(3.5, 7, 9, 7.25, 1.25, fill=True), S("M7 3.5 v1.5 M9 3.5 v1.5", w=1)]
G["media-flash"] = G["media-removable"]
G["drive-optical"] = [C(8, 8, 6.25), C(8, 8, 1.75)]
G["media-optical"] = G["drive-optical"]
G["media-eject"] = [F("M8 2.5 l5.5 6.5 h-11 z"), R(2.5, 11, 11, 2.5, 0.75, fill=True)]
G["computer"] = MONITOR
G["video-display"] = MONITOR
G["computer-laptop"] = [R(3, 3, 10, 7.5, 1), S("M1 13 h14 l-1.5 -2.5 h-11 z")]
G["phone"] = [R(4.25, 1.25, 7.5, 13.5, 1.75), S("M7 12 h2")]
G["input-mouse"] = [R(3.75, 1.75, 8.5, 12.5, 4.25), S("M8 1.75 v4.5 M3.75 6.25 h8.5", w=1.25)]
G["input-keyboard"] = [R(1, 3.75, 14, 8.5, 1.5), S("M3.5 6.25 h1 M6 6.25 h1 M8.5 6.25 h1 M11 6.25 h1.5 M3.5 8.25 h1 "
                                                  "M6 8.25 h1 M8.5 8.25 h1 M11 8.25 h1.5 M5 10.25 h6", w=1.25)]
G["input-gaming"] = [S("M4.5 4.5 h7 a3.5 3.5 0 0 1 3.25 4.5 l-0.75 3 a1.75 1.75 0 0 1 -3 0.75 l-1.25 -1.75 h-3.5 "
                       "l-1.25 1.75 a1.75 1.75 0 0 1 -3 -0.75 l-0.75 -3 a3.5 3.5 0 0 1 3.25 -4.5 z"),
                     S("M5 6.5 v3 M3.5 8 h3", w=1.25), C(10.5, 7, 0.8, fill=True), C(12, 8.75, 0.8, fill=True)]
G["input-tablet"] = [R(1.75, 2.75, 12.5, 10.5, 1.25), S("M5 10 l4.5 -4.5 l1 1 l-4.5 4.5 l-1.5 0.5 z", w=1.25)]
G["input-touchpad"] = [R(1.75, 2, 12.5, 12, 1.5), S("M1.75 10.5 h12.5 M8 10.5 v3.5")]
G["audio-card"] = [R(1.25, 4, 13.5, 8, 1), C(5, 8, 1.75), C(11, 8, 1.75), S("M3 12 v2 M6 12 v2 M9 12 v2")]
G["audio-speakers"] = [R(3.5, 1.75, 9, 12.5, 1.5), C(8, 9.75, 2.5), C(8, 4.75, 0.9, fill=True)]
G["audio-headphones"] = [S("M2.5 11 v-3 a5.5 5.5 0 0 1 11 0 v3"), R(1.75, 9.25, 3, 5, 1, fill=True), R(11.25, 9.25, 3, 5, 1, fill=True)]
G["audio-headset"] = G["audio-headphones"]
G["audio-input-microphone"] = [R(5.5, 1.5, 5, 8.5, 2.5, fill=True), S("M3.25 7.5 a4.75 4.75 0 0 0 9.5 0 M8 12.25 v2.25 M5.5 14.5 h5")]
G["camera-photo"] = [S("M1.75 5 h3 l1.5 -2 h3.5 l1.5 2 h3 v8.25 h-12.5 z"), C(8, 8.75, 2.5)]
G["applets-screenshooter"] = G["camera-photo"]
G["camera-web"] = [C(8, 6.5, 4.75), C(8, 6.5, 1.75), S("M4.5 14.25 h7 M8 11.25 v3")]
G["camera-video"] = G["folder-videos"]
G["printer"] = [S("M4.5 5.5 v-3.75 h7 v3.75 M4.5 11.5 h-2.75 v-6 h12.5 v6 h-2.75"), R(4.5, 9, 7, 5.25, 0.5), C(12, 7.5, 0.75, fill=True)]
G["printer-network"] = [S("M4.5 5.5 v-3.75 h7 v3.75 M4.5 11.5 h-2.75 v-6 h12.5 v3"), R(4.5, 9, 6, 5.25, 0.5), C(13, 12.5, 1.5, fill=True), S("M13 14 v1.25", w=1)]
G["printer-printing"] = G["printer"] + [S("M6.25 11 h3.5 M6.25 12.75 h2.5", w=1)]
G["printer-warning"] = [S("M4.5 5.5 v-3.75 h7 v3.75 M4.5 11.5 h-2.75 v-6 h5.5"), R(4.5, 9, 3, 5.25, 0.5), F("M11.5 6.5 l4 7.5 h-8 z"),
                        S("M11.5 9 v2.25", w=1.25)]
G["printer-error"] = G["printer-warning"]
G["scanner"] = [R(1.75, 8, 12.5, 5.5, 1), S("M1.75 8 l11 -5.5 M4 11 h3")]
G["modem"] = [R(1.75, 8.5, 12.5, 5, 1), S("M4 11 h0.5 M6.5 11 h0.5 M9 11 h0.5", w=1.5), S("M11 8.5 l2 -6")]
G["multimedia-player"] = [R(3.5, 1.25, 9, 13.5, 1.75), C(8, 10, 2.25), R(5.25, 3, 5.5, 3.5, 0.5, fill=True)]
G["battery"] = battery(0.75)
G["bluetooth"] = [S("M4.5 4.75 l7 6.25 l-3.5 3 v-12 l3.5 3 l-7 6.25")]
G["bluetooth-active"] = G["bluetooth"]
G["bluetooth-disabled"] = [S("M4.5 4.75 l7 6.25 l-3.5 3 v-12 l3.5 3 l-7 6.25", extra=' stroke-opacity="0.4"'),
                           S("M2 2 l12 12", w=1.75)]
G["network-wired"] = [R(5.25, 1.75, 5.5, 4.25, 0.75), S("M8 6 v3 M3 14.25 v-5.25 h10 v5.25 M8 9 v5.25")]
G["network-wired-disconnected"] = [R(5.25, 1.75, 5.5, 4.25, 0.75), S("M8 6 v2 M3 14.25 v-4.5 M13 14.25 v-4.5"),
                                   S("M2 8 l12 4", w=1.25)]
G["network-wireless"] = signal_bars(4)
G["network-wireless-signal-excellent"] = signal_bars(4)
G["network-wireless-signal-good"] = signal_bars(3)
G["network-wireless-signal-ok"] = signal_bars(2)
G["network-wireless-signal-weak"] = signal_bars(1)
G["network-wireless-signal-none"] = signal_bars(0)
G["network-wireless-offline"] = signal_bars(0) + [S("M2 2 l12 12", w=1.75)]
G["network-wireless-encrypted"] = signal_bars(4)[:3] + [R(9.5, 10, 5.5, 4.5, 0.75, fill=True), S("M10.75 10 v-1 a1.5 1.5 0 0 1 3 0 v1", w=1.25)]
G["network-offline"] = [C(8, 8, 6.25), S("M3.5 12.5 l9 -9")]
G["network-transmit-receive"] = [S("M5 13.5 v-10 M2 6.5 l3 -3 l3 3 M11 2.5 v10 M8 9.5 l3 3 l3 -3")]
G["network-vpn"] = LOCK + [C(8, 10.75, 1, fill=False, w=1)]
G["preferences-system-network"] = G["network-workgroup"]
G["airplane-mode"] = [F("M7 1.5 h2 v4.5 l5.5 3.5 v1.75 l-5.5 -1.75 v3 l1.75 1.25 v1.25 l-2.75 -0.75 l-2.75 0.75 v-1.25 "
                        "l1.75 -1.25 v-3 l-5.5 1.75 v-1.75 l5.5 -3.5 z")]

# mimetypes
G["text-x-generic"] = [SHEET, S("M5.5 8 h5 M5.5 10.5 h5 M5.5 13 h3", w=1.25)]
G["text-x-script"] = [SHEET, S("M6.5 8 l-1.75 1.75 l1.75 1.75 M9.5 8 l1.75 1.75 l-1.75 1.75", w=1.25)]
G["text-html"] = [C(8, 8, 6.25), S("M1.75 8 h12.5 M8 1.75 c-2.75 3.5 -2.75 9 0 12.5 c2.75 -3.5 2.75 -9 0 -12.5", w=1.25)]
G["text-x-preview"] = G["text-x-generic"]
G["application-x-executable"] = [R(1.75, 2.25, 12.5, 11.5, 1.5), S("M1.75 5.25 h12.5"), S("M4.5 8 l2 1.75 l-2 1.75 M8 11.5 h3.5", w=1.25)]
G["application-x-generic"] = [SHEET, R(5.5, 8, 5, 4.5, 0.75, fill=True)]
G["application-x-sharedlib"] = [SHEET, C(8, 10, 2), S("M8 7 v1 M8 12 v1 M5 10 h1 M10 10 h1", w=1.25)]
G["application-pdf"] = [SHEET, S("M5 12.5 c2 -1 3.5 -4.5 3 -5.5 c-0.5 -1 -1.5 0.5 -0.5 2.5 c1 2 2.75 2.75 3.5 2.25 c0.75 -0.5 -1.5 -1 -6 0.75", w=1)]
G["package-x-generic"] = [S("M8 1.5 l6 3 v7 l-6 3 l-6 -3 v-7 z M2 4.5 l6 3 l6 -3 M8 7.5 v7 M5 3 l6 3")]
G["application-x-archive"] = [SHEET, S("M7.5 1.75 v1 M8.5 3.25 v1 M7.5 4.75 v1 M8.5 6.25 v1", w=1), R(6.5, 8, 3, 3.5, 0.75)]
G["application-x-addon"] = [S("M3 4 h3 a2 2 0 0 1 4 0 h3 v3 a2 2 0 0 1 0 4 v3 h-10 z")]
G["image-x-generic"] = G["folder-pictures"]
G["audio-x-generic"] = G["folder-music"]
G["video-x-generic"] = G["folder-videos"]
G["font-x-generic"] = [S("M2.5 14 l4.5 -12 l4.5 12 M4.25 9.5 h5.5"), S("M11.5 10 a1.75 1.75 0 1 0 3.5 0 a1.75 1.75 0 1 0 -3.5 0 M15 8.25 v3.75", w=1.25)]
G["x-office-document"] = G["text-x-generic"]
G["x-office-spreadsheet"] = [SHEET, S("M5 7.5 h6 v5.5 h-6 z M5 10.25 h6 M8 7.5 v5.5", w=1)]
G["x-office-presentation"] = [R(1.75, 2.25, 12.5, 8.75, 1), S("M8 11 v3 M5.5 14.25 h5"), R(4, 4.75, 4, 3.5, 0.5, fill=True)]
G["x-office-drawing"] = [SHEET, C(7, 10, 1.75), S("M8.5 7 l2.5 0 l-1.25 2.5 z", w=1)]
G["x-office-calendar"] = [R(1.75, 3, 12.5, 11.25, 1.5), S("M1.75 6.5 h12.5 M5 1.5 v3 M11 1.5 v3"), S("M4.5 9 h1 M7.5 9 h1 M10.5 9 h1 M4.5 11.75 h1 M7.5 11.75 h1", w=1.5)]
G["x-office-address-book"] = [R(2.5, 1.75, 11, 12.5, 1.25), C(8, 6.5, 1.75), S("M5 11.75 a3 3 0 0 1 6 0")]
G["inode-symlink"] = [S("M6.5 9.5 a2.5 2.5 0 0 1 0 -3.5 l2 -2 a2.5 2.5 0 0 1 3.5 3.5 l-1 1 M9.5 6.5 a2.5 2.5 0 0 1 0 3.5 l-2 2 a2.5 2.5 0 0 1 -3.5 -3.5 l1 -1")]
G["insert-link"] = G["inode-symlink"]
G["emblem-symbolic-link"] = [S("M5 11 l6 -6 M6.5 4.75 h4.75 v4.75", w=2)]
G["unknown"] = [SHEET, S("M6.5 8.25 a1.5 1.5 0 1 1 2 1.4 c-0.4 0.2 -0.5 0.5 -0.5 1", w=1.25), C(8, 12.75, 0.75, fill=True)]

# actions
G["document-new"] = [S("M3.5 1.75 h6 l3 3 v3 M9.5 1.75 v3 h3 M3.5 1.75 v12.5 h5"), S("M12 10 v4.5 M9.75 12.25 h4.5")]
G["document-open"] = G["folder-open"]
G["document-save"] = [S("M2 2 h9.5 l2.5 2.5 v9.5 h-12 z"), R(4.5, 2, 6, 3.75, 0.5), R(4, 9, 8, 5, 0.5)]
G["document-save-as"] = [S("M2 2 h9.5 l2.5 2.5 v3 M2 2 v12 h5.5"), R(4.5, 2, 6, 3.75, 0.5), S("M9 14.5 l0.5 -2 l4 -4 l1.5 1.5 l-4 4 z", w=1.25)]
G["document-print"] = G["printer"]
G["document-properties"] = [SHEET, S("M5.5 8 h1 M8 8 h2.5 M5.5 10.5 h1 M8 10.5 h2.5 M5.5 13 h1 M8 13 h2.5", w=1.25)]
G["document-revert"] = [S("M3.5 6.5 a5 5 0 1 1 1 5"), S("M2.5 2.5 v4 h4")]
G["edit-undo"] = [S("M5.5 9.5 l-3.5 -3.5 l3.5 -3.5 M2 6 h7.5 a4.25 4.25 0 0 1 0 8.5 h-3")]
G["edit-redo"] = [S("M10.5 9.5 l3.5 -3.5 l-3.5 -3.5 M14 6 h-7.5 a4.25 4.25 0 0 0 0 8.5 h3")]
G["document-send"] = [S("M1.5 7.5 l13 -5.5 l-4 12.5 l-3 -5 z M7.5 9.5 l7 -7.5")]
G["mail-send"] = G["document-send"]
G["mail-message-new"] = [R(1.75, 3.5, 12.5, 9, 1), S("M1.75 4.5 l6.25 5 l6.25 -5")]
G["mail-unread"] = G["mail-message-new"]
G["internet-mail"] = G["mail-message-new"]
G["document-edit"] = [S("M2.5 13.5 l0.75 -3 l8 -8 l2.25 2.25 l-8 8 z M9.5 4 l2.25 2.25")]
G["accessories-text-editor"] = G["document-edit"]
G["edit-copy"] = [R(5.5, 5.5, 8.75, 8.75, 1.25), S("M3 10.5 h-1.25 v-8.75 h8.75 v1.25")]
G["edit-cut"] = [C(4.25, 11.75, 2.25), C(11.75, 11.75, 2.25), S("M5.75 10 l6.75 -8.25 M10.25 10 l-6.75 -8.25")]
G["edit-paste"] = [S("M5.5 2.75 h-3 v11.5 h11 v-11.5 h-3"), R(5.5, 1.5, 5, 2.75, 0.75), S("M5.5 8 h5 M5.5 10.75 h5", w=1.25)]
G["edit-delete"] = G["user-trash"]
G["edit-find"] = MAGNIFIER
G["system-search"] = MAGNIFIER
G["edit-find-replace"] = [C(6, 6, 3.75), S("M8.75 8.75 l2 2"), S("M10 14 l4 -4 M11.5 9.25 h2.75 v2.75", w=1.25)]
G["edit-select-all"] = [S("M2 4 v-2 h2 M7 2 h2 M12 2 h2 v2 M14 7 v2 M14 12 v2 h-2 M9 14 h-2 M4 14 h-2 v-2 M2 9 v-2"), R(5, 5, 6, 6, 0.75, fill=True)]
G["edit-clear"] = [S("M5.5 3 h8.5 v10 h-8.5 l-4.5 -5 z M7.5 5.75 l4.5 4.5 M12 5.75 l-4.5 4.5")]
G["edit-clear-all"] = [S("M3 3 l10 10 M13 3 l-10 10", w=2)]
G["list-add"] = [S("M8 2.5 v11 M2.5 8 h11", w=2)]
G["list-remove"] = [S("M2.5 8 h11", w=2)]
G["go-up"] = [S("M3 10.5 l5 -5 l5 5", w=2)]
G["go-down"] = [S("M3 5.5 l5 5 l5 -5", w=2)]
G["go-previous"] = [S("M10.5 3 l-5 5 l5 5", w=2)]
G["go-next"] = [S("M5.5 3 l5 5 l-5 5", w=2)]
G["go-first"] = [S("M11.5 3 l-5 5 l5 5 M3.5 3 v10", w=2)]
G["go-last"] = [S("M4.5 3 l5 5 l-5 5 M12.5 3 v10", w=2)]
G["go-top"] = [S("M3 11.5 l5 -5 l5 5 M3 3.5 h10", w=2)]
G["go-bottom"] = [S("M3 4.5 l5 5 l5 -5 M3 12.5 h10", w=2)]
G["go-jump"] = [S("M2 12.5 c0 -6 5 -8.5 11 -8.5 M10 1 l3 3 l-3 3")]
G["view-refresh"] = [S("M13.25 8 a5.25 5.25 0 1 1 -1.5 -3.7"), F("M14.5 1.5 v5 h-5 z")]
G["view-fullscreen"] = [S("M2 6 v-4 h4 M10 2 h4 v4 M14 10 v4 h-4 M6 14 h-4 v-4")]
G["view-restore"] = [S("M6 2 v4 h-4 M10 2 v4 h4 M14 10 h-4 v4 M2 10 h4 v4")]
G["view-list"] = [S("M5.5 3.5 h8.5 M5.5 8 h8.5 M5.5 12.5 h8.5"), C(2.5, 3.5, 1, fill=True), C(2.5, 8, 1, fill=True), C(2.5, 12.5, 1, fill=True)]
G["view-grid"] = [R(1.75, 1.75, 5, 5, 1), R(9.25, 1.75, 5, 5, 1), R(1.75, 9.25, 5, 5, 1), R(9.25, 9.25, 5, 5, 1)]
G["view-app-grid"] = [C(x, y, 1.6, fill=True) for y in (3, 8, 13) for x in (3, 8, 13)]
G["view-sort-ascending"] = [S("M4 13.5 v-11 M1.5 5 l2.5 -2.5 l2.5 2.5 M9 3.5 h2 M9 8 h3.5 M9 12.5 h5")]
G["view-sort-descending"] = [S("M4 2.5 v11 M1.5 11 l2.5 2.5 l2.5 -2.5 M9 3.5 h5 M9 8 h3.5 M9 12.5 h2")]
G["view-more"] = [C(3, 8, 1.5, fill=True), C(8, 8, 1.5, fill=True), C(13, 8, 1.5, fill=True)]
G["view-more-horizontal"] = G["view-more"]
G["open-menu"] = [S("M2.5 4 h11 M2.5 8 h11 M2.5 12 h11", w=1.75)]
G["view-reveal"] = [S("M1.25 8 c3.75 -6 9.75 -6 13.5 0 c-3.75 6 -9.75 6 -13.5 0 z"), C(8, 8, 2.25, fill=True)]
G["view-conceal"] = [S("M1.25 8 c3.75 -6 9.75 -6 13.5 0 c-3.75 6 -9.75 6 -13.5 0 z"), S("M2.5 13.5 l11 -11")]
G["view-pin"] = [S("M6 1.75 h4 M7 1.75 v4.5 l-3 3 h8 l-3 -3 v-4.5 M8 9.25 v5")]
G["zoom-in"] = MAGNIFIER + [S("M6.75 4.75 v4 M4.75 6.75 h4", w=1.25)]
G["zoom-out"] = MAGNIFIER + [S("M4.75 6.75 h4", w=1.25)]
G["zoom-original"] = MAGNIFIER + [S("M6.25 5 h0.75 v3.75", w=1.25)]
G["zoom-fit-best"] = [R(1.75, 1.75, 12.5, 12.5, 1.25), S("M5 8 h6 M5 8 l1.5 -1.5 M5 8 l1.5 1.5 M11 8 l-1.5 -1.5 M11 8 l-1.5 1.5", w=1.25)]
G["window-close"] = [S("M4 4 l8 8 M12 4 l-8 8", w=1.75)]
G["window-maximize"] = [R(3, 3, 10, 10, 1.5)]
G["window-minimize"] = [S("M3.5 12 h9", w=2)]
G["window-restore"] = [R(2.5, 5.5, 8, 8, 1.25), S("M5.5 5.5 v-3 h8 v8 h-3")]
G["window-new"] = [R(1.75, 2.75, 10, 9, 1), S("M1.75 5.25 h10 M13 9.5 v5 M10.5 12 h5")]
G["tab-new"] = [S("M1.5 13 h13 M2.5 13 v-8.5 h6 v8.5"), S("M12 3 v5 M9.5 5.5 h5")]
G["application-exit"] = [S("M6 2 h-4 v12 h4 M6.5 8 h8 M11.5 5 l3 3 l-3 3")]
G["system-log-out"] = G["application-exit"]
G["system-shutdown"] = [S("M8 1.75 v6", w=1.75), S("M4.5 3.75 a5.5 5.5 0 1 0 7 0", w=1.75)]
G["system-reboot"] = [S("M13.25 8 a5.25 5.25 0 1 1 -1.5 -3.7"), F("M14.5 1.5 v5 h-5 z")]
G["system-restart"] = G["system-reboot"]
G["system-suspend"] = [F("M9 1.5 a6.5 6.5 0 1 0 5.5 9.5 a5.25 5.25 0 0 1 -5.5 -9.5 z")]
G["weather-clear-night"] = G["system-suspend"]
G["system-lock-screen"] = LOCK
G["changes-prevent"] = LOCK
G["changes-allow"] = [R(3, 7, 10, 7.5, 1.5, fill=True), S("M5 7 v-2 a3 3 0 0 1 5.75 -1.25")]
G["dialog-password"] = [C(5.5, 10.5, 3.25), S("M7.75 8.25 l6 -6 M11 5 l2 2 M12.5 3.5 l1.5 1.5")]
G["system-run"] = [S("M2.5 4.5 l4 3.5 l-4 3.5 M8 12.5 h5.5", w=1.75)]
G["utilities-terminal"] = [R(1.75, 2.25, 12.5, 11.5, 1.5), S("M4.5 6 l2.25 2 l-2.25 2 M8.5 10.5 h3", w=1.25)]
G["process-stop"] = [S("M5.25 1.75 h5.5 l3.5 3.5 v5.5 l-3.5 3.5 h-5.5 l-3.5 -3.5 v-5.5 z M5.5 5.5 l5 5 M10.5 5.5 l-5 5")]
G["media-playback-start"] = [PLAY]
G["media-playback-pause"] = [R(3.5, 2.5, 3, 11, 0.75, fill=True), R(9.5, 2.5, 3, 11, 0.75, fill=True)]
G["media-playback-stop"] = [R(3, 3, 10, 10, 1, fill=True)]
G["media-record"] = [C(8, 8, 5, fill=True)]
G["media-skip-forward"] = [F("M2 3 l7 5 l-7 5 z"), R(10.5, 3, 2.25, 10, 0.5, fill=True)]
G["media-skip-backward"] = [F("M14 3 l-7 5 l7 5 z"), R(3.25, 3, 2.25, 10, 0.5, fill=True)]
G["media-seek-forward"] = [F("M1.5 3.5 l6.5 4.5 l-6.5 4.5 z M8 3.5 l6.5 4.5 l-6.5 4.5 z")]
G["media-seek-backward"] = [F("M14.5 3.5 l-6.5 4.5 l6.5 4.5 z M8 3.5 l-6.5 4.5 l6.5 4.5 z")]
G["media-playlist-repeat"] = [S("M2.5 7.5 v-1.5 a2 2 0 0 1 2 -2 h8.5 M11 2 l2 2 l-2 2 M13.5 8.5 v1.5 a2 2 0 0 1 -2 2 h-8.5 M5 10 l-2 2 l2 2")]
G["media-playlist-shuffle"] = [S("M1.5 4 h2.5 c3.5 0 4.5 8 8 8 h2 M12.5 10 l2 2 l-2 2 M1.5 12 h2.5 c1.25 0 2 -1 2.75 -2.25 "
                                 "M9.25 6.25 c0.75 -1.25 1.5 -2.25 2.75 -2.25 h2 M12.5 2 l2 2 l-2 2")]
G["format-text-bold"] = [S("M4 2 h4.5 a3 3 0 0 1 0 6 h-4.5 z M4 8 h5.5 a3 3 0 0 1 0 6 h-5.5 z", w=2)]
G["format-text-italic"] = [S("M6.5 2 h6 M3.5 14 h6 M9.75 2 l-3.5 12", w=1.75)]
G["format-text-underline"] = [S("M4 2 v5.5 a4 4 0 0 0 8 0 v-5.5 M3 14.25 h10", w=1.75)]
G["format-text-strikethrough"] = [S("M11.5 4 a3.5 2.5 0 0 0 -7 0.5 c0 1.5 1.5 2 3.5 2.5 M2.5 8 h11 M4.5 11.5 a3.5 2.5 0 0 0 7 0 c0 -0.75 -0.3 -1.2 -0.8 -1.5")]
G["format-justify-left"] = [S("M2 3 h12 M2 6.33 h8 M2 9.66 h12 M2 13 h8")]
G["format-justify-center"] = [S("M2 3 h12 M4 6.33 h8 M2 9.66 h12 M4 13 h8")]
G["format-justify-right"] = [S("M2 3 h12 M6 6.33 h8 M2 9.66 h12 M6 13 h8")]
G["format-justify-fill"] = [S("M2 3 h12 M2 6.33 h12 M2 9.66 h12 M2 13 h12")]
G["format-indent-more"] = [S("M6 3 h8 M6 6.33 h8 M6 9.66 h8 M2 13 h12 M2 4.5 l2.5 2 l-2.5 2")]
G["format-indent-less"] = [S("M6 3 h8 M6 6.33 h8 M6 9.66 h8 M2 13 h12 M4.5 4.5 l-2.5 2 l2.5 2")]
G["insert-image"] = G["folder-pictures"]
G["insert-text"] = [S("M3 4 v-1.5 h10 v1.5 M8 2.5 v11 M6 13.5 h4", w=1.75)]
G["object-rotate-left"] = [S("M3.5 7 a5 5 0 1 1 1.5 5"), F("M1.5 3.5 v5 h5 z")]
G["object-rotate-right"] = [S("M12.5 7 a5 5 0 1 0 -1.5 5"), F("M14.5 3.5 v5 h-5 z")]
G["object-flip-horizontal"] = [F("M6.5 3 v10 l-5 0 z"), S("M9.5 3 v10 h5 z M8 1 v14", w=1.25)]
G["object-flip-vertical"] = [F("M3 6.5 h10 l0 -5 z"), S("M3 9.5 h10 v5 z M1 8 h14", w=1.25)]
G["starred"] = [F("M8 1.25 l2 4.25 l4.75 0.6 l-3.5 3.25 l0.9 4.65 l-4.15 -2.3 l-4.15 2.3 l0.9 -4.65 l-3.5 -3.25 l4.75 -0.6 z")]
G["emblem-favorite"] = [F("M8 14 l-5.25 -5.25 a3.25 3.25 0 0 1 5.25 -4.25 a3.25 3.25 0 0 1 5.25 4.25 z")]
G["non-starred"] = [S("M8 1.25 l2 4.25 l4.75 0.6 l-3.5 3.25 l0.9 4.65 l-4.15 -2.3 l-4.15 2.3 l0.9 -4.65 l-3.5 -3.25 l4.75 -0.6 z", w=1.25)]
G["call-start"] = [S("M3 1.75 h3 l1.25 3.25 l-2 1.25 a8 8 0 0 0 4.5 4.5 l1.25 -2 l3.25 1.25 v3 a1.5 1.5 0 0 1 -1.5 1.5 a12 12 0 0 1 -11.25 -11.25 a1.5 1.5 0 0 1 1.5 -1.5 z")]
G["help-about"] = [C(8, 8, 6.25), S("M8 7.25 v4.25"), C(8, 4.75, 1, fill=True)]
G["dialog-information"] = G["help-about"]
G["help-contents"] = [C(8, 8, 6.25), S("M6 6.25 a2 2 0 1 1 2.75 1.85 c-0.5 0.25 -0.75 0.6 -0.75 1.15 v0.5"), C(8, 11.75, 0.95, fill=True)]
G["help-browser"] = G["help-contents"]
G["dialog-question"] = G["help-contents"]
G["help-faq"] = G["help-contents"]
G["dialog-warning"] = [S("M8 1.75 l6.75 12 h-13.5 z"), S("M8 6 v3.5"), C(8, 11.75, 0.95, fill=True)]
G["dialog-error"] = [C(8, 8, 6.25), S("M5.5 5.5 l5 5 M10.5 5.5 l-5 5")]
G["emblem-important"] = G["dialog-warning"]
G["emblem-ok"] = [C(8, 8, 6.25), S("M5 8.25 l2 2 l4 -4.25")]
G["object-select"] = [CHECK]
G["emblem-default"] = G["emblem-ok"]
G["emblem-shared"] = G["folder-publicshare"]
G["emblem-readonly"] = LOCK
G["emblem-system"] = GEAR
G["emblem-synchronizing"] = G["media-playlist-repeat"]
G["process-working"] = [S("M8 1.75 a6.25 6.25 0 1 1 -6.25 6.25", w=2), S("M8 1.75 a6.25 6.25 0 0 0 -6.25 6.25", w=2, extra=' stroke-opacity="0.3"')]
G["image-loading"] = G["process-working"]
G["image-missing"] = [R(1.75, 1.75, 12.5, 12.5, 1.25), S("M4.5 4.5 l7 7 M11.5 4.5 l-7 7")]
G["security-high"] = [S("M8 1.5 l5.75 2.25 v4 c0 3.5 -2.5 6 -5.75 7 c-3.25 -1 -5.75 -3.5 -5.75 -7 v-4 z"), S("M5.25 8 l2 2 l3.5 -3.75")]
G["security-medium"] = [S("M8 1.5 l5.75 2.25 v4 c0 3.5 -2.5 6 -5.75 7 c-3.25 -1 -5.75 -3.5 -5.75 -7 v-4 z"), F("M8 1.5 l5.75 2.25 v4 c0 3.5 -2.5 6 -5.75 7 z")]
G["security-low"] = [S("M8 1.5 l5.75 2.25 v4 c0 3.5 -2.5 6 -5.75 7 c-3.25 -1 -5.75 -3.5 -5.75 -7 v-4 z"), S("M8 5 v3.75"), C(8, 11, 0.9, fill=True)]
G["preferences-system-privacy"] = G["security-high"]
G["audio-volume-high"] = volume(3)
G["audio-volume-medium"] = volume(2)
G["audio-volume-low"] = volume(1)
G["audio-volume-muted"] = volume(-1)
G["microphone-sensitivity-muted"] = G["audio-input-microphone"] + [S("M2 2 l12 12", w=1.75)]
G["battery-full"] = battery(1)
G["battery-good"] = battery(0.7)
G["battery-low"] = battery(0.3)
G["battery-caution"] = battery(0.12)
G["battery-empty"] = battery(0)
G["battery-full-charging"] = battery(1, True)
G["battery-good-charging"] = battery(0.7, True)
G["battery-low-charging"] = battery(0.3, True)
G["battery-missing"] = [R(1.75, 4.5, 11, 7, 1.25), R(13, 6.5, 1.5, 3, 0.5, fill=True), S("M5.5 6.5 l3 3 M8.5 6.5 l-3 3", w=1.25)]
G["notifications"] = BELL
G["preferences-system-notifications"] = BELL
G["notifications-disabled"] = BELL + [S("M2 2 l12 12", w=1.75)]
G["display-brightness"] = [C(8, 8, 3), S("M8 1 v1.5 M8 13.5 v1.5 M1 8 h1.5 M13.5 8 h1.5 M3 3 l1 1 M12 12 l1 1 M3 13 l1 -1 M12 4 l1 -1")]
G["weather-clear"] = G["display-brightness"]
CLOUD = "M4.5 12.5 h7 a2.75 2.75 0 0 0 0.3 -5.5 a3.75 3.75 0 0 0 -7.2 1.05 a2.25 2.25 0 0 0 -0.1 4.45 z"
CLOUD_HI = "M4.5 9.75 h7 a2.75 2.75 0 0 0 0.3 -5.5 a3.75 3.75 0 0 0 -7.2 1.05 a2.25 2.25 0 0 0 -0.1 4.45 z"
SMALL_CLOUD = "M6.5 14 h6 a2.4 2.4 0 0 0 0.25 -4.8 a3.2 3.2 0 0 0 -6.15 0.9 a1.95 1.95 0 0 0 -0.1 3.9 z"
G["weather-overcast"] = [S(CLOUD)]
G["weather-few-clouds"] = [C(5.5, 5.5, 2.25), S("M5.5 1 v1 M1 5.5 h1 M2.3 2.3 l0.75 0.75 M8.7 2.3 l-0.75 0.75 M2.3 8.7 l0.75 -0.75"),
                           S(SMALL_CLOUD)]
G["weather-few-clouds-night"] = [S("M6 1.75 a4 4 0 1 0 3.5 6 a3.25 3.25 0 0 1 -3.5 -6 z"), S(SMALL_CLOUD)]
G["weather-fog"] = [S("M2 5 h12 M3.5 8 h9 M2 11 h12", w=1.5)]
G["weather-showers"] = [S(CLOUD_HI), S("M5.5 11.75 l-1 2.5 M8.5 11.75 l-1 2.5 M11.5 11.75 l-1 2.5")]
G["weather-snow"] = [S(CLOUD_HI), C(5, 12.75, 0.9, fill=True), C(8, 14, 0.9, fill=True), C(11, 12.75, 0.9, fill=True)]
G["weather-storm"] = [S(CLOUD_HI), F("M8.75 10.25 l-2.75 3.25 h2.25 l-1 2.25 l3.25 -3.5 h-2.25 l1.25 -2 z")]
G["weather-severe-alert"] = G["dialog-warning"]
G["user-available"] = [C(8, 8, 5.5, fill=True)]
G["user-offline"] = [C(8, 8, 5.25)]
G["user-busy"] = [C(8, 8, 6, fill=True), ]
G["avatar-default"] = PERSON
G["system-users"] = [C(5.5, 5, 2.25), S("M1.25 13.5 a4.25 4.25 0 0 1 8.5 0"), C(11.25, 5.5, 1.9), S("M10.5 9.5 a3.75 3.75 0 0 1 4.5 4")]
G["contact-new"] = PERSON[:1] + [S("M2.75 14.25 a5.25 5.25 0 0 1 7.5 -4.75 M12.5 9.5 v5 M10 12 h5")]
G["preferences-system-time"] = [C(8, 8, 6.25), S("M8 4.25 v4 l2.75 1.75")]
G["alarm"] = G["preferences-system-time"]
G["mark-location"] = [S("M8 14.5 c-3 -3.5 -5 -6 -5 -8.5 a5 5 0 0 1 10 0 c0 2.5 -2 5 -5 8.5 z"), C(8, 6, 1.75, fill=True)]
G["find-location"] = G["mark-location"]
G["preferences-desktop-locale"] = [S("M2 3 h7 M5.5 1.5 v1.5 M3.5 3 c0.5 3 2.5 5 5 6 M7.5 3 c-0.5 3 -2.5 5 -5 6"),
                                   S("M8 14.5 l3 -7 l3 7 M9 12.25 h4")]
G["preferences-desktop-keyboard"] = G["input-keyboard"]
G["preferences-desktop-keyboard-shortcuts"] = [R(1, 3.75, 14, 8.5, 1.5), S("M3.5 6.5 h1.5 M7.25 6.5 h1.5 M11 6.5 h1.5 M4 9.5 h8", w=1.5)]
G["preferences-desktop-display"] = MONITOR
G["preferences-desktop-wallpaper"] = G["folder-pictures"]
G["preferences-desktop-theme"] = [S("M8 1.75 a6.25 6.25 0 1 0 0 12.5 c1 0 1.25 -0.75 0.75 -1.5 c-0.75 -1 0 -2.25 1.25 -2.25 h1.75 "
                                    "a2.5 2.5 0 0 0 2.5 -2.5 c0 -3.5 -2.75 -6.25 -6.25 -6.25 z"),
                                  C(4.75, 7.5, 1, fill=True), C(6.5, 4.5, 1, fill=True), C(10, 4.5, 1, fill=True)]
G["preferences-color"] = G["preferences-desktop-theme"]
G["preferences-desktop-font"] = [S("M2 13.5 l4.25 -11 l4.25 11 M3.5 9.5 h5.5"), S("M10.75 9 h4 M12.75 9 v4.5", w=1.25)]
G["preferences-desktop-accessibility"] = [C(8, 2.75, 1.4, fill=True), S("M2.5 5.5 l5.5 1.25 l5.5 -1.25 M8 6.75 v3 l-2.5 4.75 M8 9.75 l2.5 4.75")]
G["preferences-desktop-screensaver"] = LOCK
G["preferences-desktop"] = [R(1.75, 2.25, 12.5, 8.5, 1), S("M5 14 h6 M8 10.75 v3.25"), R(3.5, 4, 4, 2, 0.4, fill=True)]
G["preferences-desktop-default-applications"] = [S("M3 1.75 h10 a1.25 1.25 0 0 1 1.25 1.25 v10 a1.25 1.25 0 0 1 -1.25 1.25 h-10 "
                                                   "a1.25 1.25 0 0 1 -1.25 -1.25 v-10 a1.25 1.25 0 0 1 1.25 -1.25 z"), CHECK]
G["preferences-system"] = GEAR
G["preferences-other"] = GEAR
G["applications-system"] = GEAR
G["system-software-install"] = [S("M2.5 5 h11 l-1 9.25 h-9 z M5.5 5 v-0.75 a2.5 2.5 0 0 1 5 0 v0.75"), S("M8 7.5 v4.5 M6 10 l2 2 l2 -2", w=1.25)]
G["system-software-update"] = [C(8, 8, 6.25), S("M8 11.5 v-7 M5 7.5 l3 -3 l3 3")]
G["software-update-available"] = G["system-software-update"]
G["system-file-manager"] = G["folder"]
G["utilities-system-monitor"] = [R(1.75, 2.25, 12.5, 11.5, 1.5), S("M1.75 9 h3 l1.5 -3.5 l2.5 6 l1.5 -2.5 h4", w=1.25)]
G["accessories-calculator"] = [R(3, 1.75, 10, 12.5, 1.5), R(4.75, 3.5, 6.5, 2.5, 0.4, fill=True),
                               S("M5.5 8.5 h0.5 M7.75 8.5 h0.5 M10 8.5 h0.5 M5.5 11 h0.5 M7.75 11 h0.5 M10 11 h0.5", w=1.75)]
G["accessories-character-map"] = [S("M2 14 l4 -11 l4 11 M3.25 10.5 h5.5"), S("M11 6 h3.5 M11 9 h3.5 M11 12 h3.5", w=1.25)]
G["accessories-screenshot"] = G["camera-photo"]
G["web-browser"] = G["text-html"]
G["applications-internet"] = G["text-html"]
G["applications-accessories"] = [S("M3 13 l6.5 -6.5 M9 2.5 a3 3 0 0 0 4.5 4 l-1.5 -1.5 l0.5 -2 z", w=1.75)]
G["applications-utilities"] = G["applications-accessories"]
G["applications-development"] = [S("M5 4 l-4 4 l4 4 M11 4 l4 4 l-4 4 M9.25 2.5 l-2.5 11", w=1.5)]
G["applications-engineering"] = [S("M2 14 l3 -7 h6 l3 7 M5 7 l3 -5 l3 5 M4 11 h8")]
G["applications-games"] = G["input-gaming"]
G["applications-graphics"] = [S("M2.5 13.5 c0 -2 1 -3 2.5 -3 a1.75 1.75 0 0 1 0 3.5 z M6 9.5 l7.5 -7.5 l1 1 l-7.5 7.5")]
G["applications-multimedia"] = [C(8, 8, 6.25), F("M6.25 5 v6 l5 -3 z")]
G["applications-office"] = [S("M2 4.5 h12 v9 h-12 z M5.5 4.5 v-2 h5 v2 M2 8.5 h12")]
G["applications-science"] = [S("M6 1.75 h4 M6.75 1.75 v4.5 l-4 7 a1 1 0 0 0 1 1.5 h8.5 a1 1 0 0 0 1 -1.5 l-4 -7 v-4.5 M4.5 10 h7")]
G["applications-education"] = [S("M1 6 l7 -3.5 l7 3.5 l-7 3.5 z M3.75 7.5 v3.5 c2.5 2 6 2 8.5 0 v-3.5 M14 6.5 v4")]
G["applications-other"] = G["view-app-grid"]
G["multimedia-video-player"] = G["applications-multimedia"]
G["audio-player"] = G["folder-music"]
G["image-viewer"] = G["folder-pictures"]
G["system-help"] = G["help-contents"]
G["dialog-cancel"] = [CROSS]


# Alternative names that should resolve to the same glyph.
ALIASES = {
    "inode-directory": "folder",
    "text-plain": "text-x-generic",
    "text-x-csrc": "text-x-script",
    "text-x-python": "text-x-script",
    "application-x-shellscript": "text-x-script",
    "application-x-sh": "text-x-script",
    "application-javascript": "text-x-script",
    "application-json": "text-x-script",
    "application-xml": "text-x-script",
    "application-octet-stream": "application-x-generic",
    "application-zip": "application-x-archive",
    "application-x-compressed-tar": "application-x-archive",
    "application-x-tar": "application-x-archive",
    "application-x-7z-compressed": "application-x-archive",
    "application-x-rpm": "package-x-generic",
    "application-x-deb": "package-x-generic",
    "application-vnd.debian.binary-package": "package-x-generic",
    "application-x-desktop": "application-x-executable",
    "application-vnd.oasis.opendocument.text": "x-office-document",
    "application-vnd.oasis.opendocument.spreadsheet": "x-office-spreadsheet",
    "application-vnd.oasis.opendocument.presentation": "x-office-presentation",
    "image-png": "image-x-generic",
    "image-jpeg": "image-x-generic",
    "image-svg+xml": "image-x-generic",
    "audio-mpeg": "audio-x-generic",
    "video-mp4": "video-x-generic",
    "network-wireless-connected": "network-wireless",
    "network-wireless-acquiring": "network-wireless-signal-weak",
    "network-wired-acquiring": "network-wired",
    "network-wired-offline": "network-wired-disconnected",
    "network-error": "network-offline",
    "network-idle": "network-transmit-receive",
    "system-switch-user": "system-users",
    "system-hibernate": "system-suspend",
    "gnome-power-manager": "battery",
    "preferences-desktop-peripherals": "input-mouse",
    "preferences-system-search": "system-search",
    "preferences-desktop-online-accounts": "system-users",
    "preferences-system-sharing": "folder-publicshare",
    "preferences-system-devices": "computer",
    "printer-remote": "printer-network",
    "document-open-recent": "preferences-system-time",
    "document-page-setup": "document-properties",
    "document-print-preview": "zoom-in",
    "edit-select": "edit-select-all",
    "mail-reply-sender": "edit-undo",
    "mail-forward": "edit-redo",
    "go-down-search": "go-down",
    "go-up-search": "go-up",
    "pan-down": "go-down",
    "pan-up": "go-up",
    "pan-start": "go-previous",
    "pan-end": "go-next",
    "list-drag-handle": "open-menu",
    "selection-mode": "object-select",
    "user-info": "avatar-default",
    "user-idle": "user-available",
    "user-away": "user-offline",
    "media-tape": "drive-harddisk",
    "drive-multidisk": "drive-harddisk",
    "media-zip": "media-removable",
    "multimedia-volume-control": "audio-speakers",
}
