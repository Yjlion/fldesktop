#!/bin/sh
# The lock screen in a headless session: fl-wm starts fl-lock --locker on
# SIGUSR1, and keys that close windows elsewhere (Escape, Alt+F4) must not end
# the locker with status 0, which fl-wm takes as "unlocked".
# usage: lock-smoke.sh <bindir>
set -u
BIN="$1"
command -v wtype >/dev/null 2>&1 || { echo "wtype not installed, skipping"; exit 77; }
TMP=$(mktemp -d)
trap 'kill $WM 2>/dev/null; rm -rf "$TMP"' EXIT
export XDG_RUNTIME_DIR="$TMP/run" HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/home/.config"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/fldesktop" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 PATH="$BIN:$PATH"
printf '[autostart]\nreset = true\n[lock]\ncommand = %s/fl-lock --locker\n' "$BIN" >"$XDG_CONFIG_HOME/fldesktop/wm.ini"
LOG="$TMP/wm.log"
"$BIN/fl-wm" -d >"$LOG" 2>&1 &
WM=$!
for i in $(seq 1 50); do
	SOCK=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$LOG" | head -1 | cut -d= -f2)
	[ -n "$SOCK" ] && [ -S "$XDG_RUNTIME_DIR/$SOCK" ] && break
	sleep 0.2
done
[ -n "$SOCK" ] || { echo "fl-wm did not start"; cat "$LOG"; exit 1; }
export WAYLAND_DISPLAY="$SOCK"
kill -USR1 $WM
for i in $(seq 1 50); do grep -q "map: .*title=fldesktop.lock" "$LOG" && break; sleep 0.2; done
grep -q "map: .*title=fldesktop.lock" "$LOG" || { echo "the lock screen did not show"; tail -30 "$LOG"; exit 1; }
sleep 0.5
wtype -k Escape
sleep 0.5
wtype -M alt -k F4 -m alt
sleep 1
fail=0
if grep -q "session unlocked" "$LOG"; then
	echo "the session was unlocked without a password"
	fail=1
fi
grep -q "session locked" "$LOG" || { echo "the session was never locked"; fail=1; }
[ $fail = 0 ] && echo "lock screen: Escape and Alt+F4 keep it locked"
exit $fail
