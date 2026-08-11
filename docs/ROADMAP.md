# Roadmap — native Wine port

Master plan and rationale: `NATIVE_PORT.md`. Milestones M1–M4 defined there.
Every phase ends with something demonstrable.

## Phase 0 — Foundation ✅
- [x] Feasibility analysis, blocker inventory, distribution decision
      (sideload-first; App Store explicitly out)
- [x] SwiftUI bottle-manager scaffold with engine abstraction

## Phase 1 — Pseudo-process substrate (WS-A) ← in progress
- [x] `native/pseudoproc`: processes-as-threads model — spawn/wait/exit,
      per-process identity, aux threads, nested spawn; tests passing
- [x] `psrv`: wineserver-as-thread wire model — per-client socketpairs,
      poll loop, global handle table; concurrent-client tests passing
- [ ] Harden: pproc-level TLS slots (PEB carrier), per-process environment
      block, thread-group unwind for NtTerminateProcess semantics
- [ ] CI on Linux + macOS runners (this code must stay bi-platform)

## Phase 2 — Wine fork bring-up, host-first (WS-B) → M1 ← in progress
- [x] Vendor Wine at a pinned release under `native/wine` (submodule, wine-11.0)
- [x] Build real wineserver from source (minimal configure, `server/*.o`)
- [x] **Experiment 001: unmodified wineserver runs as a thread** — full
      lifecycle (boot, client accept, host-initiated shutdown) in one
      process via objcopy main-rename + `--wrap=exit`; findings in
      `native/wineforge/README.md`
- [x] **Experiment 003: embeddable server shutdown (source-level patch)** —
      `wineserver_run()`/`server_exit()` replace the `exit()` funnels via
      setjmp/longjmp; one long-lived server serves many clients and shuts
      down in-process. Maintained as `native/patches/` series (submodule
      stays pinned). Server *restart* declared a non-goal.
- [x] **Experiment 002: real Wine client on the in-thread server** — the
      genuine `wine` loader completes `init_first_thread` against
      wineserver-as-a-thread; server self-shuts-down after the client exits
- [x] **Experiment 004: concurrent real clients** — one in-thread server
      multiplexes N Wine processes (hard handshake count from `-d1` trace),
      scales to 8+, clean self-shutdown
- [x] **Full Wine built on Linux (wine-11.0, loader + 762 DLLs)**
- [x] **Server side of M1 proven**: real Wine, one or many concurrent
      processes, no forked wineserver
- [x] **Patch 0002: client refuses to fork a server** (embedded-only model),
      tested both directions
