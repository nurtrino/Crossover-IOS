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
