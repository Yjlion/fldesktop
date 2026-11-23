#!/bin/sh
# fl-remote against a fake VNC server (tests/tools/fake-vnc.py) in a
# headless session: the remote screen (red | blue) shows up in the window.
# usage: remote-smoke.sh <bindir>
set -u
BIN="$1"
HERE=$(cd "$(dirname "$0")" && pwd)
command -v python3 >/dev/null 2>&1 || { echo "no python3"; exit 77; }
command -v grim >/dev/null 2>&1 || { echo "no grim"; exit 77; }
TMP=$(mktemp -d)
trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT
export XDG_RUNTIME_DIR="$TMP/run" HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/home/.config"
mkdir -p "$XDG_RUNTIME_DIR" "$HOME" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
PORT=$((20000 + $$ % 20000))
python3 -I "$HERE/tools/fake-vnc.py" $PORT &
SRV=$!
sleep 0.5
cat >"$TMP/run.sh" <<EOS
"$BIN/fl-remote" vnc://127.0.0.1:$PORT &
sleep 4
grim -t ppm "$TMP/shot.ppm"
kill \$FLWM_PID
EOS
timeout 20 "$BIN/fl-wm" -s "sh $TMP/run.sh" >"$TMP/wm.log" 2>&1
[ -s "$TMP/shot.ppm" ] || { echo "no screenshot"; tail -20 "$TMP/wm.log"; exit 1; }
# count clearly red and clearly blue pixels
python3 -I - "$TMP/shot.ppm" <<'EOP' || exit 1
import sys
d = open(sys.argv[1], "rb").read()
parts = d.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
red = blue = 0
for i in range(0, w * h * 3, 3 * 7):
    r, g, b = px[i], px[i + 1], px[i + 2]
    red += r > 200 and g < 50 and b < 50
    blue += b > 200 and r < 50 and g < 50
if red < 500 or blue < 500:
    print(f"the remote screen did not show (red {red}, blue {blue})")
    sys.exit(1)
EOP
echo "remote smoke test passed"
