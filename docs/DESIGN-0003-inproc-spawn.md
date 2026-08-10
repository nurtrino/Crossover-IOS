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

## Attach implementation spec (0003d/e) — the final integration

Both process-globals are now virtualized (tested, zero regression):

- **`peb`** → `current_peb()` on the hot path; `init_teb` sets `teb->Peb`, so a
  child thread whose TEB carries a distinct PEB gets its own process identity
  and (for free) its own module list via `Peb->LdrData`.
- **`fd_socket`** → `current_server_fd()`, a PEB-keyed registry; the child's
  socket is registered under the child PEB, so its server traffic never
  touches the parent's connection.

With those in place, `spawn_process_inproc` performs the attach on a fresh
pthread (a pproc thread group), replacing fork+exec. The precise sequence,
each step mapped to existing Wine machinery:

1. **Child TEB+PEB.** Allocate a PEB (template-copy the parent's, then reset
   per-process fields) and call `virtual_alloc_teb(&teb)`; set `teb->Peb =
   child_peb`. Install the TEB on the new thread (`signal_init_thread`/arch
   thread-register set), and `signal_alloc_thread` for its syscall frame +
   kernel stack.
2. **Server connection.** `set_process_server_fd(child_peb, socketfd[0])` so
   `current_server_fd()` resolves to the handed socket for this process.
