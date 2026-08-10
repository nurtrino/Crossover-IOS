# Crossover-IOS

A native port of Wine to iPadOS/iOS: Windows applications running **in-process**
on iPad — no VM, no emulated OS. Box64/FEX translate the app's x86 code,
DXVK → MoltenVK → Metal carries graphics, and a deep Wine fork replaces the
multi-process model iOS forbids.

> **Working title.** "CrossOver" is a registered trademark of CodeWeavers, Inc.
> This project is not affiliated with CodeWeavers and uses no CodeWeavers code
> (Wine is LGPL). A shipping name is chosen before any public release.

## The plan in one paragraph

iPad hardware outclasses many handheld PCs; the gap is OS policy. Three walls
stand between Wine and iOS, and each has a concrete attack: **multi-process**
(Wine's killer dependency) is replaced by a single-address-space
pseudo-process layer — Windows processes become thread groups, wineserver
becomes a thread speaking its unchanged socketpair protocol; **JIT/W^X** is
handled with dual-mapped code buffers under debugger-assisted JIT from
sideload-channel enablers; **distribution** is sideload-first (AltStore-style),
permanently — the App Store is not the target. Full engineering plan:
[`docs/NATIVE_PORT.md`](docs/NATIVE_PORT.md).

## Status

**Phase 2 — the server half of milestone M1 is proven on real Wine.**

Real, tested results (host-first on Linux, against genuine wine-11.0):

- `native/pseudoproc/` — processes-as-threads substrate (spawn/wait/exit,
  per-process identity, nested spawn) + a wineserver-as-thread wire-model
  proof. `make test`, 100× stress-clean.
- `native/wineforge/` — the fork workbench. The **genuine `wine` loader**
  completes the full `init_first_thread` handshake against **wineserver
  running as a thread**, with no forked server; one in-thread server
  multiplexes 8+ concurrent real Wine processes and self-shuts-down cleanly
  (hard handshake count from the server's own trace).
- `native/patches/` — the fork as a quilt series over a pinned submodule:
  **0001** makes wineserver embeddable (returns instead of `exit()`ing);
  **0002** forbids the client from forking a server. Both round-trip clean.
- `CrossoverPad/` — SwiftUI bottle-manager scaffold (mock engine today).

**Client half nearly done — in-process children exist and load their own
programs (0003d + 0003e).** Under `WINE_INPROC_SPAWN`, `CreateProcess`
launches the child as a thread group in the same host process — no
fork/exec. The child gets its own PEB/TEB and server socket, completes the
`init_first_thread` handshake as a first-class process on the shared server,
then fetches its **own** startup info and maps its **own** main EXE (real
base + entry addresses; all three process-globals — `peb`, `fd_socket`,
`main_image_info` — are now per-pseudo-process). Hard `-d1`-trace evidence,
deterministic, zero regression on the fork backend.

**The child runs.** Under `WINE_INPROC_RUN` an in-process child executes its
own PE entry point and exits with its own exit code — verified for several
exit codes against the server's own trace, with the fork backend as control.

**What's left for M1**: children that import DLLs. Running those needs
per-process instancing of ntdll's PE-side loader state (and private data
segments per DLL); until then such children are refused and logged rather
than run, so the host stays up. Ledger in
[`docs/DESIGN-0003-inproc-spawn.md`](docs/DESIGN-0003-inproc-spawn.md).
iOS bring-up (M2) additionally needs a macOS + iOS SDK toolchain to
cross-compile for ARM64. See [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Building

Substrate tests (Linux or macOS):

```sh
cd native/pseudoproc && make test
```

App scaffold (macOS, Xcode 16+, [XcodeGen](https://github.com/yonaskolb/XcodeGen)):

```sh
cd CrossoverPad && xcodegen generate && open CrossoverPad.xcodeproj
```

## Repository layout

```
docs/
  NATIVE_PORT.md            Master engineering plan — read first
  ROADMAP.md                Phases and milestones (M1–M4)
  FEASIBILITY.md            Platform-constraint record and legal checklist
  appendix-vm-approach.md   Retired VM-based fallback design
native/
  pseudoproc/               Single-address-space process model + server-as-thread
CrossoverPad/               SwiftUI app (bottle manager UI, engine abstraction)
```

## Honest expectations

Years of work sit between here and M4, and the multi-process rewrite is
invasive by design. This runs as a sideloaded app with JIT enablers — the
DolphiniOS lifestyle — unless Apple's policy changes. That is understood and
accepted; the hardware is ready even if the platform isn't.
