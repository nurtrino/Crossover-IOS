# Handoff — finishing M1 (in-process `CreateProcess`)

Written at the end of the session that landed patch-series stages **0003e–0003h**.
Branch `claude/codebase-familiarization-gnyptk`, HEAD **`7cb145d`**, tree clean,
suite green (5/5).

Read `docs/DESIGN-0003-inproc-spawn.md` for the design and the full staging
history. This file is the operational context: how to rebuild, what is proven,
what is broken, what was already tried, and the traps that cost time.

---

## 1. Where the project stands

**M1 is not complete.** Everything M1 rests on is working; one failure mode
remains.

Working and tested:

| capability | evidence |
|---|---|
| wineserver runs as a **thread**, no forked server | `real_client_test`, `real_multi_test` |
| Windows child processes are **thread groups**, no fork/exec | `spawn_seam_test.sh` (server `-d1` handshake counts) |
| a child fetches its **own** startup info and maps its **own** EXE | `spawn_seam_test.sh` |
| a child **runs** and returns a real exit code | `inproc_run_test.sh` (server trace `*killed* exit_code=N`) |
| DLL-importing children run (nested `cmd.exe`, `attrib.exe`) | `inproc_run_test.sh`, matched against the fork backend |
| **DLL globals isolated** — each pseudo-process maps its own `kernel32` | `inproc_run_test.sh` prints `N processes, N distinct kernel32 mappings` |
| cold-prefix creation works with the backend enabled | `inproc_run_test.sh` |

Broken — the single remaining M1 blocker:

> A **GUI child spawned through explorer's desktop path** dies with an access
> violation. `native/wineforge/m1_notepad_test.sh` is the gate; it FAILS and is
> deliberately not wired into `make test-real`.

Nuance that matters: notepad under the in-process backend against a **warm**
server runs and stays up, identical to the fork backend. It only dies when a
**cold** server means explorer spawns it as an in-process child.

---

## 2. The bug, as far as it was narrowed

```
0068:err:seh:NtRaiseException Unhandled exception code c0000005 (access violation)
     at 0x6fffffc445d5
```

- ntdll.dll is mapped at `0x6fffffbf0000` → fault RVA **`0x545d5`**.
- The PE ntdll's preferred ImageBase is `0x170000000`, so disassemble and look
  at **`0x1700545d5`**:

```
load_dll:
1700545d5:   8b 46 08    mov 0x8(%rsi),%eax
```

A 4-byte read at offset 8 through a bad/NULL pointer, **inside `load_dll`** —
a function patch 0003g touched when it moved the loader's process-globals into
a per-pseudo-process block.

**So the prime suspect is our own block, not something inherent to sharing
ntdll.** Most likely a field the primary process gets initialised inside
`loader_init`'s first-time branch, which a GUI child reaches before or without
initialisation.

Reproduce the disassembly:

```sh
cd native/wine-build
x86_64-w64-mingw32-objdump -d dlls/ntdll/x86_64-windows/ntdll.dll > /tmp/nt.asm
grep -n "1700545d5:" /tmp/nt.asm      # and read the enclosing <function>:
```

### Already tried and ELIMINATED

- **Initialising `s_hash_table`'s list heads at block-allocation time** instead
  of partway through `loader_init`. Built, ran the gate: **did not fix it**.
  Reverted; the tree matches the committed patch. Do not redo this.

### Remaining candidates (unverified)

`cached_modref`, `node_ntdll` / `node_kernel32`, `tls_dirs`, `s_ldr` list heads,
`default_load_path`. The mechanical next step: build ntdll with symbols, break
on `load_dll` in the child, and see which per-process field is NULL when `%rsi`
is bad.

Note the host **survives** this now — SEH catches it (run exits 5) rather than
the host-killing SIGSEGV seen before PE DLLs.

---

## 3. Rebuilding from scratch