3. **First-thread handshake.** Run the `server_init_process` tail on this
   thread: `wine_server_receive_fd` for the request fd, version check,
   `init_first_thread` request. The server already created the process object
   (parent's `new_process`), so the child becomes a first-class process on the
   shared in-thread server — the crux, and the exact exchange experiment 002
   proved works over a handed socket.
4. **Image.** Map + relocate the child PE (mechanic proven by `peload_test`;
   in ntdll this is `virtual_map_image` / the builtin loader) into the shared
   address space, resolving base collisions by relocation.
5. **Run.** Enter via `RtlUserThreadStart` → PE entry on the child thread. On
   exit, `NtTerminateProcess` unwinds the thread group; the server reaps the
   process object (the 0001 embeddable-shutdown machinery generalizes).

This is the one remaining coherent chunk. It is deliberately not landed
piecemeal: steps 1+3 only become testable together (a child either registers
and runs or crashes), so it must arrive as a single tested unit rather than
untested half-integration — the discipline that has kept every commit green.

### Refined scope after following the attach to ground truth

`peb` and `fd_socket` were the cleanly-bounded globals (natural per-process
homes: `TEB->Peb`, a PEB-keyed table). Tracing `server_init_process_done`
and `signal_start_thread` to completion shows the attach also pulls in:

- **`main_image_info`** (`loader.c:194`) — the main EXE's
  `SECTION_IMAGE_INFORMATION`, **32 uses across 7 files** including
  `signal_arm64.c` and `virtual.c`. `server_init_process_done` reads its
  `TransferAddress` for the entry jump, so a child needs its own. This is
  woven through core/arch paths — not a clean `current_x()` swap like the
  first two.
- **Arch thread bring-up** — `signal_start_thread` / `signal_alloc_thread`
  set up the syscall frame, kernel stack, and thread register per arch
  (`signal_x86_64.c`, `signal_arm64.c`). The child's process-thread must run
  this correctly with its own TEB.
- **A stripped `server_init_process`** — the child must run only the
  per-process tail (receive request fd, version check, `init_first_thread`,
  `set_thread_id`), *not* the one-time global init (`init_environment`,
  `init_cpu_info`, `native_machine`/`supported_machines` setup) already done
  by the shared ntdll.

So the honest remaining estimate is **one more entangled global
(`main_image_info`) + arch thread bring-up + a carefully-factored
process-init tail**, landed together as a tested unit. The two clean globals
are done; this last chunk is the genuinely hard, iteration-heavy Wine-internals
work — best done where a crashing child can be debugged interactively, not
blind in a headless batch.

## Status: 0003d landed (2026-08-10) — the attach works

The registration half of the attach is implemented and green:

- `server_init_process_inproc()` (server.c) — the stripped per-process tail:
  receives the queued request fd + protocol version (the server sends them at
  `new_thread` time, `server/thread.c: create_thread`), runs
  `init_first_thread` and `init_process_done` on the child's own connection,
  replying into locals so the shared ntdll's globals are untouched. Returns
  errors instead of `fatal_error()` — a failed child attach cannot take down
  the host, and the closed socket makes the parent's `CreateProcess` fail
  cleanly instead of hanging.
- `spawn_process_inproc()` (process.c) — dups the socket (the caller closes
  its copy), template-copies the parent PEB (LdrData/ProcessParameters reset
  for 0003e), registers the socket under the child PEB, allocates TEB + stack
  via the existing `virtual_alloc_teb`/`init_thread_stack`, points `teb->Peb`
  at the child PEB, and launches the bootstrap on the TEB's kernel stack —
  the same recipe as `NtCreateThreadEx`.
- The fd registry gained unregister (`fd == -1`) and hole-tolerant lookup so
  children can come and go.

Evidence (`spawn_seam_test.sh`, deterministic across runs): under
`WINE_INPROC_SPAWN=1` wineboot's child `CreateProcess` calls succeed with the
children attached from threads of the same host process, and the server's own
`-d1` trace counts their `init_first_thread` handshakes (2 children + the
primary = 3). The fork backend is regression-free with the flag off.

What 0003d deliberately does not do: run the child's PE. The child registers,
releases the parent, and detaches cleanly (fd close = the same death signal a
real child's exit gives the server). The child's TEB is leaked by design — the
kernel stack it contains is the live pthread stack; reclaiming it belongs to
the NtTerminateProcess unwind workstream.

Found while implementing, for 0003e's ledger: the server's terminate path
(`server/process.c:632`) signals `process->unix_pid` with `kill()` — for an
in-process child that pid is the whole host. Voluntary child exit never hits
it, but `NtTerminateProcess` of an in-process child from outside will need a
server-side notion of in-process processes (or a sentinel unix_pid).

## Status: 0003e landed (2026-08-10) — the child owns its startup and image

The third entangled global is virtualized and the child now stands up its own
Windows-process state:

- **`main_image_info`** → `current_image_info()` (`loader.c`), a PEB-keyed
  registry in the same shape as `current_server_fd()`. Unlike `current_peb()`
  it guards against a missing TEB, because early `virtual.c` paths reach it
  before `virtual_alloc_first_teb()`. Converted on the child's path:
  `load_main_exe`'s two writes, `init_peb`'s reads, and the thread-stack
  defaults. Left global (documented): the one-time `load_wow64_ntdll`, the
  arm64ec/`signal_arm64` machine lookups, and the wow64 limit calculation —
  none run for an in-process child.
- **`init_peb`** now writes `current_peb()`, so the child fills in *its* PEB.
- **`build_startup_info( info_size, inproc )`** — `init_startup_info()` is a
  thin wrapper over it, and `init_startup_info_inproc()` is the child's entry.
  The child skips `rebuild_argv()`/`main_wargv` (host-process-global; its
  command line lives in its own params) and returns errors instead of calling
  `NtTerminateProcess`, so a failed child never kills the host.

**The ordering bug this uncovered** (the one real defect found): the child
originally sent `init_process_done` inside its attach, *before* fetching
startup info. `set_process_startup_state()` releases `process->startup_info`,
so the later `get_startup_info` returned an empty reply — and
`env_pos = env_size - 1` underflowed to `SIZE_MAX`, segfaulting the child.
Fixed by splitting `server_init_process_done_inproc()` out and calling it
after the image is mapped, matching the normal loader's order
(`server_init_process` → `init_startup_info` → `server_init_process_done`).
A guard now turns an empty startup-info reply into a clean child-only failure
instead of a wild pointer walk.

Evidence (`spawn_seam_test.sh`, deterministic across runs): both of wineboot's
children attach in-process **and** map their own main image —
`L"C:\windows\system32\wineboot.exe"` with real base and entry addresses — and
the server's `-d1` trace still counts 3 handshakes. Fork backend unchanged.

## Status: 0003f landed (2026-08-10) — the child runs

An in-process child now **executes its own program** and exits with its own
exit code. The whole chain runs with no fork/exec in the child's creation:
`CreateProcess` → thread group → child PEB/TEB + own socket → own startup info
→ own mapped+relocated EXE → **PE entry executed** → real exit code.

Two changes made it work:

- **`SkipLoaderInit`.** `loader_init`'s `imports_fixup_done` and `attach_done`
  are process-globals already set by the parent, so a second process falls
  into the thread-attach branch and dies on a NULL modref for its own
  (unregistered) image. Wine's own `SkipLoaderInit` (used by
  `THREAD_CREATE_FLAGS_SKIP_LOADER_INIT`) makes `loader_init` return
  immediately, so the child reaches its PE entry through the normal
  `RtlUserThreadStart` path.
- **`LdrData` is inherited, not blanked.** This is the shared-ntdll model
  taken to its conclusion: a child that loads nothing of its own starts from
  the parent's module list, which the entry path walks. Blanking it faulted at
  `[NULL+0x38]`.

**The gate.** `image_needs_loader()` checks the child image's import directory.
Import-free images are entered and run correctly; images that import DLLs are
**refused and logged**, not entered — before this gate a DLL-importing child
faulted and took the whole host down with SIGSEGV. `WINE_INPROC_RUN` is
therefore safe to enable generally: worst case a child does not run.

**Evidence** (`inproc_run_test.sh`, deterministic over repeated runs): for exit
codes 7/42/123 the child maps its own `exitN.exe`, enters its PE, and the
**server's own `-d1` trace** records `*killed* exit_code=N` for the child
process object — the same authoritative method as experiment 004. The fork
backend is the control and returns the same codes end to end.

Server-side evidence is used deliberately rather than the parent's exit code.
Both were measured: the child's own exit code is correct in **every** run,
while propagation through a parent is currently entangled with the boundary
below.

### Known gap: parents that depend on a refused child

If a parent's own work depends on a DLL-importing child (the common case:
Wine spawning `wineboot` for a prefix update), that child is refused, the work
silently does not happen, and the *parent* can fail even though every child
that did run ran correctly. This is a consequence of the loader boundary, not
a defect in the child-run path, and it disappears when DLL-importing children
can run. Measured: with a settled prefix and no refused children, propagation
through `cmd /c exitN.exe` returns N correctly.

