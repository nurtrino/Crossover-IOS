# Feasibility: Windows apps on iPadOS/iOS

This document is the ground truth for what is and is not possible on the platform.
Every design decision in `ARCHITECTURE.md` traces back to a constraint here.

## 1. Why CrossOver/Wine cannot be ported natively

CodeWeavers' CrossOver is a packaged distribution of [Wine](https://www.winehq.org/).
Wine is not an emulator — it maps Win32/Win64 API calls onto the host OS at native
speed. That model is fundamentally incompatible with iOS:

| Wine requirement | iOS reality |
|---|---|
| `fork`/`exec` — Wine spawns `wineserver` plus one process per Windows process | Third-party iOS apps cannot spawn processes. Everything must live in one process. |
| Load and execute unsigned code (Windows PE binaries) | iOS enforces W^X and code signing; a page can never be both writable and executable without special entitlements. |
| JIT / runtime code generation (needed by Box64/FEX for x86→ARM translation, and by many Windows apps themselves) | The dynamic-codesigning entitlement is reserved for Apple's own apps (Safari/JavaScriptCore). Workarounds (debugger-assisted "JIT hacks", TrollStore, jailbreak) are not distributable products. |
| A windowing system to map Win32 windows onto | No X11/Wayland; UIKit only, single foreground app model. |
| Full filesystem semantics (case handling, drive letters mapped over a real FS) | Sandboxed container only. Manageable, but adds friction. |

Additionally, CrossOver's own source is proprietary. Any project here builds on
Wine (LGPL) and other open-source components — never on CodeWeavers code.

**Conclusion:** a native port is not a product; it is a multi-year research
project that ends at a jailbreak-only demo. The compatibility layer must run
*inside* an emulated guest instead.

## 2. What has actually shipped (precedent)

- **UTM SE** (July 2024) — a QEMU-based PC emulator approved for the App Store
  after Apple's 2024 rule change allowing PC emulator apps. It runs with the
  TCG **interpreter** (TCTI) because the App Store build gets no JIT. It can run
  Windows guests; performance is usable for old/light software, poor for modern
  software. ([App Store listing](https://apps.apple.com/us/app/utm-se-retro-pc-emulator/id1564628856),
  [approval coverage](https://apple.slashdot.org/story/24/07/14/0434227/apple-approves-pclinuxmac-emulating-app-utm-se-for-app-store-reversing-earlier-rejection))
- **UTM with JIT via AltStore PAL (EU)** — under the DMA, alternative
  marketplaces can distribute UTM with debugger-assisted JIT enabled; this is
  how an iPad Air was shown running Windows 11 ARM at usable speed
  ([mjtsai.com writeup](https://mjtsai.com/blog/2025/07/23/ipad-air-runs-windows-11-arm-via-emulation/)).
- **Wine on iOS** — no official or credible port exists; Wine's supported
  platforms remain Linux/FreeBSD/macOS with Android experimental
  ([Wine, Wikipedia](https://en.wikipedia.org/wiki/Wine_(software))).

## 3. Distribution paths, ranked

| Path | JIT | Performance | Reach | Notes |
|---|---|---|---|---|
| **App Store (worldwide)** | No — TCG interpreter only | ~5–20× slowdown vs native; fine for Win9x-era and lightweight Win32 apps | Largest | Must satisfy review rule 4.7 (emulators may load downloaded content); UTM SE is the template. Cannot ship copyrighted OS images — user supplies them, or we ship a free guest (Linux+Wine). |
| **AltStore PAL / alt marketplaces (EU)** | Yes (debugger-assisted) | Near-UTM-with-JIT; Windows 11 ARM usable on M-series iPads | EU only | Best real-world performance per effort. |
| **TrollStore / jailbreak** | Yes | Best | Tiny, unstable | Dev/research channel only; never a product plan. |

**Strategy:** one codebase, capability-detected at runtime. Interpreter path is
the baseline everyone gets; JIT lights up where the install channel allows it.

## 4. Guest strategy: why Linux+Wine beats a Windows guest as default

Emulating a full Windows 11 ARM guest works but is heavy (RAM, disk, boot time,
licensing friction — the user must obtain Windows themselves). The default
bottle image is instead:

**Alpine/Debian ARM64 + Wine (ARM64EC/WoW64) + Box64/FEX for x86 code**, booting
headless straight into the target app.

- The guest is ARM64, so on Apple Silicon iPads the *system* emulation overhead
  is the interpreter cost only — no cross-architecture penalty for the OS itself.
  Only the Windows app's x86 code pays the Box64 translation cost, and Wine's
  ARM64EC path keeps system DLLs native-speed.
- Freely redistributable: the whole image ships inside the app or as a
  first-party download. No license hunt, no ISO wrangling — this is what makes
  the CrossOver-style UX possible at all.
- Small: an Alpine+Wine rootfs is ~1–2 GB vs 12+ GB for Windows 11.
- A user-supplied **Windows ARM guest** remains a supported "power bottle" type
  for apps Wine can't handle (same engine, different image).

## 5. Honest performance expectations

Set these publicly and early:

- **App Store build (no JIT):** 2000s-era and lightweight modern Win32 apps
  (Office-class tools, utilities, old games) — usable. Modern 3D games — no.
- **JIT build (EU/alt-channel):** substantially wider range on M-series iPads;
  DirectX 9/11-era games become plausible via DXVK inside the guest.
- Nothing GPU-heavy is credible until virtio-gpu/Venus-to-Metal passthrough
  matures (tracked as a stretch goal, not a promise).

## 6. Legal checklist

- **Trademark:** "CrossOver" belongs to CodeWeavers. The working title must be
  replaced before any public artifact (TestFlight, store listing, website).
- **Licenses:** Wine (LGPL-2.1+), QEMU (GPLv2), Box64 (MIT), FEX (MIT),
  UTM/QEMUKit (Apache-2.0/GPL components). GPLv2 code in an App Store binary is
  the classic conflict — UTM resolved it via permission/licensing structure of
  its components; we must do the same audit before shipping, not after.
- **No Windows redistribution:** never bundle or auto-download Microsoft OS
  images or DLLs. Winetricks-style fetching of Microsoft redistributables needs
  the same care CrossOver/Wine communities already apply.
- **App Review 4.7:** downloaded executable content must run only inside the
  emulated environment, with no access outside the sandbox. The architecture
  enforces this by construction (guest code never touches host APIs).

## Sources

- [UTM SE — App Store](https://apps.apple.com/us/app/utm-se-retro-pc-emulator/id1564628856)
- [Apple approves UTM SE after earlier rejection — Slashdot](https://apple.slashdot.org/story/24/07/14/0434227/apple-approves-pclinuxmac-emulating-app-utm-se-for-app-store-reversing-earlier-rejection)
- [iPad Air runs Windows 11 ARM via emulation (AltStore PAL + JIT) — Michael Tsai](https://mjtsai.com/blog/2025/07/23/ipad-air-runs-windows-11-arm-via-emulation/)
- [Wine — Wikipedia (supported platforms)](https://en.wikipedia.org/wiki/Wine_(software))
- [What is JIT and Apple's rules — How-To Geek](https://www.howtogeek.com/what-is-jit-how-apples-rules-are-holding-back-iphone-game-emulators/)
