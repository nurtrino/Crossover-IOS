# CrossoverPad — iOS Display Driver: Implementation Handoff

**Audience:** the engineer/agent building the graphics driver. You have the full
codebase; this doc is the map, the constraints, and the plan — not a code dump.
Read the referenced files directly.

**Goal:** make a *GUI* Windows program present a window on iOS. Today the runtime
runs **console** programs in‑process (see `docs/` + `CrossoverPad/Sources/WineHost/`
and `WineConsoleSession.swift`); `cmd.exe` works on device. GUI programs fail
because Wine has **no iOS display driver** — there is no surface for Windows to
draw into and no window manager to host top‑level windows. That is the single
missing architectural piece for anything graphical (notepad → installers →
eventually games).

**Definition of done for v1:** launch `notepad.exe` (or `winecfg`) in a bottle
and see its real window, drawn with its actual pixels, on the iOS screen, with
touch acting as the mouse. Everything past that (multiple windows, GL/Vulkan,
performance) is follow‑on.

---

## 0. TL;DR of the shape of the work

You are writing **two halves that meet at a shared framebuffer**:

1. **A Wine graphics/USER driver** (a new `winendrv`/`wineios.drv` built inside
   the Wine tree, alongside `dlls/winemac.drv` and `dlls/winex11.drv`). It
   implements the `struct user_driver_funcs` vtable from
   `include/wine/gdi_driver.h` and registers via `__wine_set_user_driver`. Its
   job: create/destroy/position top‑level windows, hand Wine a
   `struct window_surface` to rasterize into, and translate iOS input + display
   metrics into Windows messages/DEVMODEs. **It runs on Wine threads, in‑process.**

2. **A UIKit/Metal presenter in the app** (`CrossoverPad/Sources/…`). It owns the
   `UIWindow`/`CAMetalLayer`, runs on the **UIKit main thread**, and blits the
   shared surface(s) to the screen each frame. It also forwards touch/keyboard
   events *to* the driver.

The contract between them is a **shared memory framebuffer + a thread‑safe event
queue**, because the two run on different threads (Wine bg threads vs. UIKit main
thread) in the same process. Get that boundary right and the rest is mechanical.

---

## 1. How Wine's display layer actually works (orient here first)

Read these before writing anything:

- `include/wine/gdi_driver.h` — the two vtables you implement:
  - `struct user_driver_funcs` — windowing, input, display modes, cursor, and an
    embedded `struct gdi_dc_funcs dc_funcs` for device‑context/GDI ops.
  - `struct window_surface` + `struct window_surface_funcs` — **the pixel
    bridge**. A window surface owns a bitmap (BITMAPINFO + bits) that Wine draws
    into with normal GDI; the driver decides where those bits live and how they
    reach the screen.
- `dlls/win32u/` — the modern home of GDI + USER (there is no separate user32/gdi32
  kernel side anymore). Grep for `user_driver` and `__wine_set_user_driver` to see
  how the driver is loaded and dispatched. `dlls/win32u/driver.c` is the null/default
  driver — **your baseline**; every func you don't implement falls back to it.
- `dlls/winemac.drv/` — **your primary reference implementation.** It is the closest
  analog: a single‑process, Cocoa‑hosted driver where a separate "Mac" thread owns
  the UI and Wine threads talk to it. The macOS→iOS gap is UIKit/Metal instead of
  Cocoa, one hard main thread, and touch instead of mouse. Steal its structure:
  `macdrv_main.c`, `window.c`, `surface.c`, `event.c`, `mouse.c`, `keyboard.c`,
  `display.c`, `opengl.c`.
- `dlls/winex11.drv/` — secondary reference, useful for the pure‑software
  `window_surface` path (`bitblt.c`, `window.c`, `init.c`) if you want a model with
  no compositor of its own.

The call flow you care about for v1:

```
app: LdrLoadDll/CreateWindow (guest)
  → win32u USER → user_driver_funcs.pCreateWindow / pWindowPosChanging /
                  pWindowPosChanged / pCreateWindowSurface
  → your driver allocates a window_surface (shared framebuffer)
  → guest paints via GDI into surface bits (BeginPaint/EndPaint, WM_PAINT)
  → your window_surface_funcs.flush() marks dirty rects
  → UIKit presenter reads the surface + dirty rects → CAMetalLayer → screen
```

