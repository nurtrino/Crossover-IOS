# Handoff — M2 (notepad.exe on an iPad)

State at handoff: **Phase 2 complete, Phase 3 cross-build proven.** The
iOS-native Wine runtime cross-compiles and links as arm64 Mach-O in CI; what
remains for M2 is device-dependent and cannot be built or verified without
Apple hardware. This file is the operational map of exactly what is done and
what is left, so the remaining work is turnkey on a Mac + iPad.

Branch `claude/v1-branch-handoff-1sn5p2`. Read `docs/ROADMAP.md` for the
milestone view and `native/patches/README.md` for the patch series.

---

## 1. What is done and verified

| capability | evidence | where |
|---|---|---|
| Phase 2 / M1 — Windows processes as threads, one host | `make test-real` 6/6 incl. `m1_notepad_test.sh` (5→2 hosts) | `native/wineforge/` |
| All fork/exec compiled out (`WINE_FORKLESS`) | `forkless_gate.sh`: notepad runs, 1 host process | `native/wineforge/forkless_gate.sh` |
| Prepared-prefix bundle (iOS ships a warm prefix) | build + verify script, sha256 manifest | `native/ios/make-prefix-bundle.sh` |
| 16 KB host-page safety in the fork patches | `host_page_round()`, audit | patch 0003i |
| **iOS-native runtime links as arm64 Mach-O** | `ntdll.so` + `win32u.so` + `wineserver`, CI-verified | `.github/workflows/wine-ios-probe.yml` |
| Unsigned IPA (app shell) | released v0.1.0-alpha.1, direct link | `.github/workflows/build-ipa.yml` |
| **Runtime-bearing IPA** (embeds the real runtime) | released `rt-alpha-1` (~442 MB); build gate verifies the embed | `.github/workflows/build-ipa-runtime.yml` |

The runtime-bearing IPA is the artifact for on-device testing: it embeds
`WineRuntime/lib/` (the arm64 `ntdll.so`/`win32u.so`/`wineserver`),
`WineRuntime/pe/` (602 Windows PE DLLs), and `WineRuntime/prefix/` (the
prepared prefix). The macOS build job fails unless all of those are present in
the built `.app`, so the release is proof-of-embed, not just proof-of-build.
The app probes what is embedded (`WineRuntimeBundle`) and `NativeWineEngine`
reports a specific reason rather than pretending to run — the two things it
still needs are §2a (display driver) and on-device JIT (§2c).

The cross-build is reproducible: the probe workflow captures the entire
toolchain (Xcode clang + iPhoneOS SDK, Homebrew `llvm`+`lld` for PE
cross-compilation, `bison`, `autoconf` to regenerate `configure` from the
patched `configure.ac`). It uploads `wine-ios-runtime-arm64` (the three .so /
wineserver binaries) every run.

The iOS source port is **patch 0006** — every hunk is a `TargetConditionals`
guard narrowing a macOS assumption to `TARGET_OS_OSX` and giving iOS a
working path (cdrom, file, loader, system, virtual, a `USE_INPROC_TRACE`
server backend, and the `configure.ac` iOS framework branch). macOS and Linux
builds are untouched; the Linux `ntdll.so`/`wineserver` are re-verified
byte-clean with the full series applied.

## 2. What is left for M2 — all device-dependent

These cannot be done in a Linux CI container. They need a Mac with Xcode and
a development-mode iPad (or the iOS Simulator for the non-JIT parts).

### 2a. A UIKit/Metal display driver — the piece that puts a window on screen

The probe **disables the AppKit-based `winemac.drv`** (patch 0006's
`configure.ac` branch) so `win32u.so` links on iOS. That means the runtime is
currently **headless** — it has no graphics driver, so a GUI program like
notepad cannot create a window. A replacement driver is the core of M2.

- Model it on `winemac.drv` (the macOS driver) — it is the closest existing
  reference for an Apple windowing backend. The new driver (`winemetal.drv`
  or `wineios.drv`) implements the `win32u` driver entry points (`gdi_driver`
  / `user_driver`): create/destroy window, present a surface, deliver input.
- Rendering target: a `CAMetalLayer` (or `UIView` layer) the host app owns.
  Wine draws the window's DIB into a Metal texture and the driver presents it.
  M2's bar is a single top-level window with drawn text — notepad — not a
  compositor.
- Input: translate UIKit touch/keyboard events into `win32u` input events.
- This is the multi-day, device-iterated piece. It has no Linux-testable
  slice: it links against Metal/UIKit and only means anything on a device.

### 2b. Cross-build the PE side and assemble the bundle

- The **PE DLLs are OS-independent** — the Linux build already produces all
  602 `*-windows` PE binaries (`native/wine-build/dlls/*/x86_64-windows/`).
  They ship as-is; there is nothing iOS-specific to rebuild for them. (For an
  arm64 Windows target instead of x86_64, rebuild them with the same LLVM PE
  cross-compiler the probe already installs — no source changes.)
- Bundle contents for the app:
  1. iOS-native unix side: `ntdll.so`, `win32u.so`, `wineserver`, plus the
     other unixlib `.so` files a full `make` produces (extend the probe's
     target list — they compile the same way).
  2. the OS-independent PE DLLs,
  3. the prepared prefix (`native/ios/make-prefix-bundle.sh` output),
  4. the new display driver from 2a.
- Wire `CrossoverPad/Sources/Engine/NativeWineEngine.swift` (currently
  `isAvailable = false`) to `dlopen` the runtime and drive it. `MockEngine`
  stays as the fallback so the UI tier is always honest.

### 2c. On-device runtime verification (needs real hardware)

- **JIT / W^X**: the PE loader needs RWX or dual-mapped code pages.
  Sideload/TrollStore channels grant `dynamic-codesigning` differently;
  `EngineCapabilities.jitAvailable()` probes at runtime. Verify on the actual
  install channel.
- **16 KB pages**: the fork patches are page-size-safe (`host_page_round`),
  but the full section-mapping path must be exercised on a 16 KB-page device.
- **TEB register**: confirm the Apple-arm64 TEB access path (upstream handles
  it for macOS arm64) holds on iOS, where thread-local setup is more
  restricted.

## 3. How to reproduce the cross-build (on a Mac or the CI)

The `wine-ios-probe` workflow is the source of truth. To run it locally on a
Mac:

```sh
brew install bison llvm lld autoconf
cd native/patches && ./apply.sh            # apply the fork series
cd ../wine && autoconf                      # regenerate configure from configure.ac
# native tools (host):
mkdir /tmp/native && cd /tmp/native
</path>/native/wine/configure --enable-win64 --without-x --without-freetype --disable-tests
make -j __tooldeps__
# iOS cross build:
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir /tmp/ios && cd /tmp/ios
</path>/native/wine/configure --host=aarch64-apple-darwin --with-wine-tools=/tmp/native \
  --without-x --without-freetype --disable-tests \
  CC="$(xcrun -f clang) -arch arm64 -isysroot $SDK -miphoneos-version-min=16.0 -DWINE_FORKLESS"
make -j dlls/ntdll/ntdll.so dlls/win32u/win32u.so server/wineserver
```

Everything through `make` here is proven green in CI. `2a`/`2b`/`2c` above
are the work that starts once this runtime is in hand on a device.

## 4. The one-line status

M1 done; the iOS runtime cross-compiles and links (arm64 Mach-O, CI-proven);
M2 = display driver + bundle + on-device verification, all of which need a
Mac and an iPad and none of which can be faked from a Linux container.
