#!/bin/sh
# Starts fl-wm headless with a fake input method (fl-im) and fl-edit, which
# speaks text-input-unstable-v3:
#  1. the input method is activated when fl-edit's text has the focus and
#     its text reaches fl-edit
#  2. with --grab, typed keys go to the input method and not to fl-edit
# usage: im-smoke.sh <bindir> <fl-im>
set -u
BIN="$1"
IM="$2"
[ -x "$BIN/fl-wm" ] && [ -x "$BIN/fl-edit" ] || { echo "no fl-wm/fl-edit"; exit 77; }
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

# run <name> [--grab]: fl-im and fl-edit for 5 s, typing "ab" after 2 s
run() {
	"$IM" ${2:-} 5 "日本語" >"$T/$1.im" 2>&1 &
	imp=$!
	for i in $(seq 30); do grep -q ready "$T/$1.im" && break; sleep 0.1; done
	WAYLAND_DEBUG=1 timeout 4 "$BIN/fl-edit" "$T/$1.txt" 2>"$T/$1.edit" &
	sleep 2
	wtype ab
	wait $imp
	sleep 0.3
}

run plain
grep -q '^activate' "$T/plain.im" || fail "the input method was not activated: $(cat "$T/plain.im")"
grep -q 'commit_string("日本語")' "$T/plain.edit" || fail "fl-edit got no commit_string"
grep -q '^key' "$T/plain.im" && fail "keys went to the input method without a grab"
grep -q 'wl_keyboard#[0-9]*\.key(' "$T/plain.edit" || fail "fl-edit got no keys"

run grabbed --grab
grep -q '^key' "$T/grabbed.im" || fail "the grab got no keys: $(cat "$T/grabbed.im")"
grep -q 'wl_keyboard#[0-9]*\.key(' "$T/grabbed.edit" && fail "keys reached fl-edit during the grab"

# a second input method is refused
"$IM" 1 x >"$T/one.im" 2>&1 &
first=$!
sleep 0.3
"$IM" 0.5 y >"$T/two.im" 2>&1
wait $first
kill -0 $WM 2>/dev/null || fail "fl-wm died"
grep -q unavailable "$T/two.im" || fail "a second input method was accepted"
echo "input method smoke test passed"
exit 0
