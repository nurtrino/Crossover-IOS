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

### 002 — real Wine client on the in-thread server (PASSING)

`real_client_test` runs the **genuine** `wine` loader against the in-thread
wineserver. Wine's `server_connect()` connects to the existing master socket
in the prefix instead of forking its own server, so the real ntdll client
drives the full protocol — version handshake, `init_first_thread`, the
request/reply fd dance — against wineserver-as-a-thread. The client runs,
exits, and the server then **self-shuts-down on its own** once its last user
process is gone (no signal from the harness) — proof the client registered
as a genuine user process. Deterministic across repeated runs.

The harness still `fork/exec`s the client (that is the *test* launching a
client, not Wine spawning a server); eliminating the client's own process is
the separate client-side workstream below. What 002 nails down is the
**server side**: the forked `wineserver` process is gone.

### 004 — concurrent real clients, one server (PASSING)

`real_multi_test` launches N real Wine clients at once against one in-thread
server and measures a hard, server-side number: `init_first_thread`
handshakes counted from the server's own `-d1` trace. One server multiplexes
the whole concurrent process tree (the N clients plus every subprocess
wineboot spawns) and returns to a zero user-process count (clean
self-shutdown). Client exit codes are reported but secondary — they are
limited by this minimal, `--without-mingw` prefix, not by the server.

Characterization (this container, 4 cores, TCG-less native x86-64 build):

| clients | init_first_thread handshakes | verdict | wall |
|--------:|-----------------------------:|:-------:|-----:|
|       2 |                           42 |  PASS   | 11 s |
|       4 |                           50 |  PASS   | 12 s |
|       8 |                           66 |  PASS   | 13 s |

Server multiplexing is not the bottleneck: 4× the clients adds ~2 s of wall
time, dominated by per-client prefix work, not server contention.

### Server-side of M1 is proven

Experiments 002 + 004 establish that real Wine processes — one or many,
concurrently — run against `wineserver` running purely as a **thread** in the
host process, with no forked server anywhere. That is the server half of
milestone M1.

### 005 — in-process child attach, no fork (PASSING)

Patch 0003d: `spawn_process_inproc` replaces fork+exec for `CreateProcess`
under `WINE_INPROC_SPAWN`. The child becomes a thread group in the parent's
host process: fresh PEB (template-copied) + TEB, its own server socket in the
PEB-keyed registry, and the first-thread handshake (`init_first_thread` +
`init_process_done`) run on the handed socket by `server_init_process_inproc`.
`spawn_seam_test.sh` asserts it with server-side evidence: wineboot's two
child processes attach in-process and the server's `-d1` trace counts their
handshakes (3 = primary + 2 children), deterministic across runs; the fork
backend stays regression-free. The child does not yet run its PE image —
that is 0003e, the end-to-end finish of M1's client half.

### Next: client-side process elimination

What remains for full M1 (`wine notepad.exe` with *all* fork/exec compiled
out) is the client half: load ntdll + the PE loader **in-process** and
replace Wine's `CreateProcess`→`exec` (`dlls/ntdll/unix/process.c`,
`loader.c`) with a `pproc_spawn`-style in-address-space launch. Scoped as
patch series 0002 in `docs/NATIVE_PORT.md`. This is the large, invasive part;
it is developed host-first on Linux before any iOS cross-compile.
