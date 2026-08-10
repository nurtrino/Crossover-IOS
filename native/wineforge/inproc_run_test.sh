#!/usr/bin/env bash
# Experiment 0003f — an in-process child RUNS its program end to end.
#
# The full chain, with no fork/exec anywhere in the child's creation:
#   parent CreateProcess -> thread group in the same host process
#     -> child PEB/TEB + own server socket
#     -> child's own startup info + own main EXE mapped (relocated)
#     -> child enters its PE entry point and executes
#     -> child exits with its program's exit code
#
# Evidence is taken from the SERVER's own -d1 trace (the same method as
# experiment 004): the server reports `*killed* exit_code=N` for the child
# process object. That is authoritative and independent of how any particular
# parent propagates exit codes.
#
# The child image is an import-free PE (mkexe). Images that import DLLs still
# need per-process PE-side loader state; they are refused rather than run, and
# that boundary is asserted too so it stays visible.
#
# Known gap (documented, not asserted): when a parent's own work depends on a
# refused DLL-importing child (e.g. wineboot's prefix update), the parent can
# fail even though the in-process child itself ran correctly.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$(cd "$here/../wine-build" && pwd)"
wine="$build/loader/wine"
wineserver="$build/server/wineserver"
mkexe="$here/build/mkexe"
cmd='c:\windows\system32\cmd.exe'

fail() { echo "FAIL: $1"; exit 1; }
[ -x "$mkexe" ] || fail "mkexe not built (run make)"

pkill -x wineserver 2>/dev/null; sleep 1
prefix="/tmp/wineforge-run-$$"; srv="$prefix.srv"; log="$prefix.log"
rm -rf "$prefix"; mkdir -p "$prefix"
cleanup() {
    WINEPREFIX="$prefix" "$wineserver" -k 2>/dev/null
    rm -rf "$prefix" "$srv" "$log"
}
trap cleanup EXIT

# One long-lived traced server for the whole test, and settle the prefix on it
# with the fork backend so the measured runs don't trigger a wineboot child.
WINEPREFIX="$prefix" "$wineserver" -f -d1 -p0 2>"$srv" &
sleep 1
WINEPREFIX="$prefix" WINEDEBUG=-all timeout 90 "$wine" wineboot.exe >/dev/null 2>&1
sys="$prefix/drive_c/windows/system32"
[ -d "$sys" ] || fail "prefix did not initialize"

echo "in-process children run their own programs"
for code in 7 42 123; do
    "$mkexe" "$sys/exit$code.exe" "$code" >/dev/null || fail "mkexe failed"

    # control: the fork backend, end to end
    WINEPREFIX="$prefix" WINEDEBUG=-all timeout 60 \
        "$wine" "$cmd" /c "exit$code.exe" >/dev/null 2>&1
    forkrc=$?
    [ "$forkrc" = "$code" ] || fail "fork backend returned $forkrc, expected $code"

    # remember where the server log ends, so we only read this run's trace
    # (markers can't be appended: the server holds the fd at its own offset)
    off=$(wc -c <"$srv")
    WINE_INPROC_SPAWN=1 WINE_INPROC_RUN=1 WINEPREFIX="$prefix" WINEDEBUG=-all timeout 60 \
        "$wine" "$cmd" /c "exit$code.exe" >"$log" 2>&1
    hostrc=$?

    grep -q "in-process child image mapped: L\"C:\\\\\\\\windows\\\\\\\\system32\\\\\\\\exit$code.exe\"" "$log" \
        || { cat "$log"; fail "child did not map its own image (code $code)"; }
    grep -q "in-process child entering PE" "$log" \
        || { cat "$log"; fail "child was not entered in-process (code $code)"; }
    [ "$hostrc" -lt 128 ] || fail "host died with signal $((hostrc - 128)) (code $code)"

    # authoritative: the server saw a process exit with the program's code
    tail -c "+$((off + 1))" "$srv" | grep -q "\*killed\* exit_code=$code" \
        || { tail -c "+$((off + 1))" "$srv" | grep '\*killed\*'; \
             fail "server did not record a child exiting with $code"; }

    echo "  exit $code: fork=$forkrc, in-process child ran and server recorded exit_code=$code"
done

echo "boundary: an image needing the PE-side loader is refused, not crashed"
WINE_INPROC_SPAWN=1 WINE_INPROC_RUN=1 WINEPREFIX="$prefix" WINEDEBUG=-all timeout 90 \
    "$wine" wineboot.exe >"$log" 2>&1
hostrc=$?
grep -q "needs the PE-side loader; not entered" "$log" \
    || { cat "$log"; fail "a DLL-importing child was not refused"; }
[ "$hostrc" -lt 128 ] || fail "host died with signal $((hostrc - 128)) on a refused child"
echo "  ok: refused cleanly, host survived (rc=$hostrc)"

echo "PASS: in-process children execute their own programs and exit with real codes"