The reverse flow (input): iOS touch/key → app presenter → thread‑safe queue →
driver's `pProcessEvents`/`pClipboardWindowProc`/keyboard funcs → `NtUser…`
message injection → guest `WndProc`.

---

## 2. iOS‑specific constraints (this is where the difficulty is)

These are the things that make iOS *not* macOS. Design around them from line one.

1. **Single, non‑negotiable UIKit main thread.** UIKit is main‑thread‑only, and on
   iOS the app owns the main thread with its own run loop. Wine runs on **background
   threads** in‑process (that is exactly why the console works — see
   `apple_main_thread()` in `dlls/ntdll/unix/loader.c`, which no‑ops off the main
   thread). **Corollary:** the driver must NEVER call UIKit directly from a Wine
   thread. All UIKit work is marshaled to the main thread (`DispatchQueue.main`).
   winemac.drv already assumes "UI lives on a dedicated thread"; on iOS that thread
   is fixed and shared with the app, so the marshaling is stricter.

2. **No X11, no Cocoa windows.** Windows/surfaces are `UIWindow` + `UIView` +
   `CAMetalLayer` (or `CALayer` with a `CGImage` for a dead‑simple first cut).
   Prefer Metal for the blit; it is the only path that will be fast enough for a
   game later and is fine for v1 too.

3. **JIT / W^X.** GUI adds no new JIT requirement beyond the console (the PE loader
   already needs it — StikDebug provides it). But GL/Vulkan translation later may
   want codegen; keep that in mind, not now.

4. **Coordinate systems + Retina scale.** Windows uses top‑left origin, integer
   pixels, and a virtual desktop. UIKit uses points (not pixels) with a
   `contentsScale` (2x/3x) and a top‑left origin for frames but a bottom‑left‑ish
   mental model in some APIs. Decide the mapping ONCE: run the Windows desktop at
   the screen's **pixel** resolution (`UIScreen.nativeBounds`), report that as the
   monitor via `pUpdateDisplayDevices`, and blit 1:1. Do not try to be clever with
   points early.

5. **App lifecycle / backgrounding.** iOS can suspend the app; a Wine thread mid‑GDI
   doesn't know. The presenter must stop drawing on `scenePhase == .background` and
   the driver must survive it. For v1, pause rendering; don't try to preserve GPU
   surfaces across suspension.

6. **Safe areas, notches, rotation, on‑screen keyboard.** v1: lock orientation
   (portrait or landscape, pick one), render into the safe area, and bring up a
   `UIKeyboard`/hardware‑keyboard path for text. Don't fight rotation yet.

7. **Memory.** The framebuffer is width×height×4 per top‑level window. At native
   iPad resolution that's ~10–16 MB each. Fine, but the `increased-memory-limit`
   entitlement (already in `CrossoverPad.entitlements`) matters once there are many.

8. **The forkless/in‑process model.** Everything is one Unix process. There is no
   separate `explorer.exe` desktop process spawning; the driver's `pCreateDesktop`/
   `pSetDesktopWindow` run in the same address space as the app. Good news: sharing
   a framebuffer pointer between "driver" and "presenter" is just a pointer — no IPC,
   no shared‑memory handles. Bad news: a driver crash is an app crash; be defensive.

---

## 3. Architecture decision (recommended)

Build a **new in‑tree Wine driver** rather than bolting UIKit onto winemac.drv.
winemac.drv is deeply Cocoa/AppKit‑coupled (its own `-[WineApplication]`, event
loop, `NSWindow`). Forking it to UIKit is more work than a focused new driver that
copies its *structure* but not its Cocoa. Name it `dlls/winendrv` (or
`winehost.drv`); build target `winendrv.drv` + `winendrv.dll` in the arm64 sets.

Split responsibilities across the process boundary like this:

```
Guest (PE) ── win32u ──▶ winendrv (Wine thread, Objective-C++/.m allowed on Apple)
                              │  (all UIKit calls marshaled to main via a C shim)
                              ▼
                    shared surface registry  ◀── app presenter (Swift, main thread)
                              │                         │
                              ▼                         ▼
                     window_surface bits         CAMetalLayer.present
```

- The driver is compiled as part of the Wine build (it's a unixlib `.so` +
  a thin PE `.dll`, same as winemac.drv — see its `Makefile.in`). It may use
  Objective‑C(++) (`.m`/`.mm`) and link `UIKit`, `QuartzCore`, `Metal` frameworks,
  because on device it lives in the app process that already links them. Mirror how
  winemac.drv links `-framework AppKit`.
