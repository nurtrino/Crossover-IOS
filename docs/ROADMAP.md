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
  - [~] 0003b: per-pseudo-process PEB/TEB + context plumbing — `current_peb()`
        indirection added; `process.c`, `thread.c`, `system.c`, `server.c` converted (26/81, zero regression). Remaining env.c/virtual.c
        file-by-file conversion of the other 75 global-`peb` uses (ledger in
        the design doc), then per-pseudo-process PEB allocation
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
        plus an inherited (shared-ntdll) module list. `image_needs_loader()`
        gates entry: import-free images run; DLL-importing images are refused
        and logged instead of faulting the host. Verified by
        `inproc_run_test.sh` for exit codes 7/42/123 against the **server's
        own `-d1` trace** (`*killed* exit_code=N`), fork backend as control,
        deterministic across repeated runs
  - Remaining for M1 — **per-process PE-side loader state**: `loader_init`'s
    one-shot gates, the module list/hash table, TLS bitmaps and resolved
    system-DLL handles must be instanced per pseudo-process, and DLL data
    segments need private per-process views (ledger in the design doc). Until
    then a child that imports DLLs is refused rather than run
- [ ] M1 complete: `wine notepad.exe` with all fork/exec compiled out,
      single host process

## Phase 3 — iOS bring-up (WS-C) → M2
- [ ] Cross-build the fork against the iOS SDK (winelib static libs + dylibs)
- [ ] 16 KB-page mmap/section-mapping layer; TEB register plumbing
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
