# Native port master plan: Wine in-process on iOS

Decision (2026-08-05): this project targets a **native Wine port** — Wine
compiled for ARM64 running inside the app's own process, with Box64/FEX
translating the Windows app's x86 code, and DXVK → MoltenVK → Metal for
graphics. The VM approach is retired to `docs/appendix-vm-approach.md` as a
fallback only. We accept the consequences: this is a deep fork of Wine, and
the product lives outside the App Store (sideload / alt-marketplace + JIT
enablers) indefinitely.

The strategy that makes this tractable: **develop the fork host-first.**
Every invasive change to Wine is built and regression-tested on Linux/macOS
with the iOS constraints *simulated* (no fork/exec, single address space),
where debuggers, sanitizers, and CI exist. iOS bring-up then ports a working
system instead of debugging a broken one on-device.

## Blocker 1 — Wine is multi-process; iOS forbids processes

This is the project. Wine's model: `wineserver` as a separate process holding
all kernel objects, plus one host process per Windows process, talking to the
server over per-process unix socketpairs.

Attack plan, in dependency order:

1. **Pseudo-process substrate** (`native/pseudoproc/`) — **prototype done,
   tests passing.** A Windows "process" becomes a thread group with a shared
   per-process context: identity (pid/name/argv), spawn/wait/exit-code
   semantics, and thread affiliation. `pproc_spawn` is the shape
   `CreateProcess` will lower to.
2. **wineserver as a thread** (`native/pseudoproc/psrv.c` proves the model) —
   wineserver's wire protocol is socketpair-based and does not care whether
   the peer is a process or a thread. The fork keeps `server/` largely intact:
   same request structs, same poll loop, run on a dedicated thread, one
   socketpair per pseudo-process. The prototype demonstrates concurrent
   clients and a consistent global handle table in one address space.
3. **NTDLL/loader surgery** — the invasive part, in rough order of pain:
   - `NtCreateUserProcess` → pseudo-process spawn; PE image mapped into the
     shared address space, relocated on base-address conflicts (PE supports
     this; ASLR-era binaries relocate fine).
   - **Per-pseudo-process PEB/TEB**: TEB stays per-thread (register-based —
     x18/TPIDRRO_EL0 handling), PEB becomes per-pseudo-process, reached via
     the thread's process context instead of a fixed address.
   - **Loader instancing**: each pseudo-process needs its own module list and
     its own instance of per-process DLL state. Non-shareable DLL globals are
     the long tail — same class of problem Wine already tracks for
     `.shared` PE sections, inverted.
   - **Handle table**: already server-side in Wine; per-process handle
     namespaces keep working because the server keys them by process object,
     not by host pid. fd passing over SCM_RIGHTS becomes unnecessary (same
     address space — fds are directly shareable; this *simplifies* a hot path).
   - **Process termination**: `NtTerminateProcess` cannot rely on the kernel
     reclaiming everything. Needs cooperative unwind of the thread group +
     server-side cleanup of the process object. Leaks on abnormal death are
     accepted early (document, fix incrementally).
4. **What we don't try to fix**: cross-process address-space isolation.
   Misbehaving apps can stomp each other inside a bottle. One bottle = one
   host process = one app + its helpers; isolation between bottles comes from
   iOS itself.

## Blocker 2 — JIT / W^X

- Runtime codegen (Box64/FEX dynarec, x86 apps' own JITs re-emitted) uses the
  **dual-mapping** technique proven by DolphiniOS/UTM: allocate the code
  buffer twice via `vm_remap` — one RW mapping for emission, one RX for
  execution — valid under the debugger-assisted JIT path.
- Entitlement acquisition is the install channel's job: AltStore/SideStore +
  StikDebug/SideJITServer-style enablers. The engine probes at runtime
  (`EngineCapabilities.jitAvailable()`), and refuses x86 bottles without it
  rather than pretending an interpreter tier is playable.
- **Page size**: Apple Silicon uses 16 KB pages; Windows binaries and Wine
  assume 4 KB granularity for PE section mapping and guard pages. Box64
  already runs on 16 KB-page hosts (Asahi ARM64); Wine's `mmap` emulation
  layer must round section mappings and emulate 4 KB protection granularity
  where PEs depend on it. Tracked as its own workstream — this bites early
  (loader) and late (copy-protection tricks).

## Blocker 3 — Graphics, audio, input

- **Graphics**: DXVK (D3D9/10/11 → Vulkan) → MoltenVK (Vulkan → Metal),
  all in-process. MoltenVK is mature on iOS; DXVK-on-MoltenVK's gaps
  (geometry shaders, transform feedback, BC formats) are known quantities
  from the Whisky/CrossOver-mac world rather than research topics. D3D12 via
  vkd3d-proton is explicitly out of scope until D3D11 is solid.
- **Audio**: a winepulse-style driver backed by CoreAudio/AVAudioEngine.
- **Input/windowing**: a `winemac.drv`-analog targeting UIKit — one
  fullscreen `CAMetalLayer` surface per bottle, Win32 windows composited by
  Wine's own window manager onto it; touch → mouse, Pencil → pen with
  pressure, GameController framework → XInput.

## Blocker 4 — Distribution

Sideload-first, permanently:

- Primary: signed IPA via AltStore/SideStore source + JIT enabler; EU
  alt-marketplace build where that eases installation (it does not lift the
  JIT entitlement — the enabler flow is still required).
- Dev channel: TrollStore/jailbreak for fast iteration on real hardware.
- The DolphiniOS lifestyle is the accepted cost of doing this for real.

## Workstreams and sequencing

```
WS-A  pseudo-process substrate          [prototype DONE — harden, then port into Wine]
WS-B  wine fork bring-up (host-first)   build Wine ARM64; wineserver-in-thread;
                                        single .exe running with no fork/exec (Linux, then macOS)
WS-C  iOS process bring-up              winelib as static libs/dylibs in the app bundle;
                                        dlopen, TEB register setup, 16K-page mmap layer
WS-D  x86 translation                   Box64 primary (16K-page support exists), FEX as A/B;
                                        dual-mapped dynarec cache
WS-E  graphics/audio/input              DXVK→MoltenVK spike: run d3d11 triangle demo end-to-end
WS-F  packaging                         IPA, AltStore source, JIT-enabler UX, bottle UI wiring
```

A–B are the critical path and pure Linux/macOS work — they need zero Apple
permission and produce the fork that everything else consumes. C is where
Apple pain begins; D/E are parallelizable once B boots a real .exe.

## Milestone definition of "it works"

M1: `wine notepad.exe` (ARM64 build, bundled winelib) runs on Linux with
    fork/exec compiled out — all processes are pproc threads.  ← current target
M2: same binary architecture boots on a jailbroken/TrollStore iPad; notepad
    renders into a UIKit surface.
M3: an x86 Win32 game binary (something D3D9-era) runs via Box64 + DXVK +
    MoltenVK on-device with JIT enabler.
M4: sideloadable IPA with the bottle UI: install an .exe from Files, tap, play.
