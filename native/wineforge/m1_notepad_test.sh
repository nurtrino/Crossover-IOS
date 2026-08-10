#!/usr/bin/env bash
# M1 GATE — currently FAILING. This is the executable definition of what is
# still missing, not a passing test. It is deliberately NOT wired into
# `make test-real`; run it by hand to check progress toward M1.
#
# Current result, narrowed:
#   * notepad as the PRIMARY process, in-process backend on: runs and stays up
#     (20s, no errors) — verified by hand.
#   * notepad spawned as an IN-PROCESS CHILD by explorer's desktop path: exits
#     early, so the host-process comparison is never reached.
# So the gap is not "notepad cannot run in-process" but "a GUI child launched
# through explorer does not survive". Root cause not yet found; suspect the
# same shared-DLL-globals class as hostname.exe (user32/win32u state cached in
# DLL globals that alias across pseudo-processes).
#
# M1 — `wine notepad.exe` with Windows child processes as threads, not processes.
#
# Milestone M1 (docs/NATIVE_PORT.md): a real GUI Windows program runs on Linux
# with Wine's multi-process model replaced — wineserver is a thread, and the
# Windows processes Wine spawns around the app are thread groups in the host
# process rather than forked wine loaders.
#
# The hard evidence here is a HOST PROCESS COUNT. Under the fork backend Wine
# spawns a separate `wine` host process per Windows process (services.exe,
# explorer.exe, plugplay, rpcss, ...). Under the in-process backend those must
# become threads, so the count collapses. Comparing the two backends on the
# same workload is what makes the claim falsifiable — a hardcoded number could
# be satisfied by an accident.
#
# Needs a display: notepad is a GUI app, so the run happens under Xvfb, and the
# build must include winex11.drv (configure WITHOUT --without-x).
#
# Prefix creation still runs on the fork backend by design: Wine builds a
# missing prefix from inside early init, before ntdll's PE side exists. See
# docs/DESIGN-0003-inproc-spawn.md "Bootstrap boundary".
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$(cd "$here/../wine-build" && pwd)"
wine="$build/loader/wine"
wineserver="$build/server/wineserver"

fail() { echo "FAIL: $1"; exit 1; }
command -v Xvfb >/dev/null || fail "Xvfb not installed (needed to run a GUI app headless)"
ls "$build"/dlls/winex11.drv/winex11.* >/dev/null 2>&1 \
    || fail "winex11.drv not built — reconfigure without --without-x"

dpy=":97"
Xvfb "$dpy" -screen 0 1024x768x24 >/dev/null 2>&1 &
xvfb_pid=$!
sleep 2

prefix="/tmp/wineforge-m1-$$"; rm -rf "$prefix"; mkdir -p "$prefix"
log="$prefix.log"
cleanup() {
    WINEPREFIX="$prefix" "$wineserver" -k 2>/dev/null
    sleep 1
    pkill -f "WINEPREFIX=$prefix" 2>/dev/null
    kill "$xvfb_pid" 2>/dev/null
    rm -rf "$prefix" "$log"
}
trap cleanup EXIT

# how many `wine` host processes are alive (the prefix is unique to this run,
# and every server is killed between measurements, so a plain count is scoped)
hosts() { pgrep -c -f "$build/loader/wine" 2>/dev/null || true; }

echo "creating the prefix (fork backend — bootstrap boundary)"
DISPLAY="$dpy" WINEPREFIX="$prefix" WINEDEBUG=-all timeout 240 "$wine" wineboot.exe >/dev/null 2>&1
[ -e "$prefix/drive_c/windows/system32/kernel32.dll" ] || fail "prefix was not created"

measure() {   # $1 = label, rest = env assignments
    local label="$1"; shift
    WINEPREFIX="$prefix" "$wineserver" -k 2>/dev/null; sleep 2
    # `timeout` reports 124 when it had to kill a still-running program, which
    # is what "notepad stayed up" looks like for a GUI app with no one to close it
    DISPLAY="$dpy" WINEPREFIX="$prefix" WINEDEBUG=-all env "$@" \
        timeout 18 "$wine" notepad.exe >"$log" 2>&1 &
    local job=$!
    sleep 12
    local n; n=$(hosts)
    wait "$job"; local rc=$?
    local alive=0; [ "$rc" = 124 ] && alive=1
    echo "$label|$alive|$n"
    WINEPREFIX="$prefix" "$wineserver" -k 2>/dev/null; sleep 2
}

echo "running notepad.exe on each backend"
forkres=$(measure fork)
inres=$(measure inproc WINE_INPROC_SPAWN=1 WINE_INPROC_RUN=1)
grep -q "graphics driver is missing" "$log" && fail "no graphics driver in this build"

fork_alive=$(echo "$forkres" | cut -d'|' -f2); fork_hosts=$(echo "$forkres" | cut -d'|' -f3)
in_alive=$(echo "$inres"   | cut -d'|' -f2); in_hosts=$(echo "$inres"   | cut -d'|' -f3)

echo "  fork backend:       notepad alive=$fork_alive, wine host processes=$fork_hosts"
echo "  in-process backend: notepad alive=$in_alive, wine host processes=$in_hosts"

[ "$fork_alive" = 1 ] || fail "notepad did not stay running on the fork backend (env problem)"
[ "$in_alive" = 1 ]   || { cat "$log"; fail "notepad did not stay running on the in-process backend"; }
[ "$fork_hosts" -gt 1 ] || fail "fork backend did not spawn extra host processes ($fork_hosts) — measurement is not meaningful"
[ "$in_hosts" -lt "$fork_hosts" ] \
    || fail "in-process backend did not reduce host processes ($in_hosts vs $fork_hosts)"

echo "PASS: notepad.exe runs with its Windows processes collapsed into the host"
echo "      ($fork_hosts host processes -> $in_hosts)"
