#!/bin/sh
# Tiling and the session file in a headless session: with tiling=true two
# fl-charmap windows share the output side by side (the older one on the
# left), and with restore_session=true fl-wm writes them to session.ini at
# the end.
# usage: tile-smoke.sh <bindir>
set -u
BIN="$1"
[ -x "$BIN/fl-wm" ] && [ -x "$BIN/fl-charmap" ] || { echo "no fl-wm or fl-charmap"; exit 77; }
T=$(mktemp -d)
trap 'kill $WM $A $B 2>/dev/null; rm -rf "$T"' EXIT
export XDG_RUNTIME_DIR=$T/run HOME=$T/home XDG_CONFIG_HOME=$T/home/.config XDG_STATE_HOME=$T/state
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/fldesktop" && chmod 700 "$XDG_RUNTIME_DIR"
printf '[autostart]\nreset=true\n[behaviour]\ntiling=true\ntile_gap=6\nrestore_session=true\n' >"$XDG_CONFIG_HOME/fldesktop/wm.ini"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d >"$T/wm.log" 2>&1 &
WM=$!
for i in $(seq 1 50); do
	SOCK=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$T/wm.log" | head -1 | cut -d= -f2)
	[ -n "$SOCK" ] && [ -S "$XDG_RUNTIME_DIR/$SOCK" ] && break
	sleep 0.2
done
export WAYLAND_DISPLAY="$SOCK"
"$BIN/fl-charmap" & A=$!
for i in $(seq 1 50); do [ "$(grep -c 'map: app_id=fl-charmap' "$T/wm.log")" -ge 1 ] && break; sleep 0.2; done
"$BIN/fl-charmap" & B=$!
for i in $(seq 1 50); do [ "$(grep -c 'map: app_id=fl-charmap' "$T/wm.log")" -ge 2 ] && break; sleep 0.2; done
sleep 0.5
kill -USR2 $WM
sleep 0.5
fail=0
# topmost first: the newer window (right), then the older (left)
right=$(sed -n 1p "$XDG_RUNTIME_DIR/fl-wm-windows")
left=$(sed -n 2p "$XDG_RUNTIME_DIR/fl-wm-windows")
case "$left" in "6 6 "*) ;; *) echo "the first window is at '$left', expected 6 6 ..."; fail=1 ;; esac
lx=$(echo "$left" | cut -d' ' -f1); lw=$(echo "$left" | cut -d' ' -f3); rx=$(echo "$right" | cut -d' ' -f1)
[ "$rx" -eq $((lx + lw + 6)) ] || { echo "the second window is at '$right', not beside the first ('$left')"; fail=1; }
kill -TERM $WM
wait $WM 2>/dev/null
n=$(grep -c '^app_id=fl-charmap' "$XDG_STATE_HOME/fldesktop/session.ini" 2>/dev/null)
[ "$n" = 2 ] || { echo "session.ini has $n windows, expected 2"; fail=1; }
[ $fail = 0 ] && echo "tiling: side by side ($left | $right); session saved"
exit $fail
