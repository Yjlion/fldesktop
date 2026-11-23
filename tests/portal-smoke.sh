#!/bin/sh
# Starts fl-wm headless with fl-notifyd and xdg-desktop-portal-fl on a
# private session bus and calls the portal backend directly: Settings,
# Notification (forwarded to fl-notifyd), Request.Close of a file chooser,
# Background (autostart), Inhibit, GlobalShortcuts and RemoteDesktop.
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
# Background: autostart entries
v=$(P org.freedesktop.impl.portal.Background.EnableAutostart org.example.App true "['example', '--hidden']" 0) ||
	fail "EnableAutostart"
[ "$v" = "(true,)" ] || fail "EnableAutostart: $v"
grep -qx "Exec=example --hidden" "$T/config/autostart/org.example.App.desktop" || fail "no autostart entry"
P org.freedesktop.impl.portal.Background.EnableAutostart org.example.App false "['example']" 0 >/dev/null
[ -e "$T/config/autostart/org.example.App.desktop" ] && fail "autostart entry not removed"
v=$(P org.freedesktop.impl.portal.Background.NotifyBackground /org/freedesktop/portal/desktop/request/1_1/b1 \
	org.example.App "Example") || fail "NotifyBackground"
echo "$v" | grep -q "'result': <uint32 1>" || fail "NotifyBackground: $v"
# Inhibit: the idle inhibitor reaches fl-wm, and goes with the request
P org.freedesktop.impl.portal.Inhibit.Inhibit /org/freedesktop/portal/desktop/request/1_1/i1 org.example.App "" 8 \
	"{'reason': <'Playing'>}" >/dev/null || fail "Inhibit"
gdbus call --session -d org.freedesktop.impl.portal.desktop.fl -o /org/freedesktop/portal/desktop/request/1_1/i1 \
	-m org.freedesktop.impl.portal.Request.Close >/dev/null || fail "Inhibit: Request.Close"
# GlobalShortcuts: a stored shortcut is listed and activating it sends Activated
mkdir -p "$T/config/fldesktop"
printf '[org.example.App]\ntoggle=Ctrl+Alt+k|Show or hide\n' >"$T/config/fldesktop/global-shortcuts.ini"
P org.freedesktop.impl.portal.GlobalShortcuts.CreateSession /org/freedesktop/portal/desktop/request/1_1/g1 \
	/org/freedesktop/portal/desktop/session/1_1/g1 org.example.App "{}" >/dev/null || fail "GlobalShortcuts.CreateSession"
v=$(P org.freedesktop.impl.portal.GlobalShortcuts.ListShortcuts /org/freedesktop/portal/desktop/request/1_1/g2 \
	/org/freedesktop/portal/desktop/session/1_1/g1) || fail "ListShortcuts"
echo "$v" | grep -q "'trigger_description': <'Ctrl+Alt+k'>" || fail "ListShortcuts: $v"
v=$(P org.freedesktop.impl.portal.GlobalShortcuts.BindShortcuts /org/freedesktop/portal/desktop/request/1_1/g3 \
	/org/freedesktop/portal/desktop/session/1_1/g1 "[('toggle', {'description': <'Show or hide'>})]" "" "{}") ||
	fail "BindShortcuts"
echo "$v" | grep -q "uint32 0" || fail "BindShortcuts (known shortcuts ask nothing): $v"
gdbus monitor --session -d org.freedesktop.impl.portal.desktop.fl >"$T/signals" 2>&1 &
MON=$!
sleep 0.5
"$BIN/xdg-desktop-portal-fl" --activate-shortcut org.example.App toggle || fail "--activate-shortcut"
sleep 0.5
kill $MON 2>/dev/null
grep -q "GlobalShortcuts.Activated.*'toggle'" "$T/signals" || fail "no Activated signal: $(cat "$T/signals")"
# RemoteDesktop: what it offers
v=$(gdbus call --session -d org.freedesktop.impl.portal.desktop.fl -o /org/freedesktop/portal/desktop \
	-m org.freedesktop.DBus.Properties.Get org.freedesktop.impl.portal.RemoteDesktop AvailableDeviceTypes) ||
	fail "RemoteDesktop properties"
[ "$v" = "(<uint32 3>,)" ] || fail "AvailableDeviceTypes: $v"
echo "portal smoke test passed"
exit 0
