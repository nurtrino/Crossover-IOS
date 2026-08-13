# Wine fork patches

The fork delta over the pinned Wine submodule (`native/wine`, wine-11.0),
maintained as a quilt-style series so the submodule stays at its upstream
commit and rebasing onto a newer Wine release is a matter of refreshing
patches rather than reconciling a divergent tree.

## Usage

```sh
cd native/patches
./apply.sh            # apply the series onto native/wine
./apply.sh --check    # dry-run: do all patches still apply?
./apply.sh --reverse  # restore the pristine submodule
```

`native/wine` working-tree edits are never committed to the main repo (the
main repo records only the submodule's pinned SHA), so the patches here are
the single source of truth for the fork.

## Series

### 0001-embeddable-server-shutdown.patch

Makes wineserver embeddable as a thread in the host process (the core of
Blocker 1, docs/NATIVE_PORT.md). Zero behavior change for the standalone
`wineserver` binary; adds an embedding entry point that returns instead of
calling `exit()`.

- `server/main.c`: add `wineserver_run()` (embeddable entry) and
  `server_exit()`. The server's init + `main_loop()` move into a shared
  `server_body()`; the standalone `main()` calls it and `exit()`s as before,
  while `wineserver_run()` wraps it in a `setjmp` so `server_exit()` can
  unwind back via `longjmp` and return a status.
- `server/request.c`, `server/signal.c`: route the runtime shutdown funnels
  (`close_socket_timeout`, `fatal_error`, `sigterm_callback`, signal-init
  failure) through `server_exit()` instead of `exit()`. Guarded by the
  `server_embedded` flag, so standalone behavior is byte-for-byte identical.
- `server/request.c`: `close_socket_timeout()` now unlinks the master socket
  explicitly (the embedded `longjmp` path bypasses the `atexit` cleanup), and
  `socket_cleanup()` is made idempotent (no once-guard) so it is safe to call
  from both paths.

Validated by `native/wineforge/embed_run_test` — one long-lived
`wineserver_run()` instance serves multiple clients, then a host-side signal
drives an orderly shutdown that returns 0 with the socket cleaned up and the
host process alive.

**Non-goal:** restarting the server (a second `wineserver_run()` in one
process) is not supported — process-lifetime statics (registry `root_key`,
`master_socket`, lock fd) would need a full reset. The architecture uses one
server per app session, so this is deferred unless a concrete need appears.

### 0002-no-fork-embedded-server.patch

Enforces the embedded-server model on the client side. iOS cannot fork/exec,
so a Wine client must never spawn its own `wineserver` — the server is always
the in-thread one, and its master socket must already exist.

- `dlls/ntdll/unix/loader.c`: `start_server()` refuses when
  `WINE_EMBEDDED_SERVER` is set, calling `fatal_error("no embedded wineserver
  present …")` instead of `exec_wineserver()`. Without the env var, behavior
  is unchanged (standalone Wine still auto-starts a server).

Validated by `native/wineforge/embedded_server_test.sh`: with the env var set,
the real client runs normally when the in-thread server is present, and fails
fast — forking no server — when it is absent.

This is the first client-side patch. The large remaining client-side work
(loading ntdll + the PE loader in-process and replacing `CreateProcess`→
`exec_wineloader` with a `pproc_spawn`-style in-address-space launch) is
scoped as series 0003 in `docs/DESIGN-0003-inproc-spawn.md`.

### 0003-inproc-spawn-and-peb.patch

Series 0003 in full (stages a–f): `CreateProcess` without fork/exec. Default
behavior is byte-for-byte unchanged — every new path is opt-in behind
`WINE_INPROC_SPAWN` / `WINE_INPROC_RUN` or is an equivalent indirection.

- **Stage a — spawn seam** (`dlls/ntdll/unix/process.c`): `spawn_process` is
  split into `spawn_process_fork` (the classic backend, unchanged) and
  `spawn_process_inproc` (the future in-address-space launch), dispatched on
  `WINE_INPROC_SPAWN`.
- **Stage b — PEB indirection** (`dlls/ntdll/unix/unix_private.h`,
  `process.c`): add `current_peb()` = `NtCurrentTeb()->Peb`, the hook for
  per-pseudo-process PEBs, and convert `process.c`'s post-init global-`peb`
  uses to it. Since `init_teb` sets `teb->Peb = peb` for the primary process,
  this is a zero-behavior-change step. The full file-by-file conversion
  ledger (81 uses / 10 files) is in `docs/DESIGN-0003-inproc-spawn.md`.
- **Stage c — in-address-space PE mapping**: proven standalone by
  `native/wineforge/peload_test` (map + relocate + run, two images coexisting).
- **Stage d — the attach** (`process.c`, `server.c`): `spawn_process_inproc`
  launches the child as a thread group in the same host process — child PEB
  (template-copied) + TEB, its own server socket in the PEB-keyed registry,
  and `server_init_process_inproc()` running the per-process first-thread tail
  on the handed socket.
- **Stage e — the child's own image** (`loader.c`, `env.c`, `virtual.c`):
  `main_image_info` joins `peb`/`fd_socket` as a per-pseudo-process value via
  `current_image_info()`; `build_startup_info()` is parameterised so
  `init_startup_info_inproc()` lets a child fetch its own startup info from
  the server and map its own main EXE. `server_init_process_done_inproc()` is
  split out because `init_process_done` makes the server drop the staged
  startup info — it must come *after* `get_startup_info`.

- **Stage f — the child runs** (`process.c`): the child enters its PE through
  the normal `RtlUserThreadStart` path and exits with its program's exit code.
- **Stage g — per-process PE-side loader state** (`dlls/ntdll/loader.c`): the
  loader's process-globals move into one `struct ldr_proc_state` per
  pseudo-process, reached O(1) from `peb->LdrData` via `CONTAINING_RECORD`;
  call sites unchanged via `#define`. Children build their own module list,
  load their own imports and run process attach, so DLL-importing programs
  (nested `cmd.exe`, `attrib.exe`) run in-process.

- **Stage h — bootstrap boundary** (`process.c`): `spawn_process` falls back
  to the fork backend before ntdll's PE side publishes its entry points and
  during prefix bootstrap, so cold-prefix creation works with the in-process
  flags on. iOS must ship a prepared prefix (stated architectural
  requirement).
- **Stage i — per-process fd cache + relocate-to-actual-base** (`server.c`,
  `process.c`, `virtual.c`, `unix_private.h`): the client-side handle→unix-fd
  cache becomes per-pseudo-process (handle values are per-process, so a
  shared cache aliases siblings' handles — this was the M1 GUI-child access
  violation: a child mapped kernel32 through a stale sibling fd and got an
  image with no PE header). And `map_image_into_view` now relocates an image
  to the address its view actually landed at, not the server-assigned dynamic
  base a sibling pseudo-process may already occupy.

Validated by `native/wineforge/spawn_seam_test.sh` (both dispatch directions,
server-side handshake counts, per-child image mapping) and
`inproc_run_test.sh` (children execute and exit with real codes, proved from
the server's own trace), with the whole wineforge suite staying green.

### 0004-inproc-user-session.patch

The win32u side of the in-process model (one file: `dlls/win32u/class.c`).
win32u.so is dlopen'ed once per host process, but its user-session init ran
under a single host-global `pthread_once`: with several pseudo-processes in
one host, only the first ever connected to a winstation/desktop, snapshot its
startup info, or registered the (per-process, server-side) builtin window
classes. The explorer `/desktop` child then failed every window creation with
`ERROR_INVALID_HANDLE` and win32u's desktop-start path respawned explorer
forever — the last M1 blocker. `init_user` is split: session-wide pieces
(shared session mapping, GDI shared handle table, sysparams) stay host-wide,
per-process pieces (`init_startup_info`, `winstation_init`,
`register_desktop_class`) run once per pseudo-process, keyed on the PEB.
Validated by `m1_notepad_test.sh` (the M1 gate).

### 0007-ios-display-driver.patch

The iOS display driver: `dlls/wineios.drv`, the piece that lets a GUI
Windows program present a window on iOS (docs/DISPLAY-DRIVER.md is the
design handoff this implements). Named `wineios.drv` — not the handoff's
placeholder `winendrv` — because explorer composes the module name as
`wine<name>.drv` from the `Graphics` registry value, so the driver's
selection name is simply `ios`.

Architecture (the handoff's recommended split, taken further): the driver's
unix side is **pure C with zero UIKit/Metal linkage**. All presentation
lives in the embedding app; the two meet through a dlsym'd host bridge
(`wineios_host.h`, twin copy in `CrossoverPad/Sources/WineHost/`):

- **`dlls/wineios.drv/`** (new): `dllmain.c` (PE half; `DllMain` calls the
  unixlib init and starts the input-pump guest thread),
  `iosdrv_main.c` (vtable registration via `__wine_set_user_driver`, host
  bridge resolution with `dlsym(RTLD_DEFAULT, "wineios_host_get")`, input
  injection via `NtUserSendHardwareInput` on a Wine thread), `display.c`
  (`pUpdateDisplayDevices`: one monitor at the host-reported — or
  `WINEIOS_SCREEN` — pixel size), `window.c` (per-window data;
  `window_surface` whose `flush()` copies the dirty rect into a stable
  BGRA buffer the app presenter reads under a lock). Everything not
  implemented falls back to win32u's nulldrv.
- **`configure.ac`**: `enable_wineios_drv=yes` inside the existing iOS
  branch (0006), default `no` elsewhere (`--enable-wineios-drv` opts in on
  any host — the driver is platform-independent C, which is how it is
  compile-tested and smoke-tested on Linux).
- **`programs/explorer/desktop.c`**: default driver list gains a trailing
  `,ios`, so an iOS runtime (where mac/x11/wayland don't exist) selects the
  driver with zero registry configuration; desktop platforms never reach it.

Headless mode is a feature, not a fallback: with no host bridge present the
driver still runs, and `WINEIOS_SURFACE_DUMP=<dir>` makes every flush write
the surface as a BMP plus a `wineios: FLUSH ... checksum=` marker — that is
the CI/Linux validation path (`native/wineforge/iosdrv_gui_smoke.sh`, and
the GUI smoke in `wine-ios-sim-run.yml`).

Threading contract (see the header comments in `wineios_host.h`): host
callbacks run on Wine threads and must not block on the app main thread;
the driver never calls a host callback while holding a surface pixel lock;
input is enqueued by UIKit handlers and injected by a dedicated Wine guest
thread (win32u must never be entered from the app main thread).

**Known pre-existing issue (not 0007):** on this stack a `CW_USEDEFAULT`
top-level window gets a bogus ~6750424×6750327 rect (its surface is
correctly clipped to the screen, but nonclient painting inherits the huge
geometry). Reproduces byte-for-byte with Wine's built-in `null` driver on
the same build (`Graphics=null`), display metrics/DPI/work-area all probe
correct (1024×768@96dpi), so it sits in win32u's default-placement path
interacting with the fork — filed here so the next debugging session
starts at `fix_cs_coordinates`/`map_dpi_create_struct`, not at the driver.

### Regenerating a patch — read this first

Patches are sequential diffs against the *evolving* tree, so a patch must be
generated with its predecessors applied. Two traps, both hit in practice:

1. **0002 and 0003 both touch `dlls/ntdll/unix/loader.c`.** A naive
   `git diff -- dlls/ntdll/unix/` folds 0002's hunk into 0003, and the series
   then fails to apply. Strip 0002 first:

   ```sh
   W=$PWD/../wine; P=$PWD
   git -C "$W" apply -R "$P/0002-no-fork-embedded-server.patch"
   git -C "$W" diff -- dlls/ntdll/unix/ > "$P/0003-inproc-spawn-and-peb.patch"
   git -C "$W" apply "$P/0002-no-fork-embedded-server.patch"
   ```

2. **`git -C <dir>` resolves relative paths inside `<dir>`.** Always pass the
   patch as an absolute path, or `git -C wine apply patches/foo.patch` looks
   for `wine/patches/foo.patch`, fails, and — if you ignore the error — the
   next redirect can overwrite the patch you were trying to regenerate.

Always finish with the round-trip check:
`./apply.sh --reverse && ./apply.sh --check && ./apply.sh`.

### 0005-forkless-build.patch

`WINE_FORKLESS` compile mode: the entire runtime fork/exec surface, compiled
out. Guards `spawn_process_fork`, `exec_wineloader`, `__wine_unix_spawnvp`,
`fork_and_exec`, `start_server` (client side) and the server's daemonize path
behind `#ifdef WINE_FORKLESS`; the spawn dispatch makes the in-process
backend the only backend (no `WINE_INPROC_SPAWN`/`WINE_INPROC_RUN` opt-in),
and the two model preconditions become hard requirements: a live ntdll PE
side and a prepared prefix (`native/ios/make-prefix-bundle.sh`). This is the
compile mode an iOS build uses — fork does not exist there — and it is
testable on Linux: `native/wineforge/forkless_gate.sh` runs notepad on a warm
prefix against an externally-started server with exactly one host process.
Default builds (flag unset) are byte-for-byte unchanged.

NB when regenerating: 0005 overlaps files touched by 0001 (server/request.c),
0002 (unix/loader.c) and 0003 (unix/process.c), so it must be generated as a
sequential diff on top of 0001–0004 (temp-commit the base, diff against the
full tree). `apply.sh --check` validates sequentially by applying and
unwinding.

### 0006-ios-src.patch

The iOS source port of the Wine unix side: the changes needed for
`dlls/ntdll/ntdll.so` and `wineserver` to cross-compile against the iPhoneOS
SDK (arm64), verified in CI by `.github/workflows/wine-ios-probe.yml`. Each
hunk narrows a macOS-only assumption to `TARGET_OS_OSX` and gives iOS a
working path:

- **cdrom.c** — optical-drive ioctls use macOS-only `<sys/disk.h>`/IOKit
  storage headers; iOS has no optical hardware, so take the generic paths.
- **file.c** — `getattrlist`/`FIODTYPE` exist on iOS but their constants
  (`VLNK`, `D_TAPE/D_DISK/D_TTY`) are not in the SDK; provide the stable xnu
  ABI values.
- **loader.c** — Carbon Multiprocessing Services and the distributed
  notification center are macOS-only warmups; skip on iOS.
- **system.c** — SMBIOS-from-IOKit and the IOPowerSources battery API are
  macOS-only; iOS takes the generic SMBIOS/battery fallbacks.
- **virtual.c** — the iOS SDK hard-errors on `<mach/mach_vm.h>`; provide
  `mach_vm_map/deallocate/region` as thin wrappers over the legacy `vm_*`
  calls (same operations, same-width LP64 types).
- **server/mach.c + server/process.h** — no Mach debugger API and no usable
  ptrace on iOS; a new `USE_INPROC_TRACE` backend does cross-process memory
  as a direct copy (all pseudo-processes share the server's address space)
  and stubs the suspend/context plumbing.

Compiled ONLY when targeting iOS (`TARGET_OS_IPHONE`); the macOS and Linux
builds are unaffected (the default Linux `ntdll.so`/`wineserver` build is
verified byte-clean with the series applied).