### What is genuinely left for M1

## Status: 0003g landed (2026-08-10) — DLL-importing children run

The last named blocker is done: ntdll's **PE-side** loader state is now
per-pseudo-process, so a second Windows process in one address space builds its
own module list, loads its own imports, and runs process attach — exactly as a
forked child would.

**The mechanism.** All of the loader's process-globals move into one
`struct ldr_proc_state`: the `PEB_LDR_DATA` itself, the module hash table, the
base-address index tree, TLS bitmaps + directories, the resolved system-DLL
nodes, the modref caches, the DLL search path, and the one-shot gates
(`imports_fixup_done`, `attach_done`, `process_detaching`). The block is reached
in **O(1) with no registry and no lock**: `loader_init` points `peb->LdrData` at
the block's first field, so `CONTAINING_RECORD` recovers it from the PEB. The
primary process keeps the static instance, so its behaviour is unchanged; the
call sites are unchanged too, because the former global names are `#define`d to
the block's fields (field names deliberately differ from the macro names so
member access never re-expands).

`loader_init` gains one step: a pseudo-process arriving with
`peb->LdrData == NULL` gets its own block — the first process ever takes the
static (no heap exists yet), later ones allocate from the heap they inherited
from their parent, before creating their own.

Deliberately still shared: `loader_section` and `peb_lock` (one loader lock
across pseudo-processes is conservative and deadlock-free) and the known-DLL
directory handle.

