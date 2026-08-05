# Crossover-IOS

Run Windows applications on iPad and iPhone, with a CrossOver-style "just run the app" experience — no visible desktop, no VM babysitting.

> **Working title.** "CrossOver" is a registered trademark of CodeWeavers, Inc. This project is **not** affiliated with CodeWeavers and does not use any CodeWeavers code. A shipping name must be chosen before any public release — see `docs/FEASIBILITY.md § Legal`.

## What this is

A native iPadOS/iOS app (SwiftUI) that lets a user pick a Windows program and run it. Under the hood, each program lives in a **bottle**: an isolated guest environment (lightweight Linux + Wine + Box64, or a Windows ARM guest) executed by an embedded emulator. The user never sees the plumbing — they see their app in a window.

Wine itself cannot run natively on iOS (no `fork`/`exec`, no JIT, no loadable unsigned code), so the compatibility layer runs *inside* an emulated guest. This is the only approach with shipping precedent on the platform (UTM SE, App Store, 2024). Full analysis in `docs/FEASIBILITY.md`.

## Repository layout

```
docs/
  FEASIBILITY.md    Platform constraints, distribution paths, legal notes — read first
  ARCHITECTURE.md   System design: engine, bottles, display, storage
  ROADMAP.md        Phased milestones
CrossoverPad/
  project.yml       XcodeGen project definition
  Sources/          SwiftUI app scaffold (bottle manager UI + engine abstraction)
```

## Building the app scaffold

Requires macOS with Xcode 16+ and [XcodeGen](https://github.com/yonaskolb/XcodeGen):

```sh
cd CrossoverPad
xcodegen generate
open CrossoverPad.xcodeproj
```

The scaffold builds and runs today with a **mock engine** (no emulation yet) so the bottle-management UX can be developed and tested independently of the emulator integration, which is Phase 2 in `docs/ROADMAP.md`.

## Status

- [x] Feasibility research and platform-constraint analysis
- [x] Architecture and phased roadmap
- [x] SwiftUI app scaffold: bottle model, store, engine protocol, management UI
- [ ] Embedded QEMU/TCG engine (via QEMUKit) — Phase 2
- [ ] Guest image build pipeline (Alpine + Wine + Box64) — Phase 3
- [ ] Display/input integration (SPICE → Metal) — Phase 4
- [ ] Distribution (App Store review / AltStore PAL) — Phase 5
