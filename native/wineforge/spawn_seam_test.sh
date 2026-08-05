#!/usr/bin/env bash
# Experiment 003a — the CreateProcess spawn seam (patch 0003, stage a).
#
# spawn_process() now dispatches to an in-process backend when
# WINE_INPROC_SPAWN is set, else the classic fork/exec backend. This checks:
#   OFF: default fork backend — no regression, real client still runs.
#   ON : child CreateProcess reaches spawn_process_inproc (the wired but
#        not-yet-implemented seam), returning STATUS_NOT_IMPLEMENTED.
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

echo "on: WINE_INPROC_SPAWN=1, child spawn reaches the in-process seam"
pkill -x wineserver 2>/dev/null; sleep 1
prefix="/tmp/wineforge-seam-$$"; rm -rf "$prefix"; mkdir -p "$prefix"
# prime the prefix via the fork backend so wineboot reaches child spawns
WINEPREFIX="$prefix" WINEDEBUG=-all timeout 40 "$wine" wineboot.exe >/dev/null 2>&1
# run with the in-process backend and capture the err channel
err="$(WINE_INPROC_SPAWN=1 WINEPREFIX="$prefix" WINEDEBUG=+err \
        timeout 40 "$wine" wineboot.exe 2>&1)"
rm -rf "$prefix"
echo "$err" | grep -q "spawn_process_inproc in-process spawn" \
    || fail "in-process backend was not reached with WINE_INPROC_SPAWN=1"
echo "  ok: in-process seam dispatched (STATUS_NOT_IMPLEMENTED until 0003c/d)"

echo "PASS: CreateProcess spawn seam dispatches correctly both ways"
