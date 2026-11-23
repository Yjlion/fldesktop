#!/bin/sh
# Starts fl-wm headless with fl-desktop and drags desktop icons with the
# virtual pointer:
#  1. a file icon onto a folder icon: the file moves into the folder
#  2. a file icon onto the empty desktop: the icon stays where it was dropped
#     (desktop-icons.ini) and the file stays in the Desktop folder
#  3. a file icon onto a launcher: the launcher runs with the file
#  4. the Trash icon (no file to hand over): moved on the desktop
# usage: dnd-smoke.sh <bindir> <fl-vptr>
set -u
BIN="$1"
VPTR="$2"
[ -x "$BIN/fl-wm" ] && [ -x "$BIN/fl-desktop" ] || { echo "no fl-wm/fl-desktop"; exit 77; }
T=$(mktemp -d)
mkdir -p "$T/run" "$T/config/fldesktop" "$T/Desktop/Box"
chmod 700 "$T/run"
echo hello >"$T/Desktop/a.txt"
echo world >"$T/Desktop/b.txt"
echo again >"$T/Desktop/c.txt"
# a launcher that copies the file dropped on it
printf '[Desktop Entry]\nType=Application\nName=Copy\nExec=cp %%f %s/got.txt\n' "$T" >"$T/Desktop/copy.desktop"
# the launcher and c.txt where the user put them (third column)
printf '[Positions]\ncopy.desktop=2,0\nc.txt=2,1\n' >"$T/config/fldesktop/desktop-icons.ini"
# the Desktop folder without xdg-user-dirs; normal icons (48 px: 100x96 cells)
printf 'XDG_DESKTOP_DIR="$HOME/Desktop"\n' >"$T/config/user-dirs.dirs"
printf '[Desktop]\nIconSize=48\n' >"$T/config/fldesktop/desktop.ini"
# only fl-desktop: no panel, so the icons start at the top left corner
printf '[autostart]\n1=\n' >"$T/config/fldesktop/wm.ini"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" HOME="$T"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $DESK $WM 2>/dev/null; wait $WM 2>/dev/null; rm -rf "$T"' EXIT
DESK=""
fail() { echo "FAIL: $*"; grep -v DEBUG "$T/wm.log" | tail -8; cat "$T/desktop.log" 2>/dev/null | tail -8; exit 1; }
for i in $(seq 50); do [ -S "$T/run/wayland-0" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
"$BIN/fl-desktop" >"$T/desktop.log" 2>&1 &
DESK=$!
# fl-wm's window list leaves out the desktop: give it time to map and draw
sleep 2
kill -0 $DESK 2>/dev/null || fail "fl-desktop did not start"
# the grid, column by column: Home, Trash, Box (folders first), a.txt, b.txt;
# cells are 100x96 from (8, 8): an icon's middle is at x 58, y 38 + 96 * row
row_y() { echo $((38 + 96 * $1)); }
# 1. a.txt (row 3) onto Box (row 2)
"$VPTR" drag 58 "$(row_y 3)" 58 "$(row_y 2)" 300
for i in $(seq 30); do [ -f "$T/Desktop/Box/a.txt" ] && break; sleep 0.1; done
[ -f "$T/Desktop/Box/a.txt" ] || fail "a.txt was not moved into Box: $(ls "$T/Desktop" "$T/Desktop/Box")"
[ ! -e "$T/Desktop/a.txt" ] || fail "a.txt is still on the desktop"
sleep 0.5
# 2. b.txt (now row 3) to the fifth column, second row: x 458, y 134
"$VPTR" drag 58 "$(row_y 3)" 458 "$(row_y 1)" 300
ini="$T/config/fldesktop/desktop-icons.ini"
for i in $(seq 30); do grep -q '^b.txt=4,1$' "$ini" 2>/dev/null && break; sleep 0.1; done
grep -q '^b.txt=4,1$' "$ini" 2>/dev/null || fail "b.txt was not placed at 4,1: $(cat "$ini" 2>/dev/null)"
[ -f "$T/Desktop/b.txt" ] || fail "b.txt left the Desktop folder"
# 3. c.txt (2,1) onto the launcher (2,0)
"$VPTR" drag 258 "$(row_y 1)" 258 "$(row_y 0)" 300
for i in $(seq 30); do [ -f "$T/got.txt" ] && break; sleep 0.1; done
[ "$(cat "$T/got.txt" 2>/dev/null)" = again ] || fail "the launcher did not get c.txt"
[ -f "$T/Desktop/c.txt" ] || fail "c.txt left the desktop"
# 4. Trash (0,1) to the sixth column, first row: x 558
"$VPTR" drag 58 "$(row_y 1)" 558 "$(row_y 0)" 300
for i in $(seq 30); do grep -q '^%3Atrash=5,0$' "$ini" 2>/dev/null && break; sleep 0.1; done
grep -q '^%3Atrash=5,0$' "$ini" || fail "Trash was not moved to 5,0: $(cat "$ini")"
echo "dnd-smoke: ok"
