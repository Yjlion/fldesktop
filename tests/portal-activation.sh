#!/bin/sh
# Starts the real xdg-desktop-portal on a private session bus, with
# xdg-desktop-portal-fl D-Bus activated as its only backend, and reads a
# setting through the frontend. The backend opens a Wayland display, and
# libdecor then calls the frontend while the frontend is waiting for the
# backend; this checks that the two do not wait for each other until the
# D-Bus timeout (the frontend would then go on without us).
# usage: portal-activation.sh <bindir> [<srcdir>]
set -u
BIN="$1"
SRC="${2:-$(dirname "$0")/..}"
FRONTEND=${FLD_XDP:-}
for f in /usr/libexec/xdg-desktop-portal /usr/lib/xdg-desktop-portal; do
	[ -z "$FRONTEND" ] && [ -x "$f" ] && FRONTEND=$f
done
[ -n "$FRONTEND" ] || { echo "no xdg-desktop-portal"; exit 77; }
for c in dbus-run-session dbus-update-activation-environment gdbus; do
	command -v $c >/dev/null 2>&1 || { echo "no $c"; exit 77; }
done
PORTAL=${FLD_PORTAL:-$BIN/xdg-desktop-portal-fl}
[ -x "$PORTAL" ] && [ -x "$BIN/fl-wm" ] || { echo "no portal"; exit 77; }

T=$(mktemp -d)
cleanup() { [ -n "${WM:-}" ] && kill $WM 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
mkdir -p "$T/run" "$T/services" "$T/portals" "$T/config/fldesktop" "$T/config/xdg-desktop-portal"
chmod 700 "$T/run"
printf '[Appearance]\nTheme=dark\n' >"$T/config/fldesktop/desktop.ini"
cp "$SRC/data/portal/fl.portal" "$T/portals/"
printf '[preferred]\ndefault=fl;\n' >"$T/config/xdg-desktop-portal/fldesktop-portals.conf"
printf '[D-BUS Service]\nName=org.freedesktop.portal.Desktop\nExec=%s\n' "$FRONTEND" \
	>"$T/services/org.freedesktop.portal.Desktop.service"
printf '[D-BUS Service]\nName=org.freedesktop.impl.portal.desktop.fl\nExec=%s\n' "$PORTAL" \
	>"$T/services/org.freedesktop.impl.portal.desktop.fl.service"
cat >"$T/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:dir=$T/run</listen>
  <servicedir>$T/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF

export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/config" HOME="$T"
export XDG_CURRENT_DESKTOP=fldesktop XDG_DESKTOP_PORTAL_DIR="$T/portals"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
unset WAYLAND_DISPLAY DISPLAY

dbus-run-session --config-file="$T/bus.conf" -- sh -c '
	"$1/fl-wm" -d -s "dbus-update-activation-environment WAYLAND_DISPLAY && touch $2/ready" >"$2/wm.log" 2>&1 &
	WM=$!
	for i in $(seq 1 50); do [ -e "$2/ready" ] && break; sleep 0.1; done
	[ -e "$2/ready" ] || { echo "FAIL: fl-wm did not start"; tail -20 "$2/wm.log"; kill $WM; exit 1; }
	start=$(date +%s)
	v=$(gdbus call --session --timeout 20 -d org.freedesktop.portal.Desktop -o /org/freedesktop/portal/desktop \
		-m org.freedesktop.portal.Settings.ReadOne org.freedesktop.appearance color-scheme 2>&1)
	took=$(( $(date +%s) - start ))
	kill $WM 2>/dev/null
	if [ "$v" != "(<uint32 1>,)" ]; then echo "FAIL: color-scheme through the frontend: $v (${took}s)"; exit 1; fi
	if [ $took -gt 10 ]; then echo "FAIL: the frontend took ${took}s to reach xdg-desktop-portal-fl"; exit 1; fi
	echo "portal activation test passed (${took}s)"
' sh "$BIN" "$T"
