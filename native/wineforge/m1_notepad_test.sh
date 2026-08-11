#!/usr/bin/env bash
# M1 GATE — the executable definition of milestone M1, wired into
# `make test-real` since it passes (patch series 0003i).
#
# History of the failure this gate caught (see docs/DESIGN-0003-inproc-spawn.md
# for the full account): a GUI child spawned through explorer's desktop path
# died with an access violation inside ntdll's load_dll (the instruction at
# ntdll RVA 0x545d5 read through a NULL IMAGE_NT_HEADERS pointer in
# find_existing_module). The chain behind it was:
#   1. the client-side handle->unix-fd cache was host-global while handle
#      values are per-process, so a child's section handle could alias a
#      sibling's cached fd — the child mapped its kernel32 from the wrong
#      file and got an image with no PE header (the NULL above);
#   2. map_image_into_view relocated images to the server-assigned dynamic
#      base even when a sibling pseudo-process already occupied it and the
#      view had landed elsewhere — mis-relocating every absolute address
#      into the sibling's copy;
#   3. win32u's user-session init ran under one host-global pthread_once, so
#      a second pseudo-process never connected to a winstation/desktop and
#      the explorer /desktop child failed every CreateWindow with
#      ERROR_INVALID_HANDLE, respawning forever.
# All three are fixed by 0003i (per-process fd cache, relocate-to-actual-base,
# per-process user-session init).
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

# How many wine host processes are alive for THIS prefix. Counted via
# /proc/PID/exe (the loader binary, possibly through wine-preloader): wine
# rewrites child argv to the Windows image name ("C:\windows\...\services.exe"),
# so a cmdline match (pgrep -f) misses every forked child. Scoped to this
# run's prefix through the process environment.
hosts() {
    local n=0 p e
    for p in /proc/[0-9]*; do
        e=$(readlink "$p/exe" 2>/dev/null) || continue
        case "$e" in
            "$build"/loader/wine|"$build"/loader/wine-preloader) ;;
            *) continue ;;
        esac
        tr '\0' '\n' < "$p/environ" 2>/dev/null | grep -qxF "WINEPREFIX=$prefix" || continue
        n=$((n+1))
    done
    echo "$n"
}

echo "creating the prefix (fork backend — bootstrap boundary)"
DISPLAY="$dpy" WINEPREFIX="$prefix" WINEDEBUG=-all timeout 240 "$wine" wineboot.exe >/dev/null 2>&1
[ -e "$prefix/drive_c/windows/system32/kernel32.dll" ] || fail "prefix was not created"

measure() {   # $1 = label, rest = env assignments
    local label="$1"; shift
    # Each backend is measured from a clean server, otherwise the first run
    # leaves services.exe/explorer.exe up and the second inherits them, which
    # makes the host-process comparison meaningless.
    WINEPREFIX="$prefix" "$wineserver" -k 2>/dev/null; sleep 3
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
    sleep 2
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
