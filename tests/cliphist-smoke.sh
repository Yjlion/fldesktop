#!/bin/sh
# Starts fl-wm headless with the fl-cliphist recorder and checks, through
# wlr-data-control:
#  1. texts put on the clipboard are recorded, newest first
#  2. --copy puts an older entry on the clipboard again (and on top)
#  3. History=false in panel.ini stops recording
# usage: cliphist-smoke.sh <bindir>
set -u
BIN="$1"
[ -x "$BIN/fl-wm" ] && [ -x "$BIN/fl-cliphist" ] || { echo "no fl-wm/fl-cliphist"; exit 77; }
T=$(mktemp -d)
mkdir -p "$T/run" "$T/config/fldesktop"
chmod 700 "$T/run"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" XDG_STATE_HOME="$T/state" HOME="$T"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM 2>/dev/null; wait $WM 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; "$BIN/fl-cliphist" --list; exit 1; }
for i in $(seq 50); do [ -S "$T/run/wayland-0" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
"$BIN/fl-cliphist" &
REC=$!
sleep 0.5
CH="$BIN/fl-cliphist"
listed() { for i in $(seq 30); do "$CH" --list | cut -f4 | head -1 | grep -qx "$1" && return 0; sleep 0.1; done; return 1; }

"$CH" --set "first text" || fail "--set"
listed "first text" || fail "first text not recorded"
"$CH" --set "second
line" || fail "--set"
listed "second line" || fail "second text not recorded"
[ "$("$CH" --paste)" = "second
line" ] || fail "--paste: $("$CH" --paste)"
# put the older one back
id=$("$CH" --list | grep 'first text' | cut -f1)
"$CH" --copy "$id" || fail "--copy"
listed "first text" || fail "--copy did not bring the entry to the top"
[ "$("$CH" --paste)" = "first text" ] || fail "--paste after --copy"
[ "$("$CH" --list | wc -l)" = 2 ] || fail "a copied entry was added twice"
# an image
command -v python3 >/dev/null 2>&1 && {
	python3 -I - "$T/img.png" <<'PY'
import sys, zlib, struct
w, h = 40, 30
rows = b"".join(b"\0" + b"".join(bytes([x * 6, y * 8, 160]) for x in range(w)) for y in range(h))
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                              chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
PY
	"$CH" --set-file "$T/img.png" || fail "--set-file"
	listed "Image 40×30" || fail "the image was not recorded"
	"$CH" --list | head -1 | grep -q "image/png" || fail "image type"
	"$CH" --delete "$("$CH" --list | head -1 | cut -f1)" || fail "--delete"
}
# pin, delete, clear
"$CH" --pin "$id" || fail "--pin"
"$CH" --list | grep -q "^$id	P" || fail "not pinned"
"$CH" --clear
[ "$("$CH" --list | wc -l)" = 1 ] || fail "--clear removed the pinned entry or kept others"
# no recording when turned off
printf '[Clipboard]\nHistory=false\n' >"$T/config/fldesktop/panel.ini"
"$CH" --set "secret"
sleep 0.5
"$CH" --list | grep -q secret && fail "recorded with History=false"
kill $REC
echo "clipboard history smoke test passed"
exit 0