- The **app** exposes a tiny registration API (C, via the existing bridging header)
  that the driver calls to (a) hand over a new surface + its BITMAPINFO/size/HWND,
  (b) receive "dirty rect" flushes, (c) push input back. Think of it as the
  `WineHost` bridge you already have (`CrossoverPad/Sources/WineHost/`) grown a
  graphics section.

Why a shared registry object instead of the driver touching UIKit itself: it keeps
**all** UIKit on the Swift/main‑thread side, where the app already manages the view
hierarchy and lifecycle, and keeps the Wine driver to pure data + a lock + a
main‑thread dispatch. This is the cleanest thread story.

---

## 4. The v1 vertical slice (do this first, resist scope)

Get ONE opaque top‑level window on screen with correct pixels and mouse. In order:

**4.1 Register a stub driver.** New `dlls/winendrv`. In its init, call
`__wine_set_user_driver(&winendrv_funcs, WINE_GDI_DRIVER_VERSION)`. Implement almost
nothing — let `dlls/win32u/driver.c`'s nulldrv back everything. Confirm win32u picks
it up (grep how the driver name is resolved: registry `\Drivers\Graphics` / the
`WINEDRV`/`Wine\Drivers` config, and `load_driver()` in win32u). You must make
win32u *choose* your driver instead of the (disabled) mac driver — see how patch
0006 disabled winemac in `configure.ac`; you'll add winendrv as the iOS default.

**4.2 Report a display.** Implement `pUpdateDisplayDevices` to register a single
monitor at `UIScreen.nativeBounds` size, 32bpp. Implement `pChangeDisplaySettings`
as a no‑op success. Without this, USER has no desktop rect and nothing lays out.
Reference: winemac.drv `display.c`, winex11.drv `display.c` / `xrandr.c`.

**4.3 Desktop window.** Implement `pCreateDesktop`/`pSetDesktopWindow`/
`pDesktopWindowProc` minimally (copy nulldrv/winemac behavior). The desktop is the
root; for v1 it can be an invisible full‑screen container.

