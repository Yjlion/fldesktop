#!/bin/sh
# A gpg-agent style session through pinentry-fldesktop --test-autoreply.
# usage: pinentry-session.sh <pinentry-fldesktop>
set -u
out=$(printf 'SETDESC Unlock%%0Athe key\nSETPROMPT Passphrase:\nGETPIN\nBYE\n' |
	FLD_PINENTRY_TEST_PIN='pa%ss' "$1" --test-autoreply)
expected='OK Pleased to meet you
OK
OK
D pa%25ss
OK
OK closing connection'
if [ "$out" != "$expected" ]; then
	echo "FAIL: unexpected reply:"
	echo "$out"
	exit 1
fi
# cancelled: no PIN given
out=$(printf 'GETPIN\nBYE\n' | FLD_PINENTRY_TEST_PIN= "$1" --test-autoreply | sed -n 2p)
case "$out" in
"ERR 83886179 "*) ;;
*) echo "FAIL: a cancelled GETPIN answered: $out"; exit 1 ;;
esac
echo "pinentry session test passed"
