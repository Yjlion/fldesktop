#!/bin/sh
# Starts fl-wm headless and checks, with a plain test window (fl-toplevel):
#  1. keyboard-shortcuts-inhibit: Super+2 goes to the window, not to fl-wm;
#     Super+Escape gives the bindings back and Super+2 switches workspaces
#  2. xdg-dialog-v1 (wlroots 0.19+): clicking a window with a modal dialog
#     focuses the dialog
#  3. xdg-toplevel-icon-v1 (wlroots 0.19+): fl-wm takes the icon name
#  4. Show Desktop (Super+D): one press hides all windows, the next brings
#     back the same ones in the same stacking order
# usage: toplevel-smoke.sh <bindir> <fl-toplevel> <fl-vptr>
set -u
BIN="$1"
TOP="$2"
VPTR="$3"
[ -x "$BIN/fl-wm" ] || { echo "no fl-wm"; exit 77; }
command -v wtype >/dev/null 2>&1 || { echo "no wtype"; exit 77; }
T=$(mktemp -d)
mkdir -p "$T/run"
chmod 700 "$T/run"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" HOME="$T"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM 2>/dev/null; wait $WM 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; grep -v DEBUG "$T/wm.log" | tail -10; exit 1; }
for i in $(seq 50); do [ -S "$T/run/wayland-0" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
wait_for() { # file pattern
	for i in $(seq 50); do grep -q "$2" "$1" && return 0; sleep 0.1; done
	return 1
}

# 1. shortcuts inhibit
"$TOP" --inhibit 6 >"$T/inh.out" 2>&1 &
top=$!
wait_for "$T/inh.out" "inhibit active" || fail "the inhibitor was not activated: $(cat "$T/inh.out")"
wtype -M logo -k 2 -m logo
sleep 0.3
grep -q "workspace: 2" "$T/wm.log" && fail "Super+2 switched workspaces while inhibited"
grep -q "^key" "$T/inh.out" || fail "the window got no keys"
wtype -M logo -k Escape -m logo
wait_for "$T/inh.out" "inhibit inactive" || fail "Super+Escape did not take the bindings back"
wtype -M logo -k 2 -m logo
wait_for "$T/wm.log" "workspace: 2" || fail "Super+2 did not switch workspaces after Super+Escape"
kill $top 2>/dev/null
wtype -M logo -k 1 -m logo
sleep 0.3

# 2. modal dialog and 3. icon
"$TOP" --modal --icon accessories-calculator 5 >"$T/modal.out" 2>"$T/modal.err" &
top=$!
sleep 1.5
if grep -q "no xdg-dialog\|without xdg-dialog" "$T/modal.err"; then
	echo "no xdg-dialog-v1 (wlroots < 0.19): modal and icon checks skipped"
else
	grep -q "icon of main: accessories-calculator" "$T/wm.log" || fail "the icon name did not arrive"
	tail -1 "$T/modal.out" | grep -q "enter dialog" || fail "the dialog did not get the focus: $(cat "$T/modal.out")"
	# click the main window's top left corner, outside the dialog
	kill -USR2 $WM
	sleep 0.3
	pos=$(grep ' main$' "$T/run/fl-wm-windows" | head -1)
	[ -n "$pos" ] || fail "main window not in $(cat "$T/run/fl-wm-windows")"
	set -- $pos
	"$VPTR" click $(($1 + 12)) $(($2 + 40))
	sleep 0.5
	tail -1 "$T/modal.out" | grep -q "enter dialog" || fail "the click focused the window under the modal dialog: $(cat "$T/modal.out")"
fi
wait $top

# 4. Show Desktop
"$TOP" 8 >/dev/null 2>&1 & t1=$!
sleep 0.4
"$TOP" 8 >/dev/null 2>&1 & t2=$!
sleep 0.4
"$TOP" 8 >/dev/null 2>&1 & t3=$!
sleep 0.8
windows() { kill -USR2 $WM; sleep 0.3; grep -c ' main$' "$T/run/fl-wm-windows"; }
order() { kill -USR2 $WM; sleep 0.3; grep ' main$' "$T/run/fl-wm-windows" | cut -d' ' -f1,2 | tr '\n' ';'; }
[ "$(windows)" = 3 ] || fail "3 windows expected before Show Desktop: $(cat "$T/run/fl-wm-windows")"
before=$(order)
wtype -M logo -k d -m logo
sleep 0.5
[ "$(windows)" = 0 ] || fail "Show Desktop left windows shown: $(cat "$T/run/fl-wm-windows")"
wtype -M logo -k d -m logo
sleep 0.5
[ "$(windows)" = 3 ] || fail "the second Super+D did not bring the 3 windows back: $(cat "$T/run/fl-wm-windows")"
after=$(order)
[ "$before" = "$after" ] || fail "stacking order or places changed: '$before' -> '$after'"
kill $t1 $t2 $t3 2>/dev/null
wait $t1 $t2 $t3 2>/dev/null
echo "toplevel smoke test passed"
exit 0
