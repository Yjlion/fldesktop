#!/bin/sh
# Starts PipeWire, fl-wm (headless) and xdg-desktop-portal-fl on a private
# session bus and shares through the ScreenCast backend without a dialog,
# using restore_data:
#  1. the headless screen: Start answers with a PipeWire node that exists
#  2. Session.Close removes the node
#  3. an fl-calc window (wlroots 0.20+ only)
# usage: screencast-smoke.sh <bindir>
set -u
BIN="$1"
for c in dbus-run-session gdbus pipewire pw-dump; do
	command -v $c >/dev/null 2>&1 || { echo "no $c"; exit 77; }
done
[ -x "$BIN/xdg-desktop-portal-fl" ] && [ -x "$BIN/fl-wm" ] || { echo "no portal"; exit 77; }
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
"$BIN/fl-wm" -d -s "$BIN/xdg-desktop-portal-fl" >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM $PW 2>/dev/null; wait $WM $PW 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; tail -20 "$T/wm.log"; exit 1; }
P() { gdbus call --session -d org.freedesktop.impl.portal.desktop.fl -o /org/freedesktop/portal/desktop -m "$@"; }
SC=org.freedesktop.impl.portal.ScreenCast
REQ=/org/freedesktop/portal/desktop/request/1_1
SES=/org/freedesktop/portal/desktop/session/1_1
for i in $(seq 1 50); do
	pw-dump >/dev/null 2>&1 &&
		P org.freedesktop.DBus.Properties.Get $SC AvailableSourceTypes >/dev/null 2>&1 && break
	sleep 0.1
done
types=$(P org.freedesktop.DBus.Properties.Get $SC AvailableSourceTypes) || fail "no portal"
case "$types" in
"(<uint32 1>,)") WINDOWS=0 ;;
"(<uint32 3>,)") WINDOWS=1 ;;
*) fail "AvailableSourceTypes: $types" ;;
esac
P org.freedesktop.DBus.Properties.Get $SC version | grep -q "uint32 5" || fail "version"

# share: <session> <restore entry (usss)> <types>
share() {
	P $SC.CreateSession $REQ/c$1 $SES/$1 org.example.App "{}" | grep -q "uint32 0" || fail "CreateSession $1"
	P $SC.SelectSources $REQ/s$1 $SES/$1 org.example.App \
		"{'types': <uint32 $3>, 'persist_mode': <uint32 2>, 'restore_data': <('fl', uint32 1, <[$2]>)>}" |
		grep -q "uint32 0" || fail "SelectSources $1"
	P $SC.Start $REQ/t$1 $SES/$1 org.example.App "" "{}" >"$T/start$1" 2>&1 || fail "Start $1: $(cat "$T/start$1")"
	grep -q "(uint32 0," "$T/start$1" || fail "Start $1: $(cat "$T/start$1")"
	NODE=$(sed -n "s/.*'streams': <\[(uint32 \([0-9]*\),.*/\1/p" "$T/start$1")
	[ -n "$NODE" ] || fail "no node in $(cat "$T/start$1")"
	has_node "$NODE" || fail "node $NODE not in PipeWire"
}
has_node() {
	for i in $(seq 1 30); do
		pw-dump 2>/dev/null | grep -q "\"id\": $1," && return 0
		sleep 0.1
	done
	return 1
}

share 1 "(uint32 1, 'HEADLESS-1', '', '')" 1
grep -q "'restore_data'" "$T/start1" || fail "no restore_data: $(cat "$T/start1")"
grep -q "'size': <([0-9]*, [0-9]*)>" "$T/start1" || fail "size: $(cat "$T/start1")"
gdbus call --session -d org.freedesktop.impl.portal.desktop.fl -o $SES/1 \
	-m org.freedesktop.impl.portal.Session.Close >/dev/null || fail "Session.Close"
gone=0
for i in $(seq 1 30); do
	pw-dump 2>/dev/null | grep -q "\"id\": $NODE," || { gone=1; break; }
	sleep 0.1
done
[ $gone = 1 ] || fail "node $NODE still there after Close"

if [ $WINDOWS = 1 ]; then
	timeout 20 "$BIN/fl-calc" &
	for i in $(seq 1 50); do grep -q "map: app_id=fl-calc" "$T/wm.log" && break; sleep 0.1; done
	share 3 "(uint32 2, '', 'fl-calc', '')" 2
	echo "shared a window too"
fi
echo "screencast smoke test passed"
exit 0