```sh
git submodule update --init --depth 1 native/wine
sudo apt install flex bison gcc-mingw-w64-x86-64 xvfb   # plus libx11/freetype dev
./native/patches/apply.sh
mkdir -p native/wine-build && cd native/wine-build
../wine/configure --enable-win64 --disable-tests
make -j"$(nproc)"
cd ../wineforge && make all && make test && make test-real
```

**Two configure knobs are load-bearing — do not re-add the old flags:**

- **Never `--without-mingw`.** With mingw, DLLs build as real PE files under
  `dlls/<name>/x86_64-windows/*.dll` (602 of them). PE images get a private
  copy-on-write view per Windows process, which is what gives DLL-global
  isolation. Without it, DLLs are host `.so` files that `dlopen` deduplicates,
  two pseudo-processes share one `kernel32`, and you get aliased globals —
  that was the cause of `hostname.exe`'s `ERROR_INVALID_HANDLE`.
  Verify with `find dlls -name '*.dll' | wc -l` (expect ~602, not 0).
- **Never `--without-x`.** Otherwise `winex11.drv` is missing, no GUI app runs
  at all, and the M1 gate cannot even be attempted.

The minimal `--without-x --without-freetype --without-mingw` config is still
fine for the server-side experiments 001–004 only.

---

## 4. Runtime flags

| variable | meaning |
|---|---|
| `WINE_INPROC_SPAWN=1` | `CreateProcess` uses the in-process backend instead of fork/exec |
| `WINE_INPROC_RUN=1` | the in-process child actually enters its PE and runs |
| `WINE_EMBEDDED_SERVER=1` | (patch 0002) client refuses to fork a wineserver |

Both in-process flags are **opt-in**; default Wine behaviour is byte-for-byte
unchanged, which every test asserts via the fork backend as a control.

---

## 5. Code map (patch 0003)

Unix side, `dlls/ntdll/unix/`:

