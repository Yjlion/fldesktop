#!/bin/sh
# Starts fl-wm headless and checks its workspaces through ext-workspace-v1:
#  1. fl-calc opens on workspace 1 and is in the panel's window list
#  2. on workspace 2 the list is empty; a second fl-calc opens there
#  3. back on workspace 1 only the first fl-calc is listed
#  4. with wtype: Super+Shift+3 sends the window to workspace 3, Super+3 goes there
# usage: workspaces-smoke.sh <bindir> <fl-workspaces>
set -u
BIN="$1"
WS="$2"
command -v "$BIN/fl-wm" >/dev/null 2>&1 || { echo "no fl-wm"; exit 77; }
TMP=$(mktemp -d)
export XDG_RUNTIME_DIR="$TMP/run" XDG_CONFIG_HOME="$TMP/config"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
LOG="$TMP/wm.log"
HAVE_WTYPE=0
command -v wtype >/dev/null 2>&1 && HAVE_WTYPE=1

cat >"$TMP/run.sh" <<EOS
#!/bin/sh
list() { "$WS" list >"$TMP/\$1" 2>&1; echo "exit \$?" >>"$TMP/\$1"; }
# waits until the window list holds \$1 fl-calc windows
calcs() {
	for i in \$(seq 50); do
		[ "\$("$WS" list 2>/dev/null | grep -c '^toplevel fl-calc')" = "\$1" ] && return 0
		sleep 0.1
	done
}
timeout 25 "$BIN/fl-calc" &
calcs 1
list one
"$WS" activate 2
calcs 0
list two
timeout 25 "$BIN/fl-calc" &
calcs 1
list three
"$WS" activate 1
sleep 0.3
list four
if [ $HAVE_WTYPE = 1 ]; then
	wtype -M logo -M shift 3 -m shift -m logo
	calcs 0
	list five
	wtype -M logo 3 -m logo
	calcs 1
	list six
fi
echo done >"$TMP/done"
EOS
chmod +x "$TMP/run.sh"

"$BIN/fl-wm" -d -s "$TMP/run.sh" >"$LOG" 2>&1 &
WM=$!
for i in $(seq 250); do
	[ -e "$TMP/done" ] && break
	sleep 0.1
done
sleep 0.3
kill -TERM $WM 2>/dev/null
wait $WM 2>/dev/null

if grep -q "^exit 3" "$TMP/one" 2>/dev/null; then
	echo "fl-wm has no ext-workspace-v1 (wlroots before 0.20)"
	rm -rf "$TMP"
	exit 77
fi
why=""
check() { [ -n "$why" ] || eval "$1" || why="$2"; }
n() { grep -c "^toplevel fl-calc" "$TMP/$1" 2>/dev/null; }
check '[ -e "$TMP/done" ]' "the test script did not finish"
check 'grep -q "^workspace 1 active" "$TMP/one"' "workspace 1 is not active at the start"
check 'grep -q "^workspace 4 inactive" "$TMP/one"' "there are not 4 workspaces"
check '[ "$(n one)" = 1 ]' "fl-calc is not in the window list"
check 'grep -q "^workspace 2 active" "$TMP/two"' "workspace 2 did not become active"
check '[ "$(n two)" = 0 ]' "the window of workspace 1 is listed on workspace 2"
check 'grep -q "workspace: 2" "$LOG"' "fl-wm did not log the switch"
check '[ "$(n three)" = 1 ]' "the window opened on workspace 2 is not listed"
check 'grep -q "^workspace 1 active" "$TMP/four"' "workspace 1 did not become active again"
check '[ "$(n four)" = 1 ]' "workspace 1 does not list exactly its own window"
if [ $HAVE_WTYPE = 1 ]; then
	check '[ "$(n five)" = 0 ]' "Super+Shift+3 did not move the window away"
	check 'grep -q "^workspace 3 active" "$TMP/six"' "Super+3 did not switch to workspace 3"
	check '[ "$(n six)" = 1 ]' "the moved window is not on workspace 3"
else
	echo "wtype not installed: key bindings not tested"
fi
check '! grep -q "Assertion" "$LOG"' "assertion failure"
if [ -n "$why" ]; then
	echo "FAIL: $why"
	for f in one two three four five six; do
		[ -e "$TMP/$f" ] && { echo "--- $f"; cat "$TMP/$f"; }
	done
	echo "--- fl-wm"; grep -v "^$" "$LOG" | tail -40
	rm -rf "$TMP"
	exit 1
fi
rm -rf "$TMP"
echo "workspace smoke test passed"
