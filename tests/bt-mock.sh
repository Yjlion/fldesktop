#!/bin/sh
# Runs test_bt against python-dbusmock's BlueZ template on a private bus.
# usage: bt-mock.sh <test_bt>   (FLD_TEST_PYTHON picks the python with dbusmock)
set -u
PY=${FLD_TEST_PYTHON:-python3}
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
"$PY" -c "import dbusmock" 2>/dev/null || { echo "python-dbusmock not installed"; exit 77; }
if [ -z "${FLD_IN_BUS:-}" ]; then
	FLD_IN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
"$PY" -m dbusmock --session --template bluez5 >/dev/null 2>&1 &
MOCK=$!
trap 'kill $MOCK 2>/dev/null' EXIT
for i in $(seq 1 50); do
	gdbus call --session -d org.bluez -o / -m org.bluez.Mock.AddAdapter hci0 fl-test >/dev/null 2>&1 && break
	sleep 0.1
done
gdbus call --session -d org.bluez -o / -m org.bluez.Mock.AddDevice hci0 11:22:33:44:55:66 "Headphones" >/dev/null
gdbus call --session -d org.bluez -o / -m org.bluez.Mock.AddDevice hci0 22:33:44:55:66:77 "Mouse" >/dev/null
gdbus call --session -d org.bluez -o / -m org.bluez.Mock.PairDevice hci0 22:33:44:55:66:77 >/dev/null
FLD_BT_MOCK=1 FLD_BLUEZ_BUS=session "$1"
