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

**Phase 1 of 5 — pseudo-process substrate: prototype passing.**

- `native/pseudoproc/` — processes-as-threads model (spawn/wait/exit codes,
  per-process identity, aux threads, nested spawn) **+** wineserver-as-thread
  wire-model proof (per-client socketpairs, poll loop, global handle table,
  concurrent clients). `make test` runs both suites; 100× stress-clean.
- `CrossoverPad/` — SwiftUI bottle-manager app scaffold (XcodeGen), engine
  abstraction with mock backend; `NativeWineEngine` lands in Phase 3.
- `docs/` — feasibility record, native-port master plan, phased roadmap.

Current target — **M1**: `wine notepad.exe` on Linux with fork/exec compiled
out, every Windows process a pseudo-process. See
[`docs/ROADMAP.md`](docs/ROADMAP.md).

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
