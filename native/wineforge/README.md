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

### Next experiments

- 002: teach the pproc substrate to be the client — speak the real request
  protocol (version handshake, `init_first_thread`) from a pseudo-process
  so the server counts a *user process* and the "last process exited"
  shutdown timer fires naturally.
- 003: first source patch series under `native/patches/`: replace server
  `exit()`/`fatal_error()` with an embeddable shutdown callback; make
  `wineserver_main` re-entrant (static state audit).
- 004: two pseudo-process clients sharing one server thread — cross-process
  handle duplication through the real server.
