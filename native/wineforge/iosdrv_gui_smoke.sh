#!/usr/bin/env bash
# IOSDRV GUI SMOKE — the display driver's Linux-testable slice.
#
# wineios.drv (patch 0007) is a pure-C driver: the unix side has no UIKit —
# it presents through a host bridge when the app provides one and runs
# HEADLESS when none is present (surfaces still exist; with
# WINEIOS_SURFACE_DUMP set, every flush writes the surface as a BMP and logs
# a "wineios: FLUSH ... checksum=" marker). That makes the whole driver path
# testable on Linux with no display server at all:
#
#   driver selection (explorer default "mac,x11,wayland,ios" — only ios is
#   built here) -> pUpdateDisplayDevices -> desktop -> pCreateWindowSurface
#   -> GDI paints -> flush -> BMP with notepad's real pixels.
#
# Requires a build with wineios.drv enabled and no other display driver
# (e.g. configure --without-x --enable-wineios-drv). Point WINEFORGE_BUILD at
# it, or the canonical ../wine-build is used.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$(cd "${WINEFORGE_BUILD:-$here/../wine-build}" && pwd)"
wine="$build/loader/wine"

fail() { echo "FAIL: $1"; exit 1; }
[ -x "$wine" ] || fail "no loader at $wine"
[ -f "$build/dlls/wineios.drv/wineios.so" ] || fail "wineios.so not built (configure --enable-wineios-drv)"
ls "$build"/dlls/wineios.drv/*-windows/wineios.drv >/dev/null 2>&1 || fail "wineios.drv PE half not built"

work="$(mktemp -d /tmp/iosdrv-smoke-XXXXXX)"
prefix="$work/prefix"
dump="$work/dump"
mkdir -p "$prefix" "$dump"
cleanup() {
    WINEPREFIX="$prefix" "$build/server/wineserver" -k 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

# headless on purpose: the ios driver must be picked with no display around.
# WINEIOS_SCREEN is exported for the WHOLE session (wineboot included):
# the desktop process registers the display once, so the size must be in
# its environment, not just notepad's.
unset DISPLAY WAYLAND_DISPLAY
export WINEPREFIX="$prefix"
export WINEIOS_SCREEN=800x600

echo "== prefix init (cold prefix; takes minutes on slow machines) =="
WINEDEBUG=-all timeout 600 "$wine" wineboot.exe --init >"$work/boot.log" 2>&1 \
    || { tail -20 "$work/boot.log"; fail "wineboot --init"; }

# Deterministic driver choice (the explorer fallback would also find ios —
# the only driver in this build — but pin it so the test can't silently pass
# through a different one).
WINEDEBUG=-all timeout 60 "$wine" reg add 'HKCU\Software\Wine\Drivers' \
    /v Graphics /d ios /f >>"$work/boot.log" 2>&1 || fail "reg add Graphics=ios"

echo "== notepad.exe, headless, dumping surfaces =="
WINEDEBUG=+err WINEIOS_SURFACE_DUMP="$dump" \
    timeout 45 "$wine" notepad.exe >"$work/run.log" 2>&1
rc=$?

echo "-- run.log (driver markers) --"
grep "wineios:" "$work/run.log" | head -20

# notepad must STAY RUNNING (killed by our timeout), not crash out
[ "$rc" = 124 ] || { tail -30 "$work/run.log"; fail "notepad exited early (rc=$rc)"; }
grep -q "wineios: SURFACE_CREATED" "$work/run.log" || fail "driver never created a window surface"
grep -q "wineios: FLUSH" "$work/run.log" || fail "no pixel flush reached the driver"
ls "$dump"/surface-*.bmp >/dev/null 2>&1 || fail "no surface BMP was dumped"

# The pixels must show real painting. The driver forces alpha to 0xff, so
# "any nonzero byte" would always pass; require a byte that is neither 00
# nor ff — frame grays / caption colors qualify, black+alpha alone doesn't.
biggest="$(ls -S "$dump"/surface-*.bmp | head -1)"
painted="$(od -An -tx1 -j54 "$biggest" | grep -Ev '^( (00|ff))*$' | head -1)"
[ -n "$painted" ] || fail "surface bitmap has no painted content (only black/alpha)"

echo "PASS: wineios.drv created a surface, received flushes, and dumped real pixels"
echo "      (largest dump: $(basename "$biggest"), $(stat -c%s "$biggest") bytes)"
