#!/bin/sh
# The brightness keys in a headless session: fl-wm binds XF86MonBrightness*
# to fl-panel's actions, the panel steps a fake sysfs backlight and shows the
# on-screen display (fldesktop.osd); without the panel the action still works.
# usage: osd-smoke.sh <bindir>
set -u
BIN="$1"
command -v wtype >/dev/null 2>&1 || { echo "wtype not installed, skipping"; exit 77; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export XDG_RUNTIME_DIR="$TMP/run" HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/home/.config"
mkdir -p "$XDG_RUNTIME_DIR" "$HOME" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export FLD_SYSFS_ROOT="$TMP/sys" FLD_LOGIND_BUS=session PATH="$BIN:$PATH"
D="$TMP/sys/sys/class/backlight/intel_backlight"
mkdir -p "$D" && echo raw >"$D/type" && echo 1000 >"$D/max_brightness" && echo 600 >"$D/brightness"
LOG="$TMP/wm.log"
"$BIN/fl-wm" -d -s "$BIN/fl-panel" >"$LOG" 2>&1 &
WM=$!
for i in $(seq 1 50); do
	SOCK=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$LOG" | head -1 | cut -d= -f2)
	[ -n "$SOCK" ] && [ -S "$XDG_RUNTIME_DIR/fldesktop-panel-$SOCK.sock" ] && break
	sleep 0.2
done
export WAYLAND_DISPLAY="$SOCK"
sleep 0.5
wtype -k XF86MonBrightnessUp
sleep 1
wtype -k XF86MonBrightnessUp
sleep 1
kill -TERM $WM 2>/dev/null
wait $WM 2>/dev/null
for i in $(seq 1 50); do [ -S "$XDG_RUNTIME_DIR/fldesktop-panel-$SOCK.sock" ] || break; sleep 0.1; done
fail=0
[ "$(cat "$D/brightness")" = 700 ] || { echo "brightness $(cat "$D/brightness"), expected 700"; fail=1; }
grep -q "map: .*title=fldesktop.osd.*role=4" "$LOG" || { echo "the on-screen display did not show"; fail=1; }
# no panel: fl-panel --action does it alone
"$BIN/fl-panel" --action brightness-down || fail=1
[ "$(cat "$D/brightness")" = 650 ] || { echo "headless: brightness $(cat "$D/brightness"), expected 650"; fail=1; }
"$BIN/fl-panel" --action no-such-thing 2>/dev/null && { echo "an unknown action was accepted"; fail=1; }
[ $fail = 0 ] || { tail -30 "$LOG"; exit 1; }
echo "osd smoke test passed"
