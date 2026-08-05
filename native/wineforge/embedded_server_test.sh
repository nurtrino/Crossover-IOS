#!/usr/bin/env bash
# Experiment 002b — enforce the embedded-server model (patch 0002).
#
# With WINE_EMBEDDED_SERVER set, the real Wine client must never fork its own
# wineserver (impossible on iOS). Two directions:
#   POSITIVE: an in-thread server is present -> the client runs normally.
#   NEGATIVE: no server present -> the client fails fast with a clear error
#             and forks NO wineserver process.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
wine="$(cd "$here/../wine-build" && pwd)/loader/wine"
wineserver="$(cd "$here/../wine-build" && pwd)/server/wineserver"
rc_test="$here/build/server/real_client_test"

fail() { echo "FAIL: $1"; exit 1; }

# --- POSITIVE ------------------------------------------------------------
echo "positive: real client + in-thread server + WINE_EMBEDDED_SERVER=1"
out="$(WINE_EMBEDDED_SERVER=1 timeout 90 "$rc_test" "$wineserver" "$wine" wineboot.exe 2>/dev/null)"
echo "$out" | grep -q "^PASS:" || fail "client did not run against the in-thread server"
echo "  ok: client ran on the in-thread server"

# --- NEGATIVE ------------------------------------------------------------
echo "negative: real client, NO server, WINE_EMBEDDED_SERVER=1"
prefix="/tmp/wineforge-neg-$$"
rm -rf "$prefix"; mkdir -p "$prefix"

# snapshot wineserver processes actually executing the built binary
count_servers() { pgrep -x wineserver 2>/dev/null | wc -l | tr -d ' '; }
before="$(count_servers)"

out="$(WINE_EMBEDDED_SERVER=1 WINEPREFIX="$prefix" WINEDEBUG=-all \
        timeout 30 "$wine" wineboot.exe 2>&1)"
rc=$?

echo "$out" | grep -qi "no embedded wineserver present" \
    || fail "expected 'no embedded wineserver present' message; got: $out"
[ "$rc" -ne 0 ] || fail "client should have failed (exit != 0), got $rc"

sleep 1
after="$(count_servers)"
[ "$after" -le "$before" ] || fail "a wineserver was forked ($before -> $after)"
echo "  ok: failed fast (exit $rc), no wineserver forked ($before -> $after)"

rm -rf "$prefix"
echo "PASS: embedded-server model enforced in both directions"
