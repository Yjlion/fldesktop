#!/bin/sh
# Starts fl-wm headless with fl-notifyd on a private session bus, sends
# notifications over D-Bus and checks the server's answers, the history and
# the NotificationClosed signal.
# usage: notify-smoke.sh <bindir>
set -u
BIN="$1"
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
command -v gdbus >/dev/null 2>&1 || { echo "no gdbus"; exit 77; }
[ -x "$BIN/fl-notifyd" ] || { echo "no fl-notifyd"; exit 77; }
if [ -z "${FLD_IN_BUS:-}" ]; then
	FLD_IN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
T=$(mktemp -d)
mkdir -p "$T/run" "$T/config"
chmod 700 "$T/run"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d -s "$BIN/fl-notifyd" >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM 2>/dev/null; wait $WM 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; tail -30 "$T/wm.log"; exit 1; }
call() { gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications -m "$@"; }
for i in $(seq 1 50); do
	call org.freedesktop.Notifications.GetServerInformation >/dev/null 2>&1 && break
	sleep 0.1
done
info=$(call org.freedesktop.Notifications.GetServerInformation) || fail "no server"
echo "$info" | grep -q "'fl-notifyd', 'fldesktop'" || fail "server information: $info"
caps=$(call org.freedesktop.Notifications.GetCapabilities)
echo "$caps" | grep -q "'actions'" || fail "capabilities: $caps"
gdbus monitor --session -d org.freedesktop.Notifications >"$T/signals" 2>&1 &
MON=$!
sleep 0.3
id=$(call org.freedesktop.Notifications.Notify test 0 dialog-information "Hello" "<b>World</b>" '["default","Open"]' '{}' 0 |
	sed -n 's/.*uint32 \([0-9]*\).*/\1/p')
[ -n "$id" ] && [ "$id" -gt 0 ] || fail "Notify returned '$id'"
id2=$(call org.freedesktop.Notifications.Notify test "$id" "" "Hello again" "" '[]' '{}' 0 | sed -n 's/.*uint32 \([0-9]*\).*/\1/p')
[ "$id2" = "$id" ] || fail "replaces_id gave $id2, wanted $id"
sleep 1
grep -q "map: .*fldesktop.notification" "$T/wm.log" || fail "popup window was not mapped"
hist=$(call org.fldesktop.Notifications.GetHistory)
echo "$hist" | grep -q "'Hello again'" || fail "history: $hist"
call org.freedesktop.Notifications.CloseNotification "$id" >/dev/null || fail "CloseNotification"
sleep 0.5
kill $MON 2>/dev/null
grep -q "NotificationClosed (uint32 $id, uint32 3)" "$T/signals" || { cat "$T/signals"; fail "no NotificationClosed signal"; }
call org.fldesktop.Notifications.SetDoNotDisturb true >/dev/null
call org.fldesktop.Notifications.GetHistory | grep -q "], true)" || fail "do not disturb not set"
grep -q "DoNotDisturb=true" "$T/config/fldesktop/notifications.ini" 2>/dev/null || fail "do not disturb not saved"
call org.fldesktop.Notifications.Clear >/dev/null
call org.fldesktop.Notifications.GetHistory | grep -q "(@a(ussssx) \[\]" || fail "history not cleared"
echo "notification smoke test passed"
exit 0
