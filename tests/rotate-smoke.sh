#!/bin/sh
# fl-autorotate --set turns an output through wlr-output-management (the
# headless output stands for the built-in screen): 1280x720 becomes 720x1280.
# usage: rotate-smoke.sh <bindir>
set -u
BIN="$1"
command -v grim >/dev/null 2>&1 || { echo "no grim"; exit 77; }
[ -x "$BIN/fl-autorotate" ] || { echo "no fl-autorotate"; exit 77; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export XDG_RUNTIME_DIR="$TMP/run" HOME="$TMP/home"
mkdir -p "$XDG_RUNTIME_DIR" "$HOME" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export FLD_ROTATE_OUTPUT=HEADLESS-1
cat >"$TMP/run.sh" <<EOS
sleep 1
"$BIN/fl-autorotate" --set left-up
sleep 0.5
grim -t ppm "$TMP/a.ppm"
"$BIN/fl-autorotate" --set normal
sleep 0.5
grim -t ppm "$TMP/b.ppm"
kill \$FLWM_PID
EOS
timeout 20 "$BIN/fl-wm" -s "sh $TMP/run.sh" >"$TMP/wm.log" 2>&1
a=$(sed -n 2p "$TMP/a.ppm" 2>/dev/null)
b=$(sed -n 2p "$TMP/b.ppm" 2>/dev/null)
[ "$a" = "720 1280" ] || { echo "turned: '$a', expected 720 1280"; tail -20 "$TMP/wm.log"; exit 1; }
[ "$b" = "1280 720" ] || { echo "back: '$b', expected 1280 720"; exit 1; }
echo "rotate smoke test passed"
