#!/bin/sh
# test_fprint against a fake fprintd (tests/tools/fake-fprintd.c) on a
# private bus that stands in for the system bus.
# usage: fprint-mock.sh <test_fprint> <fake-fprintd>
set -u
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
if [ -z "${FLD_IN_BUS:-}" ]; then
	FLD_IN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
"$2" &
FAKE=$!
trap 'kill $FAKE 2>/dev/null' EXIT
for i in $(seq 1 50); do
	gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus -m org.freedesktop.DBus.GetNameOwner \
		net.reactivated.Fprint >/dev/null 2>&1 && break
	sleep 0.1
done
DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" FLD_FPRINT_MOCK=1 "$1"
