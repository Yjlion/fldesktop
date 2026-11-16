#!/bin/sh
# Regenerates screenshots/*.png from a headless session (needs grim and wtype).
#   tools/screenshots.sh [builddir]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build}
OUT="$ROOT/screenshots"
TMP=$(mktemp -d)
trap 'kill $WM 2>/dev/null; rm -rf "$TMP"' EXIT
mkdir -p "$OUT" "$TMP/run" "$TMP/home/Desktop/Projects" "$TMP/home/Documents" "$TMP/home/Pictures" \
         "$TMP/home/Downloads" "$TMP/home/Music" "$TMP/home/.config/fldesktop"
chmod 700 "$TMP/run"
cp "$ROOT/README.md" "$TMP/home/Desktop/README.md"
cp "$ROOT/apps/fl-calc/calc_engine.cpp" "$TMP/home/Documents/"
cp -r "$ROOT/apps" "$TMP/home/Documents/fldesktop-apps"
cp "$ROOT/data/backgrounds/default.svg" "$TMP/home/Pictures/wallpaper.svg"
for i in "$ROOT"/data/icons/hicolor/scalable/apps/*.svg; do cp "$i" "$TMP/home/Pictures/"; done
export HOME="$TMP/home" XDG_RUNTIME_DIR="$TMP/run" XDG_CONFIG_HOME="$TMP/home/.config"
export XDG_DATA_DIRS="$BUILD/share:/usr/share" FLD_DATADIR="$BUILD/share/fldesktop" PATH="$BUILD/bin:$PATH"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
LOG="$TMP/wm.log"

session() { # session <startup> <wait> <name> [keys...]
	"$BUILD/bin/fl-wm" -s "$1" >"$LOG" 2>&1 &
	WM=$!
	sleep "$2"
	export WAYLAND_DISPLAY=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$LOG" | head -1 | cut -d= -f2)
	shift 3 || true
	grim "$OUT/$NAME.png"
	kill $WM; wait $WM 2>/dev/null || true
}

NAME=desktop; session "fl-desktop & fl-panel & sleep 1.5; fl-term -d $HOME/Documents & sleep 0.6; fl-files $HOME" 5 x
NAME=terminal; session "fl-panel & fl-term -e sh -c 'ls --color=always /usr/share | head -12; printf \"\\033[1;32mfl-term\\033[0m: \\033[38;5;208m256\\033[0m and \\033[38;2;100;180;255mtrue colour\\033[0m, UTF-8 ✓ ─│┌┐\\n\"; exec sh'" 3.5 x
NAME=editor; session "fl-panel & fl-edit $HOME/Documents/calc_engine.cpp $HOME/Desktop/README.md" 3.5 x
NAME=files; session "fl-panel & fl-files $HOME/Pictures" 4 x
NAME=calculator; session "fl-panel & fl-calc" 3 x
NAME=viewer; session "fl-panel & fl-view $HOME/Pictures/wallpaper.svg" 3.5 x
NAME=greeter; session "fl-greet --demo" 3 x
# the launcher needs a key press (Super+Space) from wtype
"$BUILD/bin/fl-wm" -s "fl-desktop & fl-panel" >"$LOG" 2>&1 &
WM=$!
sleep 2.5
export WAYLAND_DISPLAY=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$LOG" | head -1 | cut -d= -f2)
wtype -M logo ' ' -m logo
sleep 3
grim "$OUT/launcher.png"
kill $WM; wait $WM 2>/dev/null || true
ls -1 "$OUT"
