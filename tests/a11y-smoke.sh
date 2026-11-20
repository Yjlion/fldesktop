#!/bin/sh
# Starts fl-wm headless and checks the accessibility features:
#  1. the magnifier: Super+= twice zooms 2x around the pointer (the pixel
#     under the pointer stays, the picture changes), Super+0 ends it
#  2. sticky keys: Super pressed and released alone, then 2, goes to
#     workspace 2
#  3. bounce keys: a key pressed again 40 ms after its release is dropped
# usage: a11y-smoke.sh <bindir> <fl-vptr>
set -u
BIN="$1"
VPTR="$2"
for c in wtype grim python3; do command -v $c >/dev/null 2>&1 || { echo "no $c"; exit 77; }; done
[ -x "$BIN/fl-wm" ] && [ -x "$BIN/fl-calc" ] && [ -x "$BIN/fl-edit" ] || { echo "no fl-wm"; exit 77; }
T=$(mktemp -d)
mkdir -p "$T/run" "$T/config/fldesktop"
chmod 700 "$T/run"
printf '[accessibility]\nsticky_keys=true\nbounce_keys=300\n' >"$T/config/fldesktop/wm.ini"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" HOME="$T"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM 2>/dev/null; wait $WM 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; grep -v DEBUG "$T/wm.log" | tail -8; exit 1; }
for i in $(seq 50); do [ -S "$T/run/wayland-0" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0

# 1. magnifier
timeout 20 "$BIN/fl-calc" >/dev/null 2>&1 &
sleep 2
"$VPTR" move 600 330
sleep 0.3
grim -t ppm "$T/before.ppm" || fail "grim"
wtype -M logo -k equal -m logo
sleep 0.4 # bounce keys (300 ms) would drop a quicker second press
wtype -M logo -k equal -m logo
"$VPTR" move 601 330
"$VPTR" move 600 330
sleep 0.5
grep -q "zoom: 2.0x" "$T/wm.log" || fail "Super+= did not zoom to 2x"
grim -t ppm "$T/zoomed.ppm" || fail "grim"
python3 -I - "$T/before.ppm" "$T/zoomed.ppm" <<'PY' || fail "the zoomed picture is wrong"
import sys
def load(p):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, parts[3]
w, a = load(sys.argv[1])
_, b = load(sys.argv[2])
px = lambda d, x, y: d[(y * w + x) * 3:(y * w + x) * 3 + 3]
# the point under the pointer stays (a few pixels off for the hardware cursor)
same = sum(px(a, x, 330) == px(b, x, 330) for x in range(580, 590))
diff = sum(a[i] != b[i] for i in range(0, len(a), 997))
sys.exit(0 if same >= 6 and diff > 50 else 1)
PY
sleep 0.4
wtype -M logo -k 0 -m logo
sleep 0.4
grep -q "zoom: 1.0x" "$T/wm.log" || fail "Super+0 did not end the zoom"

# 2. sticky keys: one virtual keyboard for the whole sequence
wtype -P Super_L -p Super_L -s 100 -k 2
for i in $(seq 20); do grep -q "workspace: 2" "$T/wm.log" && break; sleep 0.1; done
grep -q "workspace: 2" "$T/wm.log" || fail "sticky Super then 2 did not switch workspaces"
sleep 0.4
wtype -k 1 # no Super latched any more: plain 1, no switch
sleep 0.3
grep -q "workspace: 1" "$T/wm.log" && fail "the latched Super stayed on"

# 3. bounce keys
WAYLAND_DEBUG=1 timeout 6 "$BIN/fl-edit" "$T/b.txt" 2>"$T/edit.log" &
sleep 2
wtype -s 40 -k a -k a
sleep 0.5
n=$(grep -c 'wl_keyboard#[0-9]*\.key(' "$T/edit.log")
[ "$n" = 2 ] || fail "bounce keys: fl-edit got $n key events, expected 2 (one press, one release)"
echo "accessibility smoke test passed"
exit 0
