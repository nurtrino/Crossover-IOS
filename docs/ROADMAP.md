# Roadmap

Phases are ordered so that every phase ends with something demonstrable.

## Phase 1 — Foundation (this repo, now)
- [x] Feasibility analysis and architecture (docs/)
- [x] SwiftUI scaffold: bottle model + store, engine protocol, MockEngine,
      bottle list / create / detail UI
- [ ] CI: build + unit tests on macOS runner (`xcodegen && xcodebuild test`)

**Exit criteria:** app runs on an iPad simulator; you can create, rename,
"run" (mock), and delete bottles; state survives relaunch.

## Phase 2 — Embedded emulator
- [ ] Vendor QEMUKit + QEMU (TCTI build) as SwiftPM/xcframework dependencies
- [ ] `QEMUTCTIEngine` conforming to `VirtualMachineEngine`
- [ ] Boot a stock Alpine ARM64 image headless; serial console proof in-app
- [ ] License audit of every linked component (see FEASIBILITY § Legal)

**Exit criteria:** an iPad (device, not simulator) boots a Linux guest from a
bottle and shows console output inside the app.

## Phase 3 — The Wine guest
- [ ] Reproducible guest-image build under `guest/` (Alpine + Wine ARM64EC +
      Box64, binfmt wiring, kiosk compositor, guest agent)
- [ ] Bottle manifest → auto-launch of a target `.exe` at boot
- [ ] First end-to-end win: a real Win32 x86 app (e.g. Notepad++ installer →
      installed → runs) inside a bottle

**Exit criteria:** "New bottle → pick installer.exe from Files → app icon
appears → tap → app runs." The CrossOver moment.

## Phase 4 — Display, input, polish
- [ ] Metal-backed guest display in `RunningAppView` (replace serial console)
- [ ] Touch/Pencil/keyboard mapping; clipboard sync via guest agent
- [ ] Post-install snapshots; "Reset bottle"; bottle export/import
- [ ] Performance passes: TCTI tuning, guest boot-time (<10 s target via
      snapshot-resume)

## Phase 5 — Distribution
- [ ] Runtime JIT capability detection + `QEMUJITEngine` for alt-marketplace
      builds (EU)
- [ ] App Review dry run against rule 4.7 (emulated content sandboxing)
- [ ] Rename the product (trademark), app icon, store assets
- [ ] TestFlight → App Store submission; AltStore PAL build in parallel

## Later / stretch
- Windows-ARM "power bottles" (user-supplied image) for Wine-incompatible apps
- GPU acceleration (Venus → MoltenVK)
- Community bottle recipes (known-good install scripts per app, à la
  CrossOver's compatibility database)
