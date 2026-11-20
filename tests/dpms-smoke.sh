#!/bin/sh
# Starts fl-wm headless with a short blank timeout and checks the screen
# power through wlr-output-power-management:
#  1. idle: the screen turns off; pointer motion turns it on again
#  2. a client turns it off: input leaves it off until the client turns it on
# usage: dpms-smoke.sh <bindir> <fl-power> <fl-vptr>
set -u
BIN="$1"
POWER="$2"
VPTR="$3"
[ -x "$BIN/fl-wm" ] || { echo "no fl-wm"; exit 77; }
T=$(mktemp -d)
mkdir -p "$T/run" "$T/config/fldesktop"
chmod 700 "$T/run"
printf '[power]\nblank_minutes=0.04\n' >"$T/config/fldesktop/wm.ini"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" HOME="$T"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM 2>/dev/null; wait $WM 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; grep -v DEBUG "$T/wm.log" | tail -15; exit 1; }
for i in $(seq 50); do [ -S "$T/run/wayland-0" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
# 2.4 s after start the screen goes off
w=$("$POWER" 4 | tr '\n' ' ')
[ "$w" = "on off " ] || fail "idle blanking: '$w'"
"$VPTR" move 100 100 || fail "fl-vptr"
w=$("$POWER" 0.5 | tr '\n' ' ')
[ "$w" = "on " ] || fail "wake on input: '$w'"
"$POWER" set off || fail "set off"
w=$("$POWER" 0.3 | tr '\n' ' ')
[ "$w" = "off " ] || fail "client off: '$w'"
"$VPTR" move 300 300
w=$("$POWER" 0.3 | tr '\n' ' ')
[ "$w" = "off " ] || fail "input woke a screen a client turned off: '$w'"
"$POWER" set on || fail "set on"
w=$("$POWER" 0.3 | tr '\n' ' ')
[ "$w" = "on " ] || fail "client on: '$w'"
grep -q "idle: screens off" "$T/wm.log" || fail "no log line"
echo "dpms smoke test passed"
exit 0