**4.4 Window surface — THE core.** Implement `pCreateWindowSurface` +
`pWindowPosChanging`/`pWindowPosChanged` and a `struct window_surface` whose bits
are a plain malloc'd BGRA buffer (top‑down BITMAPINFO, negative height). Implement
`window_surface_funcs`:
   - `lock`/`unlock` — a mutex around the bits.
   - `flush(surface, rect, dirty)` — record the dirty rectangle and signal the
     presenter (main‑thread dispatch) that this HWND changed.
   - `get_bitmap_info`, `create_pixmap`/etc. as the struct requires (see current
     `window_surface_funcs` in `gdi_driver.h`; the exact members shift between Wine
     versions — implement what's declared).
   Model on winex11.drv `window.c`'s `x11drv_window_surface` (software path) — it is
   the simplest correct example of "GDI draws into a buffer I own."

**4.5 Present.** App side: for each registered surface, create a `UIView` +
`CAMetalLayer` sized to the window rect; each display link tick, if dirty, upload
the BGRA bits (`MTLTexture` via `replaceRegion` or a shared `MTLBuffer`) and draw a
full‑screen textured quad; clear dirty. Start with **one** window = one full‑screen
layer. A `CGImage`/`CALayer.contents` path is an acceptable even‑simpler v0 to prove
pixels before Metal.

**4.6 Window placement + visibility.** `pWindowPosChanged` tells you the new
on‑screen rect and Z; `pShowWindow` toggles visibility; `pSetWindowText` → nav title
(optional). For v1, force every top‑level window to full‑screen (ignore its real
rect) so you don't need a window manager yet — just show the frontmost one.

**4.7 Input.** `pSetCursorPos`/`pGetCursorPos` + inject mouse via the same path
winemac uses (`__wine_send_input` / `NtUserSendHardwareInput`). Map a single touch
to move+left‑button. Implement `pProcessEvents` to drain your thread‑safe event
queue (touches, key events, display changes) and return whether anything was
handled — USER calls it from the guest's message loop, so this is your pump.

When 4.1–4.7 land, `notepad.exe` shows a window you can tap into. That's v1.

---

## 5. `user_driver_funcs` — implementation priority

Everything not listed = leave to nulldrv initially.

**Must‑have (v1):**
`pUpdateDisplayDevices`, `pChangeDisplaySettings`, `pCreateDesktop`,
`pSetDesktopWindow`, `pDesktopWindowProc`, `pCreateWindow`, `pDestroyWindow`,
`pCreateWindowSurface`, `pWindowPosChanging`, `pWindowPosChanged`, `pShowWindow`,
`pGetDC`/`pReleaseDC`, `pProcessEvents`, `pSetCursor`, `pSetCursorPos`,
`pGetCursorPos`, `pSetCapture`.

**Soon after (v1.1):** `pSetWindowText`, `pSetWindowStyle`, `pActivateWindow`,
`pSetParent`, `pSetWindowRgn`, `pScrollDC`, `pMoveWindowBits`,
`pSetLayeredWindowAttributes`/`pUpdateLayeredWindow` (many apps use layered windows
for menus/tooltips), keyboard set: `pToUnicodeEx`, `pVkKeyScanEx`, `pMapVirtualKeyEx`,
`pGetKeyNameText`, `pKbdLayerDescriptor`/`pReleaseKbdTables` (steal a US layout table
from winemac/winex11 keyboard.c), `pActivateKeyboardLayout`, `pBeep`.

**Later:** `pClipboard*`, `pNotifyIcon*` (systray — likely irrelevant on iOS),
`pVulkanInit`, `pOpenGLInit` (see §8), IME, `pFlashWindowEx`, `pClipCursor`.

Also the embedded `gdi_dc_funcs` (`dc_funcs`): for a pure software surface you can
leave nearly all of it to nulldrv — GDI rasterizes into your buffer without driver
help. You only need DC funcs when you accelerate blits or add GL. **Don't.** Not v1.

---

## 6. Threading model — get this exactly right or you'll chase deadlocks forever

- **Wine threads** call driver functions. They may allocate/lock the surface,
  memcpy bits, enqueue events, and request main‑thread work. They must not block
  waiting on the main thread while holding the surface lock (classic
  lock‑ordering deadlock, and the in‑process runtime already has a fragile loader
  critical section — see the x18/unwind notes in the fork; do not add lock cycles).
- **Main (UIKit) thread** owns all `UIView`/`CAMetalLayer`/`UIWindow` mutations and
  presents. It reads surface bits under the surface lock, copies to a texture,
  unlocks, then draws. Keep the lock hold short (copy out, release).
- **Marshaling primitive:** a small C shim in the driver posts a block/struct to a
  main‑thread queue (either `dispatch_async(dispatch_get_main_queue(), …)` from the
  `.m` file, or a lock‑protected ring buffer the Swift `CADisplayLink` drains). For
  operations the guest needs a *result* from (rare in v1 — mostly create/destroy
  window), use a `dispatch_semaphore` round‑trip but with a timeout and never under
  the surface lock.
- **Never** call `NtUser*`/win32u from the main thread — those must run on a Wine
  thread with a valid TEB (x18!). Input injection therefore happens on a driver
  worker thread that drains the input queue, not directly from the touch handler.
  Pattern: touch handler (main) → enqueue → `pProcessEvents` (Wine thread) drains →
  `NtUserSendHardwareInput`.

Sketch of the safe cycle:

```
guest paint ─(Wine thr)─▶ surface.lock ▶ GDI writes bits ▶ surface.unlock
                          surface.flush ▶ enqueue(dirtyRect,hwnd) ▶ dispatch_async(main)
main displaylink ─▶ for dirty hwnd: surface.lock ▶ memcpy→texture ▶ surface.unlock ▶ present
UIKit touch (main) ─▶ inputQueue.enqueue(event)
driver pump (Wine thr, from guest msg loop pProcessEvents) ─▶ inputQueue.drain ▶ NtUserSendHardwareInput
```

---

## 7. Registration & config (how win32u finds your driver)

- win32u loads the graphics driver by name. Trace `load_driver` /
  `create_driver` in `dlls/win32u/` and the `\Registry\Machine\System\…\Drivers\
  Graphics` value (and the `HKCU\Software\Wine\Drivers` `Graphics` override).
  Default is `mac,x11`. You must add `winendrv` and make it the iOS default.
- Mirror how **patch 0006** (`native/patches/0006-ios-src.patch`) disabled
  winemac in `configure.ac`: add your driver's `configure.ac` enable block, its
  `dlls/winendrv/Makefile.in` (link `-framework UIKit -framework QuartzCore
  -framework Metal`, and `WINE_CROSS`/PE bits like winemac), and set the default
  `Graphics` driver to `winendrv` on iOS in the registry defaults (see
  `loader/wine.inf.in` / `programs/wineboot` where default drivers get written —
  but the prefix is prebuilt, so you may instead set it via the app's env:
  `HKCU\Software\Wine\Drivers\Graphics=winendrv` baked into the shipped prefix's
  `user.reg`, or `WINEDLLOVERRIDES`/registry seed in `WineConsoleSession`/`WineHost`).
- Because the prefix is **prebuilt and shipped** (see the IPA pipeline
  `build-ipa-runtime.yml` "Assemble WineRuntime"), the cleanest switch is: seed
  `Graphics=winendrv` into the prefix's `user.reg` during prefix prep, OR have the
  app write it before first GUI launch. Document whichever you pick.

---

## 8. GL / Vulkan (NOT v1 — but design so you don't wall it off)

Games need GPU. Plan, don't build:

- `pOpenGLInit`/`pVulkanInit` return driver function tables. On iOS there is **no
  system OpenGL/Vulkan for the guest** — the realistic path is:
  - **Vulkan → Metal via MoltenVK**, then Wine's `winevulkan` targets MoltenVK; or
  - **WGL/OpenGL → Metal via ANGLE/MoltenGL**, harder.
- Either way the guest renders into a texture your presenter composites — same
  surface bridge, just GPU‑to‑GPU instead of CPU blit. Keep the window_surface
  abstraction able to be **GPU‑backed** (an `IOSurface`/`MTLTexture`) later; for v1
  it's CPU BGRA, but don't hardcode assumptions that block a GPU surface swap.
- DirectX (most games) → `vkd3d`/DXVK → Vulkan → MoltenVK → Metal. Long road.
  Out of scope here; just don't design the surface bridge in a way that forbids it.

---

## 9. Build & integration checklist

- New patch (call it `0007-ios-display-driver.patch`, quilt‑style like the others in
  `native/patches/`, sequenced after 0006 in `series`) OR extend 0006. Prefer a
  **new patch** so the display work is isolable. It adds `dlls/winendrv/*`,
  `configure.ac` enable + default, and Makefile wiring. Regenerate `configure` with
  `autoconf` (the iOS build already does this — see the workflows).
- The driver `.so` (unixlib) and `.dll` (PE thunk) must land in the runtime bundle:
  update the IPA assembly in `build-ipa-runtime.yml` (it already globs
  `dlls/*/*.so` and `dlls/*/aarch64-windows/*.dll`, so a normally‑built winendrv is
  picked up automatically — verify it is).
- App side: extend the `WineHost` bridge (`CrossoverPad/Sources/WineHost/`) with the
  surface‑registry + input C API, and add the Metal presenter view + a
  `GuestWindowView`/`GuestScreenView` SwiftUI host. Wire a bottle's "Launch (GUI)"
  action to start the guest exe the same way `WineConsoleSession` starts `cmd.exe`,
  but pointing at e.g. `notepad.exe`, and present the screen view instead of the
  terminal.
- Keep `SWIFT_OBJC_BRIDGING_HEADER` current; the driver's app‑facing C API goes in
  a header the bridging header imports.

---

## 10. Testing strategy

- **CI simulator (fast loop):** the `wine-ios-sim-run.yml` harness already runs the
  runtime under the iOS Simulator on Apple‑silicon runners. Add a headless driver
  smoke: start `notepad.exe` (or a tiny test exe that `CreateWindow`s + fills the
  client area) and assert the driver got `pCreateWindowSurface` + a non‑empty flush
  (log the dirty rects and a checksum of the bits). You can validate the *driver*
  end‑to‑end without a real screen by dumping the surface to a PNG in the harness
  and eyeballing/【pixel‑checking】it. **Do this** — it's the difference between a
  20‑minute loop and a device round‑trip.
- **Device:** sideload, attach StikDebug, launch the GUI bottle. The presenter is
  only exercisable on device/simulator‑app, not the CLI harness.
- Beware the known **flakiness**: multi‑threaded guests (services/RPC, GUI apps
  spawn more threads) are more likely to hit the residual x18/unwind deadlock than
  single‑threaded `cmd`. If GUI apps deadlock where `cmd` didn't, it's likely that,
  not your driver — cross‑check against the x18 notes before blaming graphics.

---

## 11. Milestones

| # | Deliverable | Proof |
|---|-------------|-------|
| M0 | winendrv registers; win32u selects it; nulldrv‑backed | boots a GUI app to the point it *tries* to make a window (no crash) |
| M1 | Display reported; desktop window exists | `GetSystemMetrics(SM_CXSCREEN)` = screen px; guest lays out |
| M2 | Software `window_surface` + Metal/CALayer present | `notepad.exe` window visible with correct pixels (CI PNG dump + device) |
| M3 | Touch→mouse + basic keyboard | can click menus, type into notepad |
| M4 | Multiple top‑level windows + Z‑order + move/resize | dialogs/menus render in place |
| M5 | Layered windows, cursors, clipboard | menus/tooltips/carets correct |
| M6 | GPU surface path (IOSurface/MTLTexture) | groundwork for GL/Vulkan |

Ship M2 as the "GUI works" moment; it's the analog of the `cmd` milestone.

---

## 12. Gotchas / landmines (read twice)

1. **Do not call UIKit from a Wine thread.** Every crash‑on‑launch will be this.
2. **Do not hold the surface lock across a main‑thread round‑trip.** Deadlock.
3. **Do not call win32u/NtUser from the main thread** (no valid TEB/x18). Input
   injection runs on a Wine thread draining a queue.
4. **Top‑down BITMAPINFO (negative height), BGRA/BGRX.** GDI and CoreGraphics/Metal
   disagree on origin and channel order; pick BGRA top‑down and be consistent, or
   you'll ship upside‑down blue‑tinted windows.
5. **`contentsScale` / native pixels.** Set the layer's `contentsScale` and size in
   *pixels*; run the desktop at native resolution; blit 1:1. Mismatches → blur or a
   window in the corner.
6. **Guest paints only dirty rects.** Respect the flush rect; full‑surface re‑upload
   every frame is fine for v1 but will be your first perf problem for games.
7. **The prefix is prebuilt & read‑only in the bundle** → the app copies it to
   Documents (see `WineConsoleSession.prepareRuntime`). Any registry seeding for the
   driver must happen on that writable copy (or be baked into the shipped prefix).
8. **App backgrounding** mid‑paint: guard the presenter on `scenePhase`; don't touch
   a `CAMetalLayer` drawable when not foreground.
9. **winemac.drv is your friend but it's Cocoa.** Copy structure and the
   input/keyboard tables; do not copy AppKit calls.
10. **One process.** No `explorer.exe`, no separate desktop process — `pCreateDesktop`
    runs in‑process. Don't assume the multi‑process desktop model from desktop Wine.

---

## 13. File index (where to look / where to add)

Reference (read):
- `include/wine/gdi_driver.h` — the vtables (`user_driver_funcs`,
  `window_surface(_funcs)`, `gdi_dc_funcs`, `gdi_device_manager`).
- `dlls/win32u/driver.c` — nulldrv + driver dispatch/loading. Your fallback + the
  registration path.
- `dlls/win32u/sysparams.c`, `dlls/win32u/window.c`, `dlls/win32u/input.c` — how USER
  calls the driver for display devices, window pos, and input injection.
- `dlls/winemac.drv/*` — closest architectural analog (single‑process, dedicated UI
  thread). Primary model.
- `dlls/winex11.drv/window.c` (+ `bitblt.c`) — simplest software `window_surface`.
- `native/patches/0006-ios-src.patch` — how the fork disables winemac and does
  iOS‑specific configure; mirror for enabling winendrv.

Add (write):
- `native/wine/dlls/winendrv/` — the driver (`.c`/`.m`/`.mm`, `Makefile.in`,
  `winendrv.spec`), via a new `native/patches/0007-ios-display-driver.patch`.
- `native/patches/series` — sequence 0007 after 0006.
- `CrossoverPad/Sources/WineHost/` — extend the C bridge with the surface registry +
  input API.
- `CrossoverPad/Sources/Views/GuestScreenView.swift` (+ a Metal presenter) — the
  UIKit/Metal side.
- `.github/workflows/wine-ios-sim-run.yml` — add the GUI/driver smoke (surface → PNG).

---

## 14. First concrete task for the next agent

1. Create `dlls/winendrv` with an init that calls `__wine_set_user_driver` with an
   all‑nulldrv vtable, make win32u select it on iOS, and prove a GUI exe boots far
   enough to call `pCreateWindow` (log it). No pixels yet.
2. Then §4.2–4.5 to get one `notepad` window on screen (M2).

Everything else follows from that spine. Keep the thread boundary sacred and ship
M2 before touching GL. Good luck.
