#!/bin/sh
# Window rules in a headless session: a [rule:...] for fl-calc places it at
# 10,20 of the usable area and keeps it above other windows.
# usage: rules-smoke.sh <bindir>
set -u
BIN="$1"
[ -x "$BIN/fl-wm" ] && [ -x "$BIN/fl-calc" ] || { echo "no fl-wm or fl-calc"; exit 77; }
TMP=$(mktemp -d)
trap 'kill $WM 2>/dev/null; rm -rf "$TMP"' EXIT
export XDG_RUNTIME_DIR="$TMP/run" HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/home/.config"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/fldesktop" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
cat >"$XDG_CONFIG_HOME/fldesktop/wm.ini" <<INI
[autostart]
reset = true
[rule:calculator]
app_id = fl-calc
position = 10,20
always_on_top = true
opacity = 0.9
[rule:broken]
app_id = fl-calc
size = huge
INI
LOG="$TMP/wm.log"
"$BIN/fl-wm" -d -s "timeout 6 $BIN/fl-calc" >"$LOG" 2>&1 &
WM=$!
for i in $(seq 1 50); do grep -q "map: app_id=fl-calc" "$LOG" && break; sleep 0.2; done
sleep 0.5
kill -USR2 $WM
sleep 0.5
fail=0
grep -q "map: app_id=fl-calc" "$LOG" || { echo "fl-calc did not map"; tail -30 "$LOG"; exit 1; }
grep -q "rules: 2 match fl-calc" "$LOG" || { echo "the rules did not match"; fail=1; }
grep -q "\[rule:broken\] size = huge: unknown key or bad value" "$LOG" || { echo "a bad value was not reported"; fail=1; }
line=$(head -1 "$XDG_RUNTIME_DIR/fl-wm-windows" 2>/dev/null)
# the frame: content at 10,20+title bar, minus the border
case "$line" in
	"9 20 "*) ;;
	*) echo "fl-calc is at '$line', expected 9 20 ..."; fail=1 ;;
esac
[ $fail = 0 ] && echo "window rules: placed by the rule ($line)"
exit $fail
