#!/bin/sh
# Regenerates screenshots/*.png from a headless session (needs grim and wtype).
#   tools/screenshots.sh [builddir]
# Runs on a private D-Bus session bus (notifications, portal). The screenshots
# of printers, bluetooth, packages, flatpak and firewall need those services;
# they are taken in the test containers (tests/container).
set -eu
if [ -z "${FLD_SHOT_BUS:-}" ] && command -v dbus-run-session >/dev/null 2>&1; then
	FLD_SHOT_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
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
# fl-mixer talks to the PipeWire of the user running this script
PW_RUN=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export PIPEWIRE_RUNTIME_DIR="$PW_RUN" HOME="$TMP/home" XDG_RUNTIME_DIR="$TMP/run" XDG_CONFIG_HOME="$TMP/home/.config"
export XDG_DATA_DIRS="$BUILD/share:/usr/share" FLD_DATADIR="$BUILD/share/fldesktop" PATH="$BUILD/bin:$PATH"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
LOG="$TMP/wm.log"

session() { # session <startup> <wait> <name> [keys...]
	"$BUILD/bin/fl-wm" -s "$1" >"$LOG" 2>&1 &
	WM=$!
	sleep "$2"
	export WAYLAND_DISPLAY=$(grep -ao 'WAYLAND_DISPLAY=[a-z0-9-]*' "$LOG" | head -1 | cut -d= -f2)
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
NAME=settings; session "fl-panel & fl-settings appearance" 3.5 x
NAME=keyboard; session "fl-panel & fl-edit $HOME/Desktop/README.md & sleep 1.2; fl-osk" 4 x
NAME=charmap; session "fl-panel & fl-charmap" 4 x
NAME=notifications; session "fl-notifyd & fl-panel & sleep 1.5; $ROOT/tools/demo-notifications.sh" 4 x
# the file chooser of xdg-desktop-portal-fl, called the way xdg-desktop-portal does
DOCS=$(printf '%s' "$HOME/Documents" | od -An -v -tx1 | tr -s ' \n' ' ' | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1, /g')
NAME=filechooser; session "fl-panel & xdg-desktop-portal-fl & sleep 1.5; gdbus call --session \
	-d org.freedesktop.impl.portal.desktop.fl -o /org/freedesktop/portal/desktop \
	-m org.freedesktop.impl.portal.FileChooser.OpenFile /org/freedesktop/portal/desktop/request/1_1/s1 \
	org.example.App '' 'Open a Document' \"{'current_folder': <@ay [$DOCS 0x00]>, 'filters': <[('Text', [(uint32 0, '*.txt')]), ('All files', [(uint32 0, '*')])]>}\" &" 4 x
if [ -S "$PW_RUN/pipewire-0" ]; then
	"$BUILD/tests/fl-tone" 15 "Music Player" >/dev/null 2>&1 &
	NAME=mixer; session "fl-panel & fl-mixer" 4 x
else
	echo "no PipeWire at $PW_RUN: skipping the mixer screenshot" >&2
fi
NAME=taskmgr; session "fl-panel & fl-taskmgr" 4 x
NAME=taskmgr-performance; session "fl-panel & fl-taskmgr --performance" 9 x
# keys typed with wtype from inside the session
NAME=files-search; session "fl-panel & fl-files $HOME/Documents/fldesktop-apps & sleep 1.5; wtype -M ctrl -M shift f -m shift -m ctrl; sleep 0.4; wtype '*.h'; wtype -k Return; sleep 0.8; wtype -M ctrl 3 -m ctrl" 4.5 x
printf '[Appearance]\nScheme=fld-rounded\n' > "$HOME/.config/fldesktop/desktop.ini"
NAME=styles; session "fl-panel & fl-settings appearance" 3.5 x
rm -f "$HOME/.config/fldesktop/desktop.ini"
if command -v ffmpeg >/dev/null 2>&1 && [ -x "$BUILD/bin/fl-video" ]; then
	ffmpeg -loglevel error -y -f lavfi -i testsrc2=duration=20:size=1280x720:rate=30 -f lavfi -i sine=frequency=440:duration=20 \
		-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$HOME/clip.mkv"
	printf '1\n00:00:00,500 --> 00:00:19,000\nHello from fl-video subtitles\n' > "$HOME/clip.srt"
	NAME=video; session "fl-panel & fl-video $HOME/clip.mkv" 6 x
else
	echo "no ffmpeg or fl-video: skipping the video screenshot" >&2
fi
# tabs, the menu bar and a selection; then the other settings again (the menu bar is off by default)
NAME=files-tabs; session "fl-panel & fl-files $HOME/Pictures & sleep 1.5; wtype -M ctrl t -m ctrl; sleep 0.4; \
	wtype -M ctrl l -m ctrl; wtype '$HOME/Documents'; wtype -k Return; sleep 0.4; wtype -M ctrl t -m ctrl; sleep 0.4; \
	wtype -M alt 1 -m alt; sleep 0.3; wtype -M ctrl m -m ctrl; sleep 0.3; wtype -M ctrl a -m ctrl" 5.5 x
rm -f "$HOME/.config/fldesktop/files.ini"
NAME=files-connect; session "fl-panel & fl-files --connect" 4 x
NAME=sysinfo; session "fl-panel & fl-sysinfo" 3.5 x
NAME=sysinfo-devices; session "fl-panel & fl-sysinfo --page devices" 3.5 x
# the launcher needs a key press (Super+Space) from wtype
"$BUILD/bin/fl-wm" -s "fl-desktop & fl-panel" >"$LOG" 2>&1 &
WM=$!
sleep 2.5
export WAYLAND_DISPLAY=$(grep -ao 'WAYLAND_DISPLAY=[a-z0-9-]*' "$LOG" | head -1 | cut -d= -f2)
wtype -M logo ' ' -m logo
sleep 3
grim "$OUT/launcher.png"
kill $WM; wait $WM 2>/dev/null || true
ls -1 "$OUT"
