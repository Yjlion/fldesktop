#!/bin/sh
# Starts fl-wm headless with fl-notifyd and xdg-desktop-portal-fl on a
# private session bus and calls the portal backend directly: Settings,
# Notification (forwarded to fl-notifyd), Request.Close of a file chooser.
# usage: portal-smoke.sh <bindir>
set -u
BIN="$1"
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
command -v gdbus >/dev/null 2>&1 || { echo "no gdbus"; exit 77; }
[ -x "$BIN/xdg-desktop-portal-fl" ] && [ -x "$BIN/fl-notifyd" ] || { echo "no portal"; exit 77; }
if [ -z "${FLD_IN_BUS:-}" ]; then
	FLD_IN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
T=$(mktemp -d)
mkdir -p "$T/run" "$T/config/fldesktop"
chmod 700 "$T/run"
printf '[Appearance]\nTheme=dark\nAccent=#ff0000\n' >"$T/config/fldesktop/desktop.ini"
export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" HOME="$T"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
"$BIN/fl-wm" -d -s "$BIN/fl-notifyd & $BIN/xdg-desktop-portal-fl" >"$T/wm.log" 2>&1 &
WM=$!
trap 'kill $WM 2>/dev/null; wait $WM 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; tail -20 "$T/wm.log"; exit 1; }
P() { gdbus call --session -d org.freedesktop.impl.portal.desktop.fl -o /org/freedesktop/portal/desktop -m "$@"; }
for i in $(seq 1 50); do
	P org.freedesktop.impl.portal.Settings.Read org.freedesktop.appearance color-scheme >/dev/null 2>&1 && break
	sleep 0.1
done
# fl-notifyd starts in parallel and takes its name a little later
for i in $(seq 1 50); do
	gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus -m org.freedesktop.DBus.GetNameOwner \
		org.freedesktop.Notifications >/dev/null 2>&1 && break
	sleep 0.1
done
v=$(P org.freedesktop.impl.portal.Settings.Read org.freedesktop.appearance color-scheme) || fail "no portal"
[ "$v" = "(<uint32 1>,)" ] || fail "color-scheme: $v"
v=$(P org.freedesktop.impl.portal.Settings.ReadAll '["org.freedesktop.appearance"]')
echo "$v" | grep -q "'accent-color': <(1.0, 0.0, 0.0)>" || fail "ReadAll: $v"
P org.freedesktop.impl.portal.Settings.Read org.example nothing >/dev/null 2>&1 && fail "unknown setting was found"
P org.freedesktop.impl.portal.Notification.AddNotification org.example.App n1 \
	"{'title': <'From a portal'>, 'body': <'Hello'>, 'priority': <'high'>, 'default-action': <'app.open'>}" >/dev/null ||
	fail "AddNotification"
sleep 0.5
h=$(gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications \
	-m org.fldesktop.Notifications.GetHistory)
echo "$h" | grep -q "'From a portal'" || fail "notification not forwarded: $h"
# a file chooser that is closed through its request object
P org.freedesktop.impl.portal.FileChooser.OpenFile /org/freedesktop/portal/desktop/request/1_1/t1 org.example.App "" \
	"Open" "{}" >"$T/open.out" 2>&1 &
CALL=$!
sleep 1.5
grep -q "map: .*Open" "$T/wm.log" || fail "file chooser not shown"
gdbus call --session -d org.freedesktop.impl.portal.desktop.fl -o /org/freedesktop/portal/desktop/request/1_1/t1 \
	-m org.freedesktop.impl.portal.Request.Close >/dev/null || fail "Request.Close"
wait $CALL
grep -q "(uint32 1, @a{sv} {})" "$T/open.out" || fail "file chooser answer: $(cat "$T/open.out")"
echo "portal smoke test passed"
exit 0
