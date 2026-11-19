#!/bin/sh
# Runs test_power against python-dbusmock's UPower template on a private bus.
# usage: upower-mock.sh <test_power>   (FLD_TEST_PYTHON picks the python with dbusmock)
set -u
PY=${FLD_TEST_PYTHON:-python3}
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
"$PY" -c "import dbusmock" 2>/dev/null || { echo "python-dbusmock not installed"; exit 77; }
if [ -z "${FLD_IN_BUS:-}" ]; then
	FLD_IN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
"$PY" -m dbusmock --session --template upower -p '{"OnBattery": true, "DaemonVersion": "1.90.2"}' >/dev/null 2>&1 &
MOCK=$!
trap 'kill $MOCK 2>/dev/null' EXIT
for i in $(seq 1 50); do
	gdbus call --session -d org.freedesktop.UPower -o /org/freedesktop/UPower \
		-m org.freedesktop.DBus.Mock.AddDischargingBattery mock_BAT "Mock Battery" 30.0 1200 >/dev/null 2>&1 && break
	sleep 0.1
done
# type battery, state discharging, 30 %, 20 Wh of 66, 10 W, 20 min to empty
gdbus call --session -d org.freedesktop.UPower -o /org/freedesktop/UPower \
	-m org.freedesktop.DBus.Mock.SetupDisplayDevice 2 2 30.0 20.0 66.0 10.0 1200 0 true battery-low-symbolic 1 >/dev/null || exit 1
FLD_UPOWER_MOCK=1 FLD_UPOWER_BUS=session "$1"
