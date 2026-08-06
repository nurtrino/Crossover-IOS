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

The in-process-launch groundwork (stages a + b of series 0003). Default
behavior is byte-for-byte unchanged; the new paths are opt-in / equivalent.

- **Stage a — spawn seam** (`dlls/ntdll/unix/process.c`): `spawn_process` is
  split into `spawn_process_fork` (the classic backend, unchanged) and
  `spawn_process_inproc` (the future in-address-space launch), dispatched on
  `WINE_INPROC_SPAWN`. The in-process backend is wired but returns
  `STATUS_NOT_IMPLEMENTED` until the PE loader (stage c) and child
  `init_first_thread` (stage d) land.
- **Stage b — PEB indirection** (`dlls/ntdll/unix/unix_private.h`,
  `process.c`): add `current_peb()` = `NtCurrentTeb()->Peb`, the hook for
  per-pseudo-process PEBs, and convert `process.c`'s post-init global-`peb`
  uses to it. Since `init_teb` sets `teb->Peb = peb` for the primary process,
  this is a zero-behavior-change step. The full file-by-file conversion
  ledger (81 uses / 10 files) is in `docs/DESIGN-0003-inproc-spawn.md`.

Validated by `native/wineforge/spawn_seam_test.sh` (both dispatch directions)
and the whole wineforge suite staying green after the PEB conversion.
