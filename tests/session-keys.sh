#!/bin/sh
# Drives a headless fldesktop session with wtype: Super+Space opens the
# launcher, typing "calc" filters it and Enter starts fl-calc.
# usage: session-keys.sh <bindir> <datadir>
set -u
BIN="$1"
DATA="$2"
command -v wtype >/dev/null 2>&1 || { echo "wtype not installed, skipping"; exit 77; }
TMP=$(mktemp -d)
export XDG_RUNTIME_DIR="$TMP/run" HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/home/.config"
mkdir -p "$XDG_RUNTIME_DIR" "$HOME" && chmod 700 "$XDG_RUNTIME_DIR"
export XDG_DATA_DIRS="$DATA:/usr/local/share:/usr/share" FLD_DATADIR="$DATA/fldesktop"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
LOG="$TMP/wm.log"
"$BIN/fl-wm" -d -s "$BIN/fl-panel" >"$LOG" 2>&1 &
WM=$!
sleep 2
SOCK=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$LOG" | head -1 | cut -d= -f2)
export WAYLAND_DISPLAY="$SOCK"
wtype -M logo ' ' -m logo
sleep 3
wtype calc
sleep 1
wtype -k Return
sleep 3
kill -TERM $WM 2>/dev/null
wait $WM 2>/dev/null
fail=0
grep -q "map: .*title=fldesktop.launcher" "$LOG" || { echo "launcher did not open"; fail=1; }
grep -q "map: app_id=fl-calc" "$LOG" || { echo "fl-calc was not started from the launcher"; fail=1; }
grep -q "Assertion" "$LOG" && { echo "assertion failure"; fail=1; }
[ $fail = 0 ] || tail -30 "$LOG"
rm -rf "$TMP"
[ $fail = 0 ] && echo "session keyboard test passed"
exit $fail
