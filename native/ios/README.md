# native/ios — Phase 3 bring-up (iPad)

Phase 3 (docs/ROADMAP.md) turns the host-first Wine fork — M1-complete on
Linux — into something that runs on an iPad. This directory holds the pieces
of that bring-up that are buildable and testable **on Linux**, plus the honest
map of what needs a Mac.

## What runs where

| piece | built/tested on | status |
|---|---|---|
| prepared prefix bundle (`make-prefix-bundle.sh`) | Linux | working |
| 16 KB host-page safety in the fork patches | Linux (audit + `host_page_round`) | done |
| CrossoverPad app + unsigned IPA | GitHub Actions macOS runner (`.github/workflows/build-ipa.yml`) | working |
| Wine fork cross-built for iOS (winelib arm64) | needs a Mac + Xcode iOS SDK | **not started — the M2 blocker** |
| Metal/UIKit display driver (`winemetal.drv` stub) | needs a Mac | not started |
| TEB register plumbing (arm64 TPIDRRO_EL0 constraints) | needs a Mac/device | not started |

## The prepared prefix bundle

iOS forbids fork/exec; the fork's bootstrap boundary (see
`docs/DESIGN-0003-inproc-spawn.md`) deliberately uses fork to *create* a
prefix, because Wine builds a missing prefix inside early init before the
in-process backend's preconditions hold. Therefore, on iOS, **prefix creation
never happens on device**: the app ships a prefix produced at build time by
`make-prefix-bundle.sh` and the runtime only ever opens a warm prefix.

The script builds the prefix cold, prunes caches, packages it with a
sha256 manifest, then **verifies the invariant that matters**: a fresh
extraction runs a real DLL-importing program with `WINE_INPROC_SPAWN=1
WINE_INPROC_RUN=1` without re-entering prefix bootstrap.

`WINEDLLOVERRIDES="mscoree,mshtml="` keeps Gecko/Mono out of the bundle
(they would be downloads anyway and triple its size).

## 16 KB pages (NATIVE_PORT Blocker 2)

Apple arm64 hosts use 16 KB pages while the Windows page size Wine exposes is
fixed at 4 KB. Upstream wine-11.0 already separates the two
(`page_size` vs `host_page_size`/`host_page_mask` in `dlls/ntdll/unix/`), and
its image mapping copes with `host_page_size > page_size`.

Audit result for the fork patches (0003/0004): every allocation goes through
the `page_size` symbol — no literal 4096s — but `anon_mmap_alloc` asserts
*host*-page alignment, so the spawn path's PEB/image-info/fd-cache
allocations, correct on 4 KB Linux, would assert on a 16 KB host. Fixed with
`host_page_round()` (server.c), used at every fork-patch allocation site.
This keeps the Linux build byte-identical in behaviour (4 KB rounds to 4 KB)
and is one less landmine on the Mac.

## Cross-building the fork for iOS — what it takes (needs a Mac)

This container has no Apple SDK (and cannot legitimately obtain one), so the
following is scoped, not done:

1. **Toolchain**: Xcode 15+, `clang -target arm64-apple-ios15.0`, the iOS SDK
   sysroot. Wine's `configure` needs a full cross environment
   (`--host=aarch64-apple-darwin`, `CC="xcrun -sdk iphoneos clang -arch arm64"`)
   plus a native build tree for the tools (`--with-wine-tools`).
2. **PE side is already portable**: the 602 mingw-built PE DLLs are
   OS-independent — the same `x86_64-windows`/`aarch64-windows` binaries ship
   as-is. Only the unix side (ntdll.so, win32u.so, wineserver-as-a-thread)
   needs the iOS compile.
3. **No fork/exec anywhere at runtime**: patches 0001/0002 (embedded server) +
   0003 (in-process CreateProcess) + the prepared prefix close every runtime
   fork; the remaining fork call sites (bootstrap, winebrowser-style spawns)
   must be compiled out or stubbed for the iOS target.
4. **TEB register**: on Apple arm64, TPIDRRO_EL0 is owned by the system; Wine
   upstream already handles the darwin TEB access path for macOS arm64 —
   verify it on iOS, where thread-local storage setup is more restricted.
5. **JIT/W^X**: the PE loader needs RWX or dual-mapped code pages;
   sideload/TrollStore channels enable `dynamic-codesigning`/JIT differently.
   `EngineCapabilities.jitAvailable()` in the app probes at runtime.
6. **Display**: replace winex11 with a Metal-layer driver; the app hosts a
   `CAMetalLayer` and the driver presents into it (M2 scope is a window and
   text — notepad).

## Unsigned IPA

Built by `.github/workflows/build-ipa.yml` on a macOS runner: XcodeGen →
`xcodebuild CODE_SIGNING_ALLOWED=NO` → `Payload/` zip → GitHub release with a
direct download link. Trigger via workflow dispatch (any branch) or a `v*`
tag. The app is the bottle-manager shell with the engine tier surfaced
honestly (mock engine until the wine fork is cross-built — M2).
