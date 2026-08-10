#!/usr/bin/env bash
# Experiment 003a+d — the CreateProcess spawn seam and the in-process attach.
#
# spawn_process() dispatches to an in-process backend when WINE_INPROC_SPAWN
# is set, else the classic fork/exec backend. This checks:
#   OFF: default fork backend — no regression, real client still runs.
#   ON : child CreateProcess attaches the child as a first-class Windows
#        process on the shared server WITHOUT fork/exec (patch 0003d):
#        the child registers via init_first_thread + init_process_done on the
#        handed socket from a thread of the parent's host process, so the
#        parent's CreateProcess succeeds and the server's own -d1 trace
#        counts the extra handshakes (the hard, server-side evidence).
#        Running the child's PE image is 0003e, not asserted here.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$(cd "$here/../wine-build" && pwd)"
wine="$build/loader/wine"
wineserver="$build/server/wineserver"
rc_test="$here/build/server/real_client_test"

fail() { echo "FAIL: $1"; exit 1; }

echo "off: fork backend, real client (no regression)"
out="$(timeout 90 "$rc_test" "$wineserver" "$wine" wineboot.exe 2>/dev/null)"
echo "$out" | grep -q "^PASS:" || fail "fork-backend real client did not pass"
echo "  ok: default fork backend unchanged"

echo "on: WINE_INPROC_SPAWN=1, children attach in-process (0003d)"
pkill -x wineserver 2>/dev/null; sleep 1
prefix="/tmp/wineforge-seam-$$"; rm -rf "$prefix"; mkdir -p "$prefix"
srvlog="$prefix.serverlog"; clientlog="$prefix.clientlog"

# prime the prefix via the fork backend so wineboot reaches child spawns
WINEPREFIX="$prefix" WINEDEBUG=-all timeout 40 "$wine" wineboot.exe >/dev/null 2>&1
WINEPREFIX="$prefix" "$wineserver" -k 2>/dev/null; sleep 1

# run a foreground wineserver with request tracing so init_first_thread
# handshakes can be counted from the server's own trace (exp-004 method)
WINEPREFIX="$prefix" "$wineserver" -f -d1 -p0 2>"$srvlog" &
srvpid=$!
sleep 1

WINE_INPROC_SPAWN=1 WINEPREFIX="$prefix" WINEDEBUG=-all \
    timeout 40 "$wine" wineboot.exe >"$clientlog" 2>&1
WINEPREFIX="$prefix" "$wineserver" -k 2>/dev/null   # in case a client hung
wait "$srvpid" 2>/dev/null

attached=$(grep -c "in-process child attached" "$clientlog")
handshakes=$(grep -c "init_first_thread( unix_pid" "$srvlog")
echo "  in-process children attached: $attached"
echo "  server-side init_first_thread handshakes: $handshakes"

rm -rf "$prefix"

[ "$attached" -ge 1 ] || { echo "--- client log ---"; cat "$clientlog"; rm -f "$srvlog" "$clientlog"; \
    fail "no in-process child completed the attach"; }
# every attached child did a real handshake the server counted, on top of
# the primary wineboot process's own
[ "$handshakes" -ge $((attached + 1)) ] || { rm -f "$srvlog" "$clientlog"; \
    fail "server trace does not account for the in-process children"; }
rm -f "$srvlog" "$clientlog"

echo "  ok: in-process children are first-class processes on the server"
echo "PASS: CreateProcess spawn seam dispatches correctly both ways"
