# Design: patch series 0003 — CreateProcess without exec

The last piece of milestone M1. Goal: a Windows `CreateProcess` launches the
child **in the same host process** — a new thread group loading the child PE
into the shared address space — instead of `fork()`+`exec()`ing a new wine
loader. Developed host-first on Linux, same as 0001/0002.

## The seam is narrow (the key finding)

Tracing `NtCreateUserProcess` in `dlls/ntdll/unix/process.c`, the child-launch
sequence is:

```
1. socketpair(socketfd)                      // parent<->child server link
2. wine_server_send_fd(socketfd[1])          // hand one end to wineserver
3. SERVER_START_REQ(new_process){socket_fd}  // server makes the process obj
4. SERVER_START_REQ(new_thread)              // server makes the init thread
5. spawn_process(params, socketfd[0], ...)   // fork+exec loader w/ socketfd[0]
      └─ exec_wineloader(argv, socketfd[0])   // child runs init on socketfd[0]
```

Steps 1–4 are **pure wineserver traffic and already work in-process today.**
Experiment 004 proves it: one in-thread server served 50 `init_first_thread`
handshakes for 4 `wineboot` runs, and the surplus over the 4 top-level
clients are child processes wineboot spawned through exactly this path —
connecting to the in-thread server over the inherited `socketfd`, not the
master socket. SCM_RIGHTS fd passing to a peer that happens to share our
address space is fine (and ultimately reducible to a plain fd copy).

So **only step 5 changes.** `spawn_process()` is the entire fork boundary for
Windows process creation. Everything else in `NtCreateUserProcess` is reused
verbatim.

## What the in-process `spawn_process` must do

Replace fork+exec with, on a fresh **pproc** thread group:

1. **Thread + process context.** Create the child's initial thread via the
   pproc substrate (`native/pseudoproc`), carrying a new per-pseudo-process
   context. `socketfd[0]` becomes the child's server connection — set as if
   `WINESERVERSOCKET` were inherited, but passed directly, no env/exec.
2. **Fresh TEB/PEB.** Allocate a new PEB for the child pseudo-process and a
   TEB for its main thread. On ARM64 the TEB is register-reached
   (TPIDRRO_EL0/x18); on the Linux host it is the usual thread register. The
   PEB moves from a fixed address to per-pseudo-process, reached through the
   thread's context (see NATIVE_PORT Blocker 1, step 3).
3. **Load the child PE into the shared address space.** Reuse ntdll's own
   mapping/relocation path (`virtual_map_image` / loader). Because the parent
   is already resident, base-address collisions are resolved by PE relocation
   (ASLR-era binaries relocate cleanly). This is the substantive work.
4. **Run ntdll init on `socketfd[0]`.** Drive the same `server_init_process`
   / `init_first_thread` path experiment 002 exercised, but with the
   pre-connected socket instead of `server_connect()`. The server already has
   the process object from step 3 of the parent flow, so the handshake
   completes and the child is a first-class process on the shared server.
5. **Entry + lifetime.** Jump to the PE entry point on the child thread; on
   exit, run the pproc/`NtTerminateProcess` unwind and let the server reap the
   process object (embeddable-shutdown machinery from 0001 generalizes here).

## Staging (each stage stays testable host-first)

- **0003a — spawn seam refactor (no behavior change).** Route `spawn_process`
  through an explicit backend selector; default backend is the current
  fork+exec, so `real_client_test`/`real_multi_test` stay green. Adds the
  in-process backend as a clearly-stubbed `STATUS_NOT_IMPLEMENTED`. Testable:
  zero regression with the flag off.
- **0003b — PEB/TEB per pseudo-process.** Stand up child TEB/PEB allocation
  and the context plumbing; unit-test identity/isolation like the pproc suite.
- **0003c — in-process image mapping.** Map+relocate a child PE beside the
  parent; test with a trivial PE that just returns an exit code.
- **0003d — in-process init handshake.** Run `init_first_thread` on the
  handed socket; assert the server counts the in-process child (same
  `-d1`-trace method as experiment 004).
- **0003e — end to end.** `CreateProcess` of a real child (the installer→app
  pattern) fully in one host process. **M1 complete.**

## PEB virtualization (stage 0003b) — measured scope

The single biggest sub-problem. `peb` is one process-global
(`dlls/ntdll/unix/env.c: PEB *peb`), and every Windows process assumes one
PEB (process parameters, environment, and the loaded-module list in
`peb->Ldr`). Giving an in-process child its own identity means routing "my
process's PEB" through the current thread instead of the global.

The mechanism is cheap and already correct-by-construction: `init_teb()` sets
`teb->Peb = peb` for every thread, so `current_peb()` (added in
`unix_private.h`) — `NtCurrentTeb()->Peb` — equals the global for the primary
process today and becomes per-pseudo-process the moment a child's threads get
a TEB with a different `Peb`. Converting a call site is therefore a
zero-behavior-change step now and the hook for multiple processes later.

