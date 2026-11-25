#!/bin/sh
# Starts fl-wm headless, runs a client inside it and checks that the client
# was mapped and the compositor exits cleanly.
# usage: wm-smoke.sh <bindir>
set -u
BIN="$1"
command -v "$BIN/fl-wm" >/dev/null 2>&1 || { echo "no fl-wm"; exit 77; }
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$(mktemp -d)}"
LOG=$(mktemp)
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
# a headless (or nested) fl-wm must not hand its WAYLAND_DISPLAY to D-Bus and
# systemd: a stand-in for the tool records whether it was run
FAKE=$(mktemp -d)
printf '#!/bin/sh\necho "$@" >> "%s/called"\n' "$FAKE" >"$FAKE/dbus-update-activation-environment"
chmod +x "$FAKE/dbus-update-activation-environment"
# the client: fl-calc (any FLTK app) closes itself after a moment via timeout
PATH="$FAKE:$PATH" "$BIN/fl-wm" -d -s "timeout 3 $BIN/fl-calc" >"$LOG" 2>&1 &
WM=$!
sleep 4
kill -TERM $WM 2>/dev/null
wait $WM
STATUS=$?
if ! grep -q "running on WAYLAND_DISPLAY" "$LOG"; then
	echo "compositor did not start"; cat "$LOG"; rm -f "$LOG"; exit 1
fi
if ! grep -q "map: app_id=fl-calc" "$LOG"; then
	echo "client was not mapped"; tail -40 "$LOG"; rm -f "$LOG"; exit 1
fi
if grep -q "Assertion" "$LOG"; then
	echo "assertion failure"; grep Assertion "$LOG"; rm -f "$LOG"; exit 1
fi
if [ -e "$FAKE/called" ]; then
	echo "a headless fl-wm exported its environment: $(cat "$FAKE/called")"; rm -rf "$LOG" "$FAKE"; exit 1
fi
rm -rf "$FAKE"
# SIGTERM (logging out) ends it cleanly, without a crash
if [ "$STATUS" -ne 0 ]; then
	echo "fl-wm exited with $STATUS on SIGTERM"; tail -20 "$LOG"; rm -f "$LOG"; exit 1
fi
rm -f "$LOG"
echo "fl-wm smoke test passed (exit $STATUS)"
exit 0
