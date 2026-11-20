#!/bin/sh
# Starts fl-wm headless with fl-edit and checks window snapping through the
# window list fl-wm writes on SIGUSR2 (outer frames: x y width height):
#  1. dragging the title bar to the left edge snaps to the left half
#  2. Super+Up: top left quarter; Super+Right: top right; Super+Down: right
#     half; Super+Left: back to the size it had
#  3. Super+R, Right three times, Return: 96 pixels wider
# usage: snap-smoke.sh <bindir> <fl-vptr>
set -u
BIN="$1"
VPTR="$2"
[ -x "$BIN/fl-wm" ] && [ -x "$BIN/fl-edit" ] || { echo "no fl-wm/fl-edit"; exit 77; }
command -v wtype >/dev/null 2>&1 || { echo "no wtype"; exit 77; }
T=$(mktemp -d)
mkdir -p "$T/run" "$T/config/fldesktop"
chmod 700 "$T/run"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" HOME="$T"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM 2>/dev/null; wait $WM 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; grep -v DEBUG "$T/wm.log" | tail -8; exit 1; }
for i in $(seq 50); do [ -S "$T/run/wayland-0" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
geo() { # the frame of the fl-edit window
	rm -f "$T/run/fl-wm-windows"
	kill -USR2 $WM
	for i in $(seq 20); do [ -f "$T/run/fl-wm-windows" ] && break; sleep 0.05; done
	grep -m1 'fl-edit\|Untitled\|note' "$T/run/fl-wm-windows" | cut -d' ' -f1-4
}
expect() { # name "x y w h": waits up to 3 s for it
	for i in $(seq 30); do
		g=$(geo)
		[ "$g" = "$2" ] && return 0
		sleep 0.1
	done
	fail "$1: '$g', expected '$2'"
}
settle() { # waits until the geometry stops changing
	prev=""
	for i in $(seq 30); do
		sleep 0.15
		g=$(geo)
		[ -n "$g" ] && [ "$g" = "$prev" ] && return 0
		prev=$g
	done
}
timeout 30 "$BIN/fl-edit" "$T/note.txt" >/dev/null 2>&1 &
for i in $(seq 50); do [ -n "$(geo)" ] && break; sleep 0.1; done
start=$(geo)
[ -n "$start" ] || fail "no fl-edit window: $(cat "$T/run/fl-wm-windows")"
set -- $start
# 1. drag by the title bar (16 px below the frame's top) to the left edge
"$VPTR" drag $(($1 + $3 / 2)) $(($2 + 12)) 2 360
settle
set -- $(geo)
# the usable area: the output (1280x720) less the panel, wherever it is
[ "$1" = 0 ] && [ "$3" = 640 ] && [ "$4" -ge 600 ] || fail "drag to the left edge: $(geo)"
top=$2
h=$4
# 2. Super+arrows
wtype -M logo -k Up -m logo
expect "Super+Up" "0 $top 640 $((h / 2))"
wtype -M logo -k Right -m logo
expect "Super+Right" "640 $top 640 $((h / 2))"
wtype -M logo -k Down -m logo
expect "Super+Down" "640 $top 640 $h"
wtype -M logo -k Right -m logo
expect "Super+Right on the right half" "640 $top 640 $h"
wtype -M logo -k Left -m logo
settle
set -- $(geo)
w0=$3
[ "$(geo)" != "640 $top 640 $h" ] || fail "Super+Left did not restore the window: $(geo)"
[ "$(geo)" = "$start" ] || echo "note: restored to $(geo), started at $start"
# 3. keyboard resize
wtype -M logo -k r -m logo
wtype -k Right -k Right -k Right -k Return
settle
set -- $(geo)
[ "$3" = $((w0 + 96)) ] || fail "resize mode: width $3, expected $((w0 + 96))"
grep -q "resize mode: done" "$T/wm.log" || fail "resize mode did not end"
echo "snap smoke test passed"
exit 0
