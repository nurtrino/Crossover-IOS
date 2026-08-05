# Architecture

The product goal is CrossOver's UX, not CrossOver's implementation: the user
picks a Windows program, we run it. All emulation machinery is hidden behind
the **bottle** abstraction.

```
┌────────────────────────────────────────────────────────────┐
│ SwiftUI app (iPadOS/iOS)                                   │
│                                                            │
│  BottleListView ─ NewBottleWizard ─ RunningAppView (Metal) │
│        │                 │                   ▲             │
│        ▼                 ▼                   │ framebuffer │
│  BottleStore ──────► VirtualMachineEngine ◄──┘  + input    │
│  (JSON + disk        (protocol)                            │
│   overlays)              │                                 │
│            ┌─────────────┼──────────────┐                  │
│            ▼             ▼              ▼                  │
│       MockEngine    QEMUTCTIEngine  QEMUJITEngine          │
│       (dev/tests)   (App Store)     (EU/alt-store)         │
│                          │                                 │
│              ┌───────────┴───────────┐                     │
│              │ Guest VM (per bottle) │                     │
│              │ Alpine ARM64          │                     │
│              │  └─ Wine (ARM64EC)    │                     │
│              │      └─ Box64/FEX     │                     │
│              │          └─ app.exe   │                     │
│              └───────────────────────┘                     │
└────────────────────────────────────────────────────────────┘
```

## Components

### 1. Engine layer (`Sources/Engine`)

`VirtualMachineEngine` is a protocol so the UI never knows which backend runs a
bottle:

- **MockEngine** — in-process fake used for UI development, previews, and unit
  tests. Ships now.
- **QEMUTCTIEngine** — QEMU with the Threaded-Code interpreter, embedded via
  [QEMUKit](https://github.com/utmapp/QEMUKit) (the same stack UTM SE ships on
  the App Store). Baseline for all install channels.
- **QEMUJITEngine** — same QEMU, JIT-enabled when the install channel provides
  the debugger-assisted entitlement path (AltStore PAL etc.). Selected at
  runtime by capability probe, never by build flag alone.

Engine selection: probe once at launch (`EngineCapabilities.detect()`), cache,
and surface in Settings so users understand which tier they're on.

### 2. Bottles (`Sources/Bottles`)

A bottle = one Windows program + its isolated environment.

On disk (inside the app container):

```
Bottles/
  <uuid>/
    bottle.json        # metadata: name, engine config, guest type, entry point
    base -> shared     # base guest image is shared, content-addressed
    overlay.qcow2      # per-bottle copy-on-write disk overlay
    shared/            # host<->guest file exchange (exposed via virtfs/9p)
```

The base image (Alpine + Wine + Box64) is shared across bottles; each bottle
only stores its qcow2 overlay (the installed app + Wine prefix). This keeps a
10-bottle setup near 1× base-image cost, mirroring CrossOver's bottle economy.

Bottle lifecycle: `create → install (run installer .exe) → snapshot → run`.
The post-install snapshot lets "Reset bottle" be instant and safe.

### 3. Guest image pipeline (repo: `guest/`, Phase 3)

A reproducible build (Docker/mkosi script, CI-built, content-addressed
artifact) producing the ARM64 rootfs with:

- Wine built with the WoW64/ARM64EC configuration
- Box64 (and FEX as an A/B option) registered via binfmt for x86/x86-64 PE
- A tiny init that reads the bottle manifest from a virtfs mount and launches
  either the installer or the target app full-screen (no desktop, cage/weston
  kiosk compositor)
- virtio-serial control channel: host app ⇄ guest agent (launch, quit,
  clipboard, DPI, window title)

### 4. Display & input (Phase 4)

- Guest scanout via virtio-gpu → SPICE/pixman surface → `CAMetalLayer` blit.
  QEMUKit already provides this plumbing; we render it inside
  `RunningAppView` instead of a general-purpose VM console.
- Touch → tablet absolute pointer events; Apple Pencil maps to pen with
  pressure; hardware keyboard passes through; on-screen keyboard summoned via
  guest-agent focus events.
- Stretch: Venus (Vulkan-over-virtio) → MoltenVK for GPU acceleration. Not a
  committed feature; interpreter-tier devices will never get it.

### 5. What we deliberately do NOT build

- A general VM manager UI (that's UTM; we are app-centric by design).
- Our own emulator (we embed QEMU via QEMUKit and contribute patches upstream).
- Anything requiring jailbreak/TrollStore as a user-facing requirement.
