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
# Children go through the real PE-side loader: loader_init gives each
# pseudo-process its own loader state (0003g), so a child builds its own module
# list, loads its own imports and runs process attach. Both an import-free PE
# (mkexe) and real DLL-importing programs are covered.
#
# Known gap (documented, not asserted): heavy multi-child workloads — notably
# creating a Wine prefix from cold, which spawns a tree of wineboot helpers —
# still crash under WINE_INPROC_RUN. Simple console programs work.
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

    grep -q "image mapped: .*exit$code\.exe\" base=" "$log" \
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

echo "DLL-importing children run in-process too"

# nested cmd.exe: a heavy DLL user, checked by exit code against the fork backend
WINEPREFIX="$prefix" WINEDEBUG=-all timeout 60 \
    "$wine" "$cmd" /c cmd /c exit 7 >/dev/null 2>&1
forkrc=$?
WINE_INPROC_SPAWN=1 WINE_INPROC_RUN=1 WINEPREFIX="$prefix" WINEDEBUG=-all timeout 60 \
    "$wine" "$cmd" /c cmd /c exit 7 >"$log" 2>&1
inprocrc=$?
grep -q 'image mapped: .*cmd\.exe" base=' "$log" \
    || { cat "$log"; fail "nested cmd was not run as an in-process child"; }
[ "$forkrc" = 7 ] || fail "fork backend nested cmd returned $forkrc, expected 7"
[ "$inprocrc" = 7 ] || { cat "$log"; fail "in-process nested cmd returned $inprocrc, expected 7"; }
echo "  nested cmd.exe: fork=$forkrc in-process=$inprocrc  match"

# attrib.exe: output compared against the fork backend
forkout=$(WINEPREFIX="$prefix" WINEDEBUG=-all timeout 60 \
    "$wine" "$cmd" /c 'attrib.exe c:\' 2>/dev/null | head -1)
inprocout=$(WINE_INPROC_SPAWN=1 WINE_INPROC_RUN=1 WINEPREFIX="$prefix" WINEDEBUG=-all timeout 60 \
    "$wine" "$cmd" /c 'attrib.exe c:\' 2>/dev/null | grep -v "^wine: in-process" | head -1)
[ -n "$forkout" ] || fail "fork backend produced no attrib output"
[ "$forkout" = "$inprocout" ] \
    || fail "attrib output differs: fork=[$forkout] in-process=[$inprocout]"
echo "  attrib.exe: output identical to the fork backend"

echo "PASS: in-process children — import-free and DLL-importing — run and match the fork backend"
