#!/bin/sh
# Runs test_backlight with a mocked UPower keyboard backlight (3 levels, at 1)
# on a private bus; the screens use a fake sysfs (FLD_SYSFS_ROOT, in the test).
# usage: backlight-mock.sh <test_backlight>
set -u
PY=${FLD_TEST_PYTHON:-python3}
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
"$PY" -c "import dbusmock" 2>/dev/null || { echo "python-dbusmock not installed"; exit 77; }
if [ -z "${FLD_IN_BUS:-}" ]; then
	FLD_IN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
"$PY" -m dbusmock --session org.freedesktop.UPower /org/freedesktop/UPower/KbdBacklight \
	org.freedesktop.UPower.KbdBacklight >/dev/null 2>&1 &
MOCK=$!
trap 'kill $MOCK 2>/dev/null' EXIT
OBJ="-d org.freedesktop.UPower -o /org/freedesktop/UPower/KbdBacklight"
for i in $(seq 1 50); do
	gdbus call --session $OBJ -m org.freedesktop.DBus.Mock.AddProperty \
		org.freedesktop.UPower.KbdBacklight Level '<int32 1>' >/dev/null 2>&1 && break
	sleep 0.1
done
gdbus call --session $OBJ -m org.freedesktop.DBus.Mock.AddMethods org.freedesktop.UPower.KbdBacklight \
	"[('GetMaxBrightness', '', 'i', 'ret = 3'),
	  ('GetBrightness', '', 'i', 'ret = self.Get(\"org.freedesktop.UPower.KbdBacklight\", \"Level\")'),
	  ('SetBrightness', 'i', '', 'self.Set(\"org.freedesktop.UPower.KbdBacklight\", \"Level\", dbus.Int32(args[0]))')]" \
	>/dev/null || exit 1
FLD_KBD_MOCK=1 "$1"
