#!/bin/sh
# Records the headless screen for 3 seconds: PipeWire and WirePlumber, fl-wm
# with xdg-desktop-portal-fl (its ScreenCast backend gives the node, through
# restore_data so no dialog), then fl-snip --record-node. The file must be
# a WebM of about 3 seconds.
# usage: record-smoke.sh <bindir>
set -u
BIN="$1"
for c in dbus-run-session gdbus pipewire wireplumber gst-inspect-1.0; do
	command -v $c >/dev/null 2>&1 || { echo "no $c"; exit 77; }
done
for e in pipewiresrc vp8enc webmmux; do
	gst-inspect-1.0 $e >/dev/null 2>&1 || { echo "no GStreamer $e"; exit 77; }
done
[ -x "$BIN/xdg-desktop-portal-fl" ] && [ -x "$BIN/fl-wm" ] && [ -x "$BIN/fl-snip" ] || { echo "no portal/fl-snip"; exit 77; }
"$BIN/fl-snip" --help | grep -q -- --record || { echo "fl-snip without recording"; exit 77; }
if [ -z "${FLD_IN_BUS:-}" ]; then
	FLD_IN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
T=$(mktemp -d)
mkdir -p "$T/run" "$T/config"
chmod 700 "$T/run"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" HOME="$T"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
pipewire >"$T/pw.log" 2>&1 &
PW=$!
sleep 0.3
wireplumber >"$T/wp.log" 2>&1 &
WP=$!
"$BIN/fl-wm" -d -s "$BIN/xdg-desktop-portal-fl" >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM $WP $PW 2>/dev/null; wait $WM $WP $PW 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; tail -5 "$T/rec.err" 2>/dev/null; exit 1; }
P() { gdbus call --session -d org.freedesktop.impl.portal.desktop.fl -o /org/freedesktop/portal/desktop -m "$@"; }
SC=org.freedesktop.impl.portal.ScreenCast
REQ=/org/freedesktop/portal/desktop/request/1_1
SES=/org/freedesktop/portal/desktop/session/1_1/r
for i in $(seq 1 50); do
	P org.freedesktop.DBus.Properties.Get $SC version >/dev/null 2>&1 && break
	sleep 0.1
done
P $SC.CreateSession $REQ/c $SES org.example.App "{}" >/dev/null || fail "CreateSession"
P $SC.SelectSources $REQ/s $SES org.example.App \
	"{'types': <uint32 1>, 'restore_data': <('fl', uint32 1, <[(uint32 1, 'HEADLESS-1', '', '')]>)>}" >/dev/null ||
	fail "SelectSources"
out=$(P $SC.Start $REQ/t $SES org.example.App "" "{}") || fail "Start"
NODE=$(echo "$out" | sed -n "s/.*'streams': <\[(uint32 \([0-9]*\),.*/\1/p")
[ -n "$NODE" ] || fail "no node: $out"
WAYLAND_DISPLAY=wayland-0 "$BIN/fl-snip" --record-node "$NODE" --seconds 3 --no-window -o "$T/out.webm" \
	>"$T/rec.out" 2>"$T/rec.err" || fail "fl-snip --record-node failed"
[ "$(cat "$T/rec.out")" = "$T/out.webm" ] || fail "printed path: $(cat "$T/rec.out")"
[ -s "$T/out.webm" ] || fail "no file"
# a WebM starts with the EBML magic number
[ "$(head -c 4 "$T/out.webm" | od -An -tx1 | tr -d ' \n')" = "1a45dfa3" ] || fail "not a WebM file"
if command -v gst-discoverer-1.0 >/dev/null 2>&1; then
	d=$(gst-discoverer-1.0 "$T/out.webm" 2>/dev/null | sed -n 's/.*Duration: 0:00:0\([0-9]\).*/\1/p' | head -1)
	[ -n "$d" ] && [ "$d" -ge 2 ] && [ "$d" -le 4 ] || fail "duration: $(gst-discoverer-1.0 "$T/out.webm" | grep -i duration)"
fi
echo "recording smoke test passed ($(wc -c <"$T/out.webm") bytes)"
exit 0