- Client side of M1 — `CreateProcess` without exec (patch series 0003,
  design in `docs/DESIGN-0003-inproc-spawn.md`):
  - [x] **0003a: spawn seam** — `spawn_process` dispatches fork vs in-process
        backend; default unchanged (no regression), in-process backend wired
        and reached under `WINE_INPROC_SPAWN` (stub → `STATUS_NOT_IMPLEMENTED`)
  - [x] **0003b: per-pseudo-process PEB/TEB + context plumbing** —
        `current_peb()` indirection; the uses on the child's path converted
        (`process.c`, `thread.c`, `system.c`, `server.c`, `env.c`'s `init_peb`),
        pre-first-TEB uses deliberately left on the global (ledger in the
        design doc), plus per-pseudo-process PEB allocation in the spawn path
  - [x] **0003c: in-address-space PE mapping + relocation** — `peload_test`
        loads a PE at a non-preferred base, relocates it, runs it, with two
        images coexisting (parent+child mechanic). Core loader mechanic proven
  - Both process-globals now virtualized (shared-ntdll insight: child shares
    the parent's ntdll, module list rides on the PEB; see design doc):
    - [x] **`peb`** → `current_peb()` (hot path converted, TEB->Peb hook)
    - [x] **`fd_socket`** → `current_server_fd()` (PEB-keyed per-process
          registry) — both zero-regression, suite green
  - [x] **0003d: the in-process attach** — child PEB/TEB allocation + install
        on a new thread, child `init_first_thread` + `init_process_done` on
        the handed socket. `CreateProcess` under `WINE_INPROC_SPAWN` now
        launches the child as a thread group in the same host process; the
        parent's `CreateProcess` succeeds and the server's own `-d1` trace
        counts the in-process children as first-class processes
        (`spawn_seam_test.sh`: 2 children attach, 3 handshakes, deterministic;
        zero regression on the fork backend)
  - [x] **0003e: the child's own startup info + image** — `main_image_info`
        virtualized per-pseudo-process (`current_image_info()`, the third and
        last entangled global); `build_startup_info()` parameterised so a
        child fetches its own startup info from the server, builds its own
        process parameters, and maps its own main EXE. Verified: both of
        wineboot's children map `C:\windows\system32\wineboot.exe` with real
        base + entry addresses, deterministic, fork backend regression-free.
  - [x] **0003f: the child runs** — an in-process child executes its own PE
        entry and exits with its own exit code, using Wine's `SkipLoaderInit`
        plus an inherited (shared-ntdll) module list, with an import-directory
        gate so DLL-importing children were refused rather than crashing the
        host (both superseded by 0003g below). Verified by
        `inproc_run_test.sh` for exit codes 7/42/123 against the **server's
        own `-d1` trace** (`*killed* exit_code=N`), fork backend as control,
        deterministic across repeated runs
  - [x] **0003g: per-process PE-side loader state** — ntdll's loader globals
        (module list, hash table, base-address tree, TLS bitmaps/dirs, resolved
        system-DLL nodes, search path, one-shot gates) move into a single
        per-pseudo-process block, reached O(1) from `peb->LdrData` via
        `CONTAINING_RECORD`; call sites unchanged via `#define`. A child now
        builds its own module list, loads its own imports and runs process
        attach. `SkipLoaderInit` and the import gate are gone. Verified:
        nested `cmd.exe` returns exit code 7 in-process, `attrib.exe` output
        identical to the fork backend, import-free children still exact
  - [x] **0003h: bootstrap boundary** — cold-prefix creation no longer crashes.
        Wine builds a missing prefix from inside early init, before ntdll's PE
        side publishes `pRtlUserThreadStart`, so entering a child jumped to
        NULL. `spawn_process` now falls back to fork when the PE side is not up
        or `is_prefix_bootstrap` is set; a cold prefix builds end-to-end with
        the in-process flags on (asserted). iOS must ship a prepared prefix
  - Remaining, measured and documented: **DLL images are shared between
    pseudo-processes** (2 processes, 1 kernel32 mapping) so DLL globals alias —
    the likely cause of `hostname.exe`'s ERROR_INVALID_HANDLE; and a crashing
    child still takes the host down, so the backend stays opt-in. The fix is a
    private per-process image mapping per DLL (mechanic proven by `peload_test`)
- [x] **M1 complete: `wine notepad.exe` with Windows child processes as
      threads, not processes** — gate script
      `native/wineforge/m1_notepad_test.sh`, PASSING and wired into
      `make test-real`: cold boot on the in-process backend runs notepad and
      explorer's desktop as thread groups inside one host (5 host processes
      under fork → 3, the rest being the deliberate bootstrap-boundary
      forks), notepad verified alive on both backends. The last blocker — a
      GUI child spawned through explorer dying in `load_dll` — was three
      stacked shared-state bugs, fixed in 0003i + patch 0004: the
      handle→unix-fd cache was host-global while handle values are
      per-process (the crash); images were relocated to the server-assigned
      dynamic base even when a sibling occupied it (silent global aliasing);
      and win32u's user-session init ran once per host so later
      pseudo-processes never connected to a winstation/desktop (the explorer
      respawn loop). Full story in `docs/DESIGN-0003-inproc-spawn.md`
      "0003i landed". Still open, tracked there: per-pseudo-process fault
      containment (backend stays opt-in), resource reclamation at
      pseudo-process exit, and the audit list of remaining shared PE-side
      globals; "all fork/exec compiled out" awaits the iOS prepared-prefix
      model (bootstrap deliberately falls back to fork)

## Phase 3 — iOS bring-up (WS-C) → M2 ← in progress
Groundwork done on Linux (see `native/ios/README.md` for the full map):
- [x] **Prepared-prefix bundle pipeline** (`native/ios/make-prefix-bundle.sh`):
      iOS cannot create a prefix at runtime (no fork), so the app ships one
      built at build time. The script builds, prunes, packages (sha256
      manifest) and *verifies the iOS invariant*: a fresh extraction runs a
      real DLL-importing program with the in-process backend on and never
      re-enters prefix bootstrap.
- [x] **Forkless warm-prefix session start**: the bootstrap fork-fallback now
      tests the real precondition (system DLLs on disk) instead of
      `is_prefix_bootstrap`, which is set for the whole wineboot session pass
      on every cold start. With a prepared prefix, services.exe and friends
      run in-process too — the M1 gate's collapse improved from 5→3 to 5→2
      host processes.
- [x] **16 KB-page audit of the fork patches** + `host_page_round()`: the
      spawn path's allocations were 4K-page rounded and would assert on
      Apple's 16 KB-page hosts; fixed, Linux behaviour unchanged.
- [x] **`WINE_FORKLESS` build mode (patch 0005): all fork/exec compiled
      out.** Six guard sites remove the entire runtime fork/exec surface;
      in-process becomes the only backend and the prepared prefix a hard
      requirement. Proven on Linux (`native/wineforge/forkless_gate.sh`):
      notepad runs on a warm bundle prefix against an externally-started
      server — start.exe, notepad and explorer as thread groups in ONE host
      process, with fork not present in the binary. This is the compile mode
      and runtime shape of the iOS build.
- [x] **Unsigned IPA pipeline** (`.github/workflows/build-ipa.yml`): macOS
      runner builds CrossoverPad unsigned (arm64, iOS 17+), runs the unit
      tests on a simulator, packages the IPA, and publishes a GitHub release
      with a direct download link.
Cross-build now proven in CI (`.github/workflows/wine-ios-probe.yml`):
- [x] **The iOS-native Wine runtime links for iOS arm64** — `ntdll.so`,
      `win32u.so`, and `wineserver` all build and link against the iPhoneOS
      SDK with the full patch series applied and `WINE_FORKLESS` defined,
      verified as arm64 Mach-O on a GitHub macOS runner and published as the
      `wine-ios-runtime-arm64` artifact. The iOS source port is patch 0006:
      `TargetConditionals`-guarded narrowings in cdrom/file/loader/system/
      virtual, a `USE_INPROC_TRACE` server backend replacing the Mach
      debugger, and a `configure.ac` iOS branch that disables the AppKit
      winemac driver and blanks the macOS-only frameworks (this is what let
      win32u.so link). The macOS and Linux builds are untouched (Linux
      `ntdll.so`/`wineserver` re-verified byte-clean). This retires the
      "needs a Mac to even try" unknown: the cross toolchain (Xcode clang +
      iPhoneOS SDK + Homebrew LLVM for PE + autoconf) is captured in the probe.
      What this is NOT: a windowed runtime. winemac is disabled and there is
      no Metal/UIKit display driver yet, so this links the headless unix side
      only — a GUI app cannot present on-device until that driver exists.
- [x] **Runtime-bearing IPA assembled and released** (`rt-alpha-1`, ~442 MB,
      `.github/workflows/build-ipa-runtime.yml`): a two-stage pipeline (Linux
      builds the PE DLLs + prepared prefix; macOS cross-builds the iOS arm64
      libs and assembles everything) produces an unsigned IPA that actually
      **embeds** the runtime — `WineRuntime/{lib,pe,prefix}` — with the build
      gated on the embed being present and arm64. This is the artifact your
      on-device testing runs against; the app reports the runtime tier
      truthfully instead of faking execution.
Remaining for M2 (needs a device / the display driver):
- [ ] A UIKit/Metal `win32u` display backend (replaces the disabled winemac
      driver) — the piece that actually puts a window on screen for GUI guests
- [ ] On-device bring-up against `rt-alpha-1`: JIT/W^X probe, loader
      integration, 16 KB-page + TEB verification (needs real hardware)
- [ ] 16 KB-page mmap/section-mapping verification on-device; TEB register plumbing
- [ ] Dev-channel harness (TrollStore/jailbreak) for on-device iteration
- [ ] Render Wine's display into a `CAMetalLayer` via a UIKit winedrv stub
- [ ] **M2: notepad.exe on an iPad**

## Phase 4 — x86 translation (WS-D) → M3
- [ ] Box64 integrated for x86/x86-64 PE code (16 KB-page host support);
      FEX as A/B comparison
- [ ] Dual-mapped (RW+RX `vm_remap`) dynarec cache; JIT-enabler runtime probe
- [ ] DXVK → MoltenVK spike: D3D9/D3D11 sample running end-to-end
- [ ] **M3: a D3D9-era x86 game runs on-device**

## Phase 5 — Product (WS-E/F) → M4
- [ ] Audio (CoreAudio driver), input (touch/Pencil/GameController→XInput)
- [ ] Bottle UI wired to `NativeWineEngine`; install-from-Files flow
- [ ] Packaging: IPA, AltStore/SideStore source, JIT-enabler onboarding UX
- [ ] Rename product (trademark), compat database seeded with tested apps
- [ ] **M4: sideloadable "pick .exe → tap → play"**

## Standing rules
- Host-first: nothing lands in the fork without a Linux/macOS test.
- Upstream what's upstreamable (16K-page fixes, ARM64EC work) to reduce
  long-term fork drift.
- The VM fallback (`appendix-vm-approach.md`) is only revisited if a
  platform-rule change makes it strictly better for users.