- **`process.c`** — `spawn_process` (backend dispatch + bootstrap guard),
  `spawn_process_fork` (unchanged classic path), `spawn_process_inproc`
  (allocates child PEB + image-info block, dups the socket, creates the TEB and
  bootstrap thread), `inproc_process_thread` (the child's entry sequence).
- **`server.c`** — `set_process_server_fd` / `current_server_fd` (PEB-keyed
  socket registry), `server_init_process_inproc` (per-process first-thread
  tail), `server_init_process_done_inproc` (split out — see §7).
- **`env.c`** — `build_startup_info(info_size, inproc)` backing both
  `init_startup_info()` and `init_startup_info_inproc()`; `init_peb` writes
  `current_peb()`.
- **`loader.c`** — `set_process_image_info` / `current_image_info`
  (PEB-keyed main-image-info registry).
- **`unix_private.h`** — `current_peb()` and the declarations above.
- `virtual.c`, `system.c`, `thread.c` — call-site conversions to `current_*()`.

PE side, `dlls/ntdll/loader.c` (patch 0003g):

- `struct ldr_proc_state` — per-pseudo-process loader state: `s_ldr`
  (`PEB_LDR_DATA`, **must stay first**), `s_hash_table`,
  `s_base_address_index_tree`, TLS bitmaps/dirs, modref caches,
  `s_node_ntdll` / `s_node_kernel32`, `s_default_load_path`,
  `s_imports_fixup_done`, `s_attach_done`, `s_process_detaching`.
- `primary_ldr_state` — the static instance the first process claims.
- `ldr_state()` — O(1), no lock: `peb->LdrData` points at `s_ldr`, so
  `CONTAINING_RECORD` recovers the block.
- `#define`s redirect the former global names at their unchanged call sites.
  **Field names deliberately differ from macro names** (`s_hash_table` vs
  `hash_table`) so member access never re-expands.
- `loader_init` installs a block for any process arriving with
  `peb->LdrData == NULL`.

Shared on purpose (not per-process): `loader_section`, `peb_lock`, the
known-DLL directory handle.

---

## 6. The bootstrap boundary (deliberate, not a bug)

`spawn_process` falls back to the **fork** backend when either holds:

1. `!pRtlUserThreadStart || !pLdrInitializeThunk` — ntdll's PE side has not
   published its entry points yet. Wine creates a missing prefix from inside
   early init, so `wineboot` is spawned before this exists; entering a child
   then jumps to a NULL entry (raw SIGSEGV, empty stack).
2. `is_prefix_bootstrap` — a child's loader cannot resolve system DLLs from a
   prefix that does not exist yet.

These are the model's real preconditions: **a live ntdll PE side and an
initialized prefix**. On iOS there is no fork, so bootstrap must ship a
prepared prefix or defer — a stated architectural requirement, not a crash.

---

## 7. Traps that cost real time

- **`init_process_done` destroys staged startup info.** The server's
  `set_process_startup_state()` releases `process->startup_info`, so a child
  that reports done before calling `get_startup_info` gets an empty reply, and
  `env_pos = env_size - 1` underflows to `SIZE_MAX`. Order must be
  `init_first_thread` → `get_startup_info` → `init_process_done`. A guard now
  turns an empty reply into a clean child-only failure.
- **Regenerating patch 0003: 0002 also touches `dlls/ntdll/unix/loader.c`.**
  A naive `git diff -- dlls/ntdll/` folds 0002's hunk into 0003 and the series
  stops applying. Strip 0002 first:
  ```sh
  W=$PWD/../wine; P=$PWD
  git -C "$W" apply -R "$P/0002-no-fork-embedded-server.patch"
  git -C "$W" diff -- dlls/ntdll/ > "$P/0003-inproc-spawn-and-peb.patch"
  git -C "$W" apply "$P/0002-no-fork-embedded-server.patch"
  ```
- **`git -C <dir>` resolves relative paths *inside* `<dir>`.** Always pass
  patches as absolute paths. Getting this wrong silently no-ops every command,
  and a following redirect can overwrite the patch you were regenerating.
  (`apply.sh` now fails loudly instead of leaving a half-patched tree.)
- **Always finish with the round-trip check:**
  `./apply.sh --reverse && ./apply.sh --check && ./apply.sh`
- **Leftover `Xvfb` / `wineserver` processes cause false test failures.** A
  4/5 suite result was traced to this, not to a code change. Run
  `pkill Xvfb; pkill -x wineserver` before measuring.
- **The M1 gate has a measurement tension.** Killing the server between
  backends is required for the host-process count to mean anything (otherwise
  the second run inherits the first's `services.exe`/`explorer.exe`), but that
  same cold start is exactly what triggers the failing child path.

---

## 8. Test inventory

`native/wineforge/`, run with `make test` (no prefix needed) and
`make test-real` (needs a built loader):

- `embed_test`, `embed_run_test` — wineserver as a thread, embeddable shutdown.
- `peload_test` — PE map + relocate + run in-address-space, two images at once.
- `real_client_test`, `real_multi_test` — real Wine clients on the in-thread server.
- `embedded_server_test.sh` — embedded-only model, both directions.
- `spawn_seam_test.sh` — backend dispatch, in-process attach, per-child image.
- `inproc_run_test.sh` — children run; exit codes from the server trace;
  DLL-importing children; DLL-mapping measurement; cold-prefix creation.
- `mkexe.c` — emits import-free Win64 test programs.
- `m1_notepad_test.sh` — **the M1 gate. Currently failing. Not in the suite.**

---

## 9. Known stale text

- `inproc_run_test.sh`'s header comment still says DLL images are shared
  between pseudo-processes and blames `hostname.exe`'s failure on it. That was
  true on the `.so` build and is now **wrong** — the script's own output prints
  `… distinct kernel32 mappings — isolated`. Worth a one-line correction.

---

## 10. Suggested order of attack

1. Fix the GUI-child crash (§2). That is the whole remaining M1 blocker.
2. Re-run `m1_notepad_test.sh`; if it passes, wire it into `make test-real`.
3. Then, and only then, update `docs/ROADMAP.md`'s M1 checkbox.
4. Separately (not M1-blocking): per-pseudo-process fault containment — a
   crashing child should not be able to take the host down, which is why the
   backend is still opt-in.
