#!/bin/sh
# Starts fl-wm headless and runs X11 clients through Xwayland in it:
#  1. a window fl-wm places, decorates and focuses; it opens an
#     override-redirect "menu"; a click on its close button closes it
#  2. a window with a position hint and no decorations (Motif hints)
#  3. a window that asks to be maximized (_NET_WM_STATE)
# A failed run is tried once more: Xwayland now and then pairs a new window
# with its surface only seconds later (see the comment in xwayland.c); a
# second failure fails the test.
# usage: xwayland-smoke.sh <bindir> <fl-xclient>
set -u
BIN="$1"
XCLIENT="$2"
TOOLS=$(dirname "$XCLIENT")
command -v "$BIN/fl-wm" >/dev/null 2>&1 || { echo "no fl-wm"; exit 77; }
command -v Xwayland >/dev/null 2>&1 || { echo "no Xwayland installed"; exit 77; }
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$(mktemp -d)}"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1

# runs the clients once; prints why it failed and returns 1, or returns 0
# (2: fl-wm was built without Xwayland)
run_once() {
	TMP=$(mktemp -d)
	export XDG_CONFIG_HOME="$TMP/config" # not the user's wm.ini
	LOG="$TMP/wm.log"
	cat >"$TMP/run.sh" <<EOF
#!/bin/sh
{ "$XCLIENT" -t 15 --menu; echo "exit \$?"; } >"$TMP/one.out" 2>&1 &
ONE=\$!
# wait until it is focused and its menu came and went (the first client also
# starts Xwayland), then click the close button: the title bar is 30 px above
# the content, the close button the rightmost 30 px of it
for i in \$(seq 100); do grep -q "^menu-mapped" "$TMP/one.out" && break; sleep 0.1; done
sleep 1.5
set -- \$(grep "^configure" "$TMP/one.out" | tail -1)
[ \$# -eq 5 ] && "$TOOLS/fl-vptr" click \$((\$2 + \$4 - 15)) \$((\$3 - 16))
wait \$ONE
"$XCLIENT" -t 2 --nodecor --pos 100,150 >"$TMP/two.out" 2>&1
"$XCLIENT" -t 2 --maximize >"$TMP/three.out" 2>&1
echo done >"$TMP/done"
EOF
	chmod +x "$TMP/run.sh"

	"$BIN/fl-wm" -d -s "$TMP/run.sh" >"$LOG" 2>&1 &
	WM=$!
	for i in $(seq 300); do
		[ -e "$TMP/done" ] && break
		sleep 0.1
	done
	sleep 0.5
	kill -TERM $WM 2>/dev/null
	wait $WM
	STATUS=$?

	why=""
	check() { [ -n "$why" ] || eval "$1" || why="$2"; }
	check 'grep -q "running on WAYLAND_DISPLAY" "$LOG"' "compositor did not start"
	if [ -z "$why" ] && ! grep -q "X11 programs use DISPLAY" "$LOG"; then
		rm -rf "$TMP"
		return 2
	fi
	check 'grep -q "Xwayland is ready" "$LOG"' "Xwayland did not start"
	check 'grep -q "map: app_id=FlXclient title=fl-xclient role=0 (X11)" "$LOG"' "the X11 window was not mapped"
	check 'grep -q "^mapped" "$TMP/one.out"' "client 1 was not mapped"
	check 'grep -q "^focus-in" "$TMP/one.out"' "client 1 did not get the focus"
	check 'grep -q "^menu-mapped" "$TMP/one.out"' "the override-redirect window was not mapped"
	# centered on the 1280x720 output, below a 30 px title bar and 1 px border
	check 'grep -q "^configure 480 275 320 200" "$TMP/one.out"' "client 1 was not centered"
	check 'grep -q "^delete-window" "$TMP/one.out"' "the close button did not close client 1"
	check 'grep -q "^configure 100 150 320 200" "$TMP/two.out"' "client 2 did not keep its position hint"
	# the 1280x720 output without the 30 px title bar and the 1 px borders
	check 'grep -q "^configure 1 31 1278 688" "$TMP/three.out"' "client 3 was not maximized"
	check '! grep -q "Assertion" "$LOG"' "assertion failure"
	if [ -n "$why" ]; then
		echo "FAIL: $why"
		echo "--- client 1"; cat "$TMP/one.out" 2>/dev/null
		echo "--- client 2"; cat "$TMP/two.out" 2>/dev/null
		echo "--- client 3"; cat "$TMP/three.out" 2>/dev/null
		echo "--- fl-wm"; grep -v "^$" "$LOG" | tail -60
		rm -rf "$TMP"
		return 1
	fi
	rm -rf "$TMP"
	return 0
}

for attempt in 1 2; do
	run_once
	r=$?
	[ $r -eq 2 ] && { echo "fl-wm was built without Xwayland"; exit 77; }
	if [ $r -eq 0 ]; then
		echo "Xwayland smoke test passed (attempt $attempt, fl-wm exit $STATUS)"
		exit 0
	fi
	[ $attempt -eq 1 ] && echo "attempt 1 failed, trying once more"
done
exit 1
