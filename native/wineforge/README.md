# wineforge — the Wine fork workbench (host-first)

Surgery on real Wine, developed and tested on Linux/macOS before any iOS
bring-up. Wine is pinned as a submodule at `native/wine` (wine-11.0); builds
happen out-of-tree in `native/wine-build` (gitignored).

## Setup

```sh
git submodule update --init --depth 1 native/wine
sudo apt install flex bison          # or brew equivalents
mkdir -p native/wine-build && cd native/wine-build
../wine/configure --enable-win64 --without-x --without-freetype \
                  --disable-tests --without-mingw
make -j"$(nproc)" server/wineserver
cd ../wineforge && make test
```

## Experiment log

### 001 — wineserver as a thread (PASSING)

`embed_test.c` links the **unmodified** wine-11.0 `server/*.o` objects into a
host binary and runs the complete wineserver lifecycle on a pthread:
startup, master-socket creation, accepting a client connection (wineserver
creates a real process object for it), EOF teardown, and host-initiated
shutdown — all in one process. Two link-time transforms, zero source patches:

- `objcopy --redefine-sym main=wineserver_main` — wineserver becomes a
  callable library entry point.
- `-Wl,--wrap=exit` — wineserver's shutdown path ends in `exit(0)`
  (`close_socket_timeout()` in `server/request.c`); the wrap turns an exit
  on the server thread into `pthread_exit`, i.e. *process death becomes
  thread death*. This is the exact semantic the fork will implement in
  source once patches start.

Findings that shape the fork (kept current as experiments accumulate):

1. **The wire model needs no changes.** A raw AF_UNIX client connects and is
   accepted normally; wineserver doesn't know or care that it shares an
   address space with its peer. Blocker 1 step 2 of `docs/NATIVE_PORT.md`
   is validated with real code, not a mock.
2. **`exit()` is the process-model leak.** All server shutdown funnels
   through `exit(0)`/`fatal_error()`. The fork replaces these with a
   server-thread unwind (the wrap proves the shape works).
3. **Path derivation uses `/proc/self/exe`** (`get_nls_dir()` in
   `server/unicode.c`), not `argv[0]`, on Linux — and `_NSGetExecutablePath`
   on Apple. The embedding binary's directory must end in `/server/` with
   wine's `nls/` as a sibling (the build tree layout), or data files fail to
   load. The iOS app bundle must reproduce this relative layout.
4. **Host-initiated shutdown already exists**: the SIGINT/SIGTERM handlers
   are self-pipe based, so any thread may raise them and the server thread
   performs an orderly `shutdown_master_socket()`. This maps directly onto
   iOS app-lifecycle events.
5. **Signal handlers are process-global.** Fine for one embedded server;
   inventory needed before Wine's own loader signals join the party.

### 003 — embeddable server shutdown, source-level (PASSING)

Turns the experiment-001 linker hack into a real fork patch
(`native/patches/0001-embeddable-server-shutdown.patch`). The server gains
`wineserver_run()` (an entry point that returns instead of `exit()`ing) and
`server_exit()` (unwinds via `setjmp`/`longjmp` back to `wineserver_run`).
All runtime shutdown funnels — `close_socket_timeout`, `fatal_error`,
`sigterm_callback`, signal-init failure — route through `server_exit()`,
guarded by a `server_embedded` flag so the standalone `wineserver` binary is
byte-for-byte unchanged.

`embed_run_test` validates the real M1 shape: **one** long-lived
`wineserver_run()` instance serves multiple sequential clients, then a
host-side SIGINT drives an orderly shutdown that returns 0 with the master
socket unlinked and the host process alive.

Findings:

6. **`exit()` bypasses `atexit`.** The `longjmp` shutdown skips the
   `atexit(socket_cleanup)`, so the embedded path must unlink the master
   socket itself; `socket_cleanup()` was made idempotent to serve both paths.
7. **Server restart needs a static-state reset.** A second `wineserver_run()`
   in the same process aborts in `init_registry` (`root_key` and friends are
   process-lifetime statics). Declared a **non-goal**: the architecture runs
   one server per app session. Revisit only if a concrete need appears.

### Next experiments

- 002 (reframed): connect the **real** Wine client to the in-thread server.
  Wine's `WINESERVERSOCKET` path already lets a client use a pre-connected
  socket instead of forking its own server, and `server_connect()` will use
  an existing master socket if present — so the genuine ntdll client is a
  more authentic driver of `init_first_thread` than a hand-rolled protocol
  mock. Gated on the full Wine build (in progress) + a working prefix.
- 004: two clients exercising cross-process handle duplication through the
  one shared in-thread server.
- Baseline: `wine notepad.exe` end-to-end against the in-thread server (M1).