With this in place `SkipLoaderInit` and the import gate from 0003f are gone —
children enter their PE through the normal `LdrInitializeThunk` → `loader_init`
path, import-free or not.

**Evidence** (`inproc_run_test.sh`, deterministic): import-free children still
map, run and exit with codes 7/42/123 confirmed from the server's own `-d1`
trace; **nested `cmd.exe`** — a heavy DLL user — runs as an in-process child and
returns exit code 7, matching the fork backend; `attrib.exe` output is identical
to the fork backend. `hostname.exe` runs its real code and prints, but one API
returns error 6 (see below). Whole wineforge suite green, fork backend
regression-free.

### Bootstrap boundary (fixed): the model needs a live PE side and a prefix

Cold-prefix creation used to crash the host. Root cause, found by printing the
init block from the child: Wine creates a missing prefix from **inside early
init**, so `wineboot` is spawned before ntdll's PE side has published
`pRtlUserThreadStart` / `pLdrInitializeThunk`. Entering a child then jumps to a
NULL entry — a raw SIGSEGV with an empty stack, which is exactly what was seen.
A second phase fails later for the same class of reason: a child's loader cannot
resolve system DLLs from a prefix that does not exist yet.

`spawn_process` now treats both as what they are — the in-process backend's real
precondition — and falls back to the fork backend when either holds
(`!pRtlUserThreadStart || !pLdrInitializeThunk`, or `is_prefix_bootstrap`).
With that, **a cold prefix builds end to end with the in-process flags on**
(asserted by `inproc_run_test.sh`), and every child after bootstrap runs
in-process. On iOS, where there is no fork, bootstrap must ship a prepared
prefix or defer until the PE side is up — this is now a stated architectural
requirement rather than a crash.

### The open risk, measured: DLL images are shared between pseudo-processes

Two in-process Windows processes load **one** `kernel32` at **one** base — the
child reuses the parent's mapping instead of getting a private copy-on-write
view. Measured directly (`+loaddll`: 2 pseudo-processes, 1 distinct kernel32
mapping) and printed by `inproc_run_test.sh` every run, so a fix will be
visible. An earlier reading of 4 distinct bases was wrong: those were *forked*
processes in separate address spaces, not in-process children.

Consequence: DLL globals alias across pseudo-processes. That is very likely why
`hostname.exe` runs its real code, prints, and yet gets ERROR_INVALID_HANDLE
from its computer-name query — a handle cached in a DLL global by one process is
meaningless in another's handle table. Programs that do not lean on cached
per-process DLL state (nested `cmd.exe`, `attrib.exe`, import-free PEs) are
unaffected and match the fork backend exactly.

The principled fix is a private image mapping per pseudo-process for each DLL,
relocated on collision — the mechanic `peload_test` already proves. It is the
last real research item in Blocker 1.

### What still does not work

- **DLL globals are shared** (above) — the open correctness risk, and the known
  cause class for stray `ERROR_INVALID_HANDLE` in children (`hostname.exe`).
- **A crashing child still takes the host down.** There is no per-pseudo-process
  fault containment, so the in-process backend stays opt-in behind
  `WINE_INPROC_SPAWN` / `WINE_INPROC_RUN`.
- **M1's literal bar is not met.** `wine notepad.exe` needs a display driver this
  `--without-x` build does not have, and "all fork/exec compiled out" is not
  true while bootstrap deliberately falls back to fork.

What *is* proven is the mechanism M1 rests on: Windows processes as thread
groups in one address space, running real DLL-importing programs against a
shared in-thread wineserver, with no fork/exec in the creation of any child
after bootstrap.

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
