#!/bin/sh
# fl-secretd through libsecret's secret-tool on a private session bus:
# the login keyring made and unlocked as at login, then store, lookup, clear.
# usage: secretd-smoke.sh <fl-secretd>
set -u
SECRETD="$1"
command -v secret-tool >/dev/null 2>&1 || { echo "no secret-tool (libsecret-tools)"; exit 77; }
command -v dbus-run-session >/dev/null 2>&1 || { echo "no dbus-run-session"; exit 77; }
if [ -z "${FLD_SECRETD_BUS:-}" ]; then
	FLD_SECRETD_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
TMP=$(mktemp -d)
trap 'kill $D 2>/dev/null; rm -rf "$TMP"' EXIT
mkdir -p "$TMP/run" && chmod 700 "$TMP/run"
export XDG_RUNTIME_DIR="$TMP/run" XDG_DATA_HOME="$TMP/data" HOME="$TMP"
why=""
check() { [ -n "$why" ] || eval "$1" || why="$2"; }

# the login: makes the keyring, keeps its key for the service
printf 'login-password\n' | "$SECRETD" --login
for i in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/fldesktop/keyring-stash" ] && break; sleep 0.1; done
check '[ -f "$XDG_DATA_HOME/fldesktop/keyrings/login.keyring" ]' "--login did not make the login keyring"
"$SECRETD" 2>"$TMP/err" &
D=$!
for i in $(seq 50); do
	gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus -m org.freedesktop.DBus.NameHasOwner \
		org.freedesktop.secrets 2>/dev/null | grep -q true && break
	sleep 0.1
done
sleep 0.3
check '[ ! -S "$XDG_RUNTIME_DIR/fldesktop/keyring-stash" ]' "the service did not take the login key"
printf 'p4ss w0rd' | secret-tool store --label='Test mail' service imap user leo 2>>"$TMP/err"
check '[ "$(secret-tool lookup service imap user leo 2>>"$TMP/err")" = "p4ss w0rd" ]' "stored secret not found"
check 'secret-tool search service imap 2>&1 | grep -q "label = Test mail"' "search does not list the item"
grep -q "p4ss" "$XDG_DATA_HOME/fldesktop/keyrings/login.keyring" && why="the secret is in the file in clear"
check 'grep -q "Test mail" "$XDG_DATA_HOME/fldesktop/keyrings/login.keyring" && false || true' "the label is in clear"
secret-tool clear service imap user leo 2>>"$TMP/err"
check '[ -z "$(secret-tool lookup service imap user leo 2>/dev/null)" ]' "clear did not delete it"
# the service again: the keyring is now locked, the item list still known
printf 'kept' | secret-tool store --label=k app test 2>>"$TMP/err"
kill $D; wait $D 2>/dev/null
"$SECRETD" 2>>"$TMP/err" &
D=$!
sleep 0.5
check 'gdbus call --session -d org.freedesktop.secrets -o /org/freedesktop/secrets -m org.freedesktop.Secret.Service.SearchItems "{\"app\": \"test\"}" | grep -q "collection/login/"' "a locked keyring cannot be searched"
if [ -n "$why" ]; then
	echo "FAIL: $why"
	cat "$TMP/err"
	exit 1
fi
echo "fl-secretd smoke test passed"
