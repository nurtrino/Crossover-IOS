#!/usr/bin/env bash
# FORKLESS GATE — the iOS runtime model demonstrated on Linux.
#
# Requires a build made with the fork/exec surface compiled out:
#     cd ../wine-build
#     touch ../wine/dlls/ntdll/unix/{process.c,loader.c} ../wine/server/request.c
#     make CC="cc -DWINE_FORKLESS"
# (Re-touch + plain `make` restores the default build. NOT part of
# `make test-real` because it needs that variant build; run by hand or in CI.)
#
# What it proves: with fork/exec REMOVED AT COMPILE TIME (patch 0005 guards:
# spawn_process_fork, exec_wineloader, __wine_unix_spawnvp, fork_and_exec,
# start_server, server daemonize), a GUI program still runs on a warm prefix —
# every Windows process a thread group in ONE host process, the server started
# externally (on iOS it is a thread in the app; wineforge embed model).
# This is M1's "all fork/exec compiled out" bar, and the exact runtime shape
# an iPad build must have: prepared prefix + embedded server + in-process
# children, no fork anywhere.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$(cd "$here/../wine-build" && pwd)"
wine="$build/loader/wine"
wineserver="$build/server/wineserver"
bundle="$here/../ios/build/prefix-bundle.tar.gz"

fail() { echo "FAIL: $1"; exit 1; }
command -v Xvfb >/dev/null || fail "Xvfb not installed"
[ -e "$bundle" ] || fail "prefix bundle missing — run ../ios/make-prefix-bundle.sh first"

# refuse to run against a default build: the fork backend must be dead
strings "$build/dlls/ntdll/ntdll.so" | grep -q "WINE_FORKLESS build" \
    || fail "this is not a WINE_FORKLESS build (see header for the build recipe)"

dpy=":96"
Xvfb "$dpy" -screen 0 1024x768x24 >/dev/null 2>&1 &
xvfb_pid=$!
work="$(mktemp -d /tmp/forkless-gate-XXXXXX)"
srvlog="$work/server.log"; runlog="$work/run.log"
srv_pid=""
cleanup() {
    [ -n "$srv_pid" ] && kill "$srv_pid" 2>/dev/null
    kill "$xvfb_pid" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

tar -C "$work" -xzf "$bundle"
prefix="$work/prefix"

# the server is provided externally (thread-in-app on iOS): foreground+persistent
WINEPREFIX="$prefix" "$wineserver" -f -p >"$srvlog" 2>&1 &
srv_pid=$!
sleep 2
kill -0 "$srv_pid" 2>/dev/null || { cat "$srvlog"; fail "external wineserver did not stay up"; }

DISPLAY="$dpy" WINEPREFIX="$prefix" WINEDEBUG=err+all timeout 16 \
    "$wine" notepad.exe >"$runlog" 2>&1 &
job=$!
sleep 11

# measure DURING the run: hosts via /proc/PID/exe (argv is rewritten), scoped
# to this prefix via the environment
hosts=0
for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null) || continue
    case "$e" in
        "$build"/loader/wine|"$build"/loader/wine-preloader) ;;
        *) continue ;;
    esac
    tr '\0' '\n' < "$p/environ" 2>/dev/null | grep -qF "WINEPREFIX=$prefix" || continue
    hosts=$((hosts+1))
done

wait "$job"; rc=$?

grep -q "entering PE" "$runlog" || { cat "$runlog"; fail "no in-process child entered its PE"; }
[ "$rc" = 124 ] || { cat "$runlog"; fail "notepad did not stay running (rc=$rc)"; }
[ "$hosts" = 1 ] || fail "expected exactly 1 wine host process, saw $hosts"

echo "PASS: notepad runs on a warm prefix with fork/exec compiled out"
echo "      1 host process; children observed in-process:"
grep "child image mapped" "$runlog" | sed 's/^/      /'
