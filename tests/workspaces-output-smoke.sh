#!/bin/sh
# Workspaces of their own on each output ([behaviour] workspaces_per_output):
# two headless outputs, fl-calc on one, fl-clock on the other (window
# rules). With fl-clock focused, switching to workspace 2 changes only
# the second output: fl-calc stays in the window list, fl-clock goes; back on
# workspace 1 both are listed.
# usage: workspaces-output-smoke.sh <bindir> <fl-workspaces>
set -u
BIN="$1"
WS="$2"
command -v "$BIN/fl-wm" >/dev/null 2>&1 || { echo "no fl-wm"; exit 77; }
[ -x "$BIN/fl-clock" ] || { echo "no fl-clock"; exit 77; }
TMP=$(mktemp -d)
export XDG_RUNTIME_DIR="$TMP/run" XDG_CONFIG_HOME="$TMP/config"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/fldesktop" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
LOG="$TMP/wm.log"
cat >"$XDG_CONFIG_HOME/fldesktop/wm.ini" <<EOI
[behaviour]
workspaces_per_output=true
[rule:calc]
app_id=fl-calc
output=HEADLESS-1
[rule:clock]
app_id=fl-clock
output=HEADLESS-2
EOI

cat >"$TMP/run.sh" <<EOS
#!/bin/sh
list() { "$WS" list >"$TMP/\$1" 2>&1; echo "exit \$?" >>"$TMP/\$1"; }
# waits until the window list has \$2 lines matching \$1
wait_for() {
	for i in \$(seq 50); do
		[ "\$("$WS" list 2>/dev/null | grep -c "^toplevel \$1")" = "\$2" ] && return 0
		sleep 0.1
	done
}
timeout 25 "$BIN/fl-calc" &
wait_for fl-calc 1
timeout 25 "$BIN/fl-clock" &
wait_for fl-clock 1
sleep 0.3
list one
"$WS" activate 2
wait_for fl-clock 0
list two
"$WS" activate 1
wait_for fl-clock 1
list three
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
n() { grep -c "^toplevel $2" "$TMP/$1" 2>/dev/null; }
check '[ -e "$TMP/done" ]' "the test script did not finish"
check '[ "$(n one fl-calc)" = 1 ] && [ "$(n one fl-clock)" = 1 ]' "the two windows are not listed at the start"
check '[ "$(n two fl-clock)" = 0 ]' "fl-clock (second output) is still listed on workspace 2"
check '[ "$(n two fl-calc)" = 1 ]' "fl-calc (first output) went away: the switch was not per output"
check 'grep -q "^workspace 2 active" "$TMP/two"' "workspace 2 is not active for the focused output"
check '[ "$(n three fl-clock)" = 1 ] && [ "$(n three fl-calc)" = 1 ]' "back on workspace 1 the windows are not both listed"
check '! grep -q "Assertion" "$LOG"' "assertion failure"
if [ -n "$why" ]; then
	echo "FAIL: $why"
	for f in one two three; do
		echo "--- $f"
		cat "$TMP/$f" 2>/dev/null
	done
	echo "--- fl-wm log (end)"
	tail -30 "$LOG"
	rm -rf "$TMP"
	exit 1
fi
echo "per-output workspaces smoke test passed"
rm -rf "$TMP"
