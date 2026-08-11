#!/usr/bin/env bash
# Build the PREPARED PREFIX BUNDLE the iOS port ships with.
#
# Why this exists (docs/DESIGN-0003-inproc-spawn.md "Bootstrap boundary"):
# creating a Wine prefix from cold happens inside early init, before ntdll's
# PE side exists, so on Linux it deliberately falls back to fork/exec. iOS
# forbids fork/exec entirely — so an iOS app can never create a prefix at
# runtime. Instead the app ships a prefix built here, at build time, and the
# runtime only ever *uses* a warm prefix.
#
# Output: build/prefix-bundle.tar.gz + build/prefix-bundle.manifest
# Verification: the bundle is re-extracted to a fresh path and a real program
# is run against it with the in-process backend on and NO prefix update —
# proving the runtime never needs the bootstrap path with this bundle.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$(cd "$here/../wine-build" && pwd)"
wine="$build/loader/wine"
wineserver="$build/server/wineserver"
out="$here/build"

fail() { echo "FAIL: $1"; exit 1; }
[ -x "$wine" ] || fail "wine loader not built ($wine)"

mkdir -p "$out"
work="$(mktemp -d /tmp/prefix-bundle-XXXXXX)"
prefix="$work/prefix"
# Preserve the script's real exit code across cleanup: killing the server and
# removing $work can fail (a socket still held, a dir busy) and must NOT turn a
# successful bundle build into a spurious non-zero exit.
cleanup() {
    rc=$?
    WINEPREFIX="$prefix" "$wineserver" -k 2>/dev/null || true
    sleep 1
    rm -rf "$work" 2>/dev/null || true
    exit "$rc"
}
trap cleanup EXIT

echo "==> building prefix from cold (fork fallback allowed at build time)"
WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
    timeout 300 "$wine" wineboot.exe --init >/dev/null 2>&1 \
    || fail "wineboot --init returned $?"
[ -e "$prefix/drive_c/windows/system32/kernel32.dll" ] || fail "prefix not populated"

# let the boot-time services settle, then shut the server down cleanly so the
# registry files are flushed
WINEPREFIX="$prefix" "$wineserver" -w 2>/dev/null || true

echo "==> pruning"
# caches and logs that a fresh install never needs
rm -rf "$prefix/drive_c/users/"*/Temp/* 2>/dev/null || true
rm -f  "$prefix/.update-timestamp.bak" 2>/dev/null || true

echo "==> packaging"
# dereference nothing: dosdevices contains intentional symlinks (c: -> ../drive_c);
# on iOS the extractor must recreate them (documented in the manifest header)
tar -C "$work" -czf "$out/prefix-bundle.tar.gz" prefix
{
    echo "# prefix-bundle manifest — $(du -sh "$out/prefix-bundle.tar.gz" | cut -f1) compressed"
    echo "# built $(date -u +%Y-%m-%dT%H:%M:%SZ) from wine-build at $build"
    echo "# NOTE: dosdevices/* are symlinks; extract with symlink support"
    echo "sha256  $(sha256sum "$out/prefix-bundle.tar.gz" | cut -d' ' -f1)"
    echo "files   $(find "$prefix" -type f | wc -l)"
    echo "size    $(du -sh "$prefix" | cut -f1) uncompressed"
} > "$out/prefix-bundle.manifest"

echo "==> verifying: warm-prefix run with the in-process backend, no update pass"
verify="$work/verify"
mkdir -p "$verify"
tar -C "$verify" -xzf "$out/prefix-bundle.tar.gz"
vprefix="$verify/prefix"
vlog="$work/verify.log"
# the bundle must run a real DLL-importing program with the in-process backend
# enabled and without rebuilding/updating the prefix
WINEPREFIX="$vprefix" WINE_INPROC_SPAWN=1 WINE_INPROC_RUN=1 WINEDEBUG=warn+process \
    timeout 120 "$wine" cmd.exe /c "echo BUNDLE-OK" >"$vlog" 2>&1 || true
grep -q "BUNDLE-OK" "$vlog" || { tail -20 "$vlog"; fail "warm-prefix run did not produce output"; }
grep -q "prefix bootstrap" "$vlog" && fail "verification run re-entered prefix bootstrap"
WINEPREFIX="$vprefix" "$wineserver" -k 2>/dev/null || true

echo "PASS: prefix bundle built and verified"
echo "      $out/prefix-bundle.tar.gz"
cat "$out/prefix-bundle.manifest"