Measured coupling on the Unix side (wine-11.0): **81 direct `peb` uses across
10 files.** Conversion ledger:

| file | uses | when it runs | action |
|---|---:|---|---|
| `process.c` | 6 | post-init (spawn/query) | **converted (0003b)** |
| `env.c` | 34 | mixed; `virtual_alloc_first_teb` + params | split: keep pre-init, convert the rest |
| `system.c` | 12 | post-init (all after first TEB) | **converted (0003b)** |
| `virtual.c` | 10 | **includes pre-TEB init** | keep the early ones global; convert the rest |
| `server.c` | 5 | init-path; 4 after first TEB | **4 converted (0003b)**; wow64 layout kept |
| `thread.c` | 4 | post-init (runtime queries) | **converted (0003b)** |
| `signal_*.c`, `loader.c`, `debug.c` | ~10 | mixed | case-by-case |

Rule: any use reachable before `virtual_alloc_first_teb()` must stay on the
global `peb` (no TEB yet). Everything after can move to `current_peb()`. The
PE side (`dlls/ntdll/*.c`) and higher DLLs already read the PEB via
`NtCurrentTeb()->Peb`, so they mostly come along for free — the Unix side is
the concentrated work. This is mechanical but must be done file-by-file with
the wineforge suite green after each, exactly as 0003b's first file was.

## The shared-ntdll insight — what actually makes M1 finishable

Fork+exec gives every Windows process a *fresh* ntdll and a full init. The
in-process child does **not**: it shares the parent's already-initialized
ntdll in the same address space. So the child never re-runs ntdll init — it
runs a much lighter **"attach a new Windows process to the running ntdll"**
path. That collapses "make ntdll init re-entrant" (open-ended) into a small,
enumerable virtualization set (bounded).

What is already per-process or per-thread, and comes along for free:

- **Module list** — reached as `NtCurrentTeb()->Peb->LdrData` (see
  `dlls/ntdll/loader.c`). Once the PEB is per-pseudo-process, each process
  gets its own module list automatically. The whole PE-side loader already
  routes through `NtCurrentTeb()->Peb`, so it needs no changes.
- **Per-thread server pipes** — `request_fd`/`reply_fd`/`wait_fd` live in
  `ntdll_thread_data` (TEB-based), so the child's main thread gets its own by
  construction (experiment 002 exercised exactly this handshake).
- **Loader/debug one-shot guards** (`loadorder.c`, `debug.c` `init_done`,
  `start_server`'s `started`) — these gate *config*, not per-process instance
  state, and the child that uses a handed socket never calls `start_server`.

What genuinely must be virtualized (the entire core remaining work):

1. **`peb`** — in progress (26/81 unix-side uses converted to `current_peb()`;
   remainder in `env.c`/`virtual.c`, plus per-pseudo-process PEB allocation).
2. **`fd_socket`** (`server.c:104`, the SCM_RIGHTS fd-exchange socket, one per
   process) — must become per-pseudo-process so the child talks to the server
   over its own `socketfd[0]` without clobbering the parent's. `server_pid`
   rides along. (`server_block_set` is a signal mask — shared is fine.)

The **attach sequence** `spawn_process_inproc` then performs, on a new pproc
thread with a fresh TEB whose `Peb` points at a freshly-allocated child PEB:
map+relocate the child PE (mechanic proven by `peload_test`), point the
child's `fd_socket` at `socketfd[0]`, run the `init_first_thread` handshake
(proven against the in-thread server by experiment 002), then jump to the PE
entry. Every one of those sub-mechanics is individually proven; the remaining
work is the two globals above plus wiring the sequence together.

## Risks / open questions

- **Per-process DLL globals.** DLLs written assuming one process per address
  space keep mutable globals that now alias across pseudo-processes. Wine's
  existing per-process data mechanisms cover much of ntdll/kernelbase; the
  long tail is third-party DLLs. Same problem class as `.shared` PE sections,
  inverted — enumerate and instance incrementally.
- **Thread-local vs process-local.** Audit which ntdll statics are truly
  process-scoped and must move into the PEB-side context.
- **16 KB pages (iOS).** Not a Linux-host concern, but image mapping code
  written now must not bake in 4 KB granularity (NATIVE_PORT Blocker 2).
- **Signals.** One process-global signal disposition already noted in
  wineforge finding #5; the child's ntdll init must not fight the parent's.

## Why this is a separate, funded effort

0003c–d are the genuine Wine-internals research: in-address-space PE loading
and a second `init_first_thread` on a shared server. There is no small
green-testable slice of *those* — 0003a/b scaffold cleanly, but the loader is
indivisible. This is the multi-week centerpiece the whole port has been
walking toward, and the reason a native iOS Wine did not already exist.
