/*
 * pproc — single-address-space pseudo-process layer.
 *
 * iOS forbids fork/exec, so every Windows "process" must live inside one
 * host process. This layer models a process as a group of threads sharing
 * a per-process context (identity, name, argv, exit state) — the shape the
 * Wine fork's CreateProcess/NtTerminateProcess paths will target.
 *
 * This prototype is deliberately host-portable (Linux/macOS/iOS): the model
 * gets developed and regression-tested where debugging is cheap, then the
 * same API is wired into Wine's loader and kernelbase.
 *
 * Known divergences from real processes, tracked in docs/NATIVE_PORT.md:
 *  - no address-space isolation (PE relocation must resolve base conflicts)
 *  - per-process globals in loaded PEs need per-pseudo-process instancing
 *  - abnormal termination cannot reclaim leaked resources of siblings
 */
#ifndef PPROC_H
#define PPROC_H

#include <stdint.h>

typedef uint32_t pproc_pid_t;
#define PPROC_INVALID_PID ((pproc_pid_t)0)

/* Entry point of a pseudo-process "image"; returns its exit code. */
typedef int (*pproc_entry_t)(int argc, char **argv);

/* Initialize the layer. Must be called once before any spawn. */
int pproc_init(void);

/*
 * Spawn a pseudo-process — the replacement for fork/exec/CreateProcess.
 * name and argv are deep-copied. Returns the new pid or PPROC_INVALID_PID.
 */
pproc_pid_t pproc_spawn(const char *name, pproc_entry_t entry,
                        int argc, char *const *argv);

/* Identity of the calling thread's pseudo-process (0 if none). */
pproc_pid_t pproc_getpid(void);

/* Image name of the calling thread's pseudo-process (NULL if none). */
const char *pproc_getname(void);

/*
 * Create an auxiliary thread inside the calling pseudo-process.
 * The thread inherits the caller's process context (like CreateThread).
 */
int pproc_thread_create(void *(*fn)(void *), void *arg);

/*
 * Terminate the calling pseudo-process with the given exit code.
 * Prototype semantics: marks the process exited and ends the calling
 * thread; auxiliary threads are expected to observe pproc_exiting().
 */
_Noreturn void pproc_exit(int code);

/* True once the calling thread's pseudo-process has been asked to exit. */
int pproc_exiting(void);

/*
 * Wait for a pseudo-process to exit and reap it (WaitForSingleObject +
 * GetExitCodeProcess). Returns the exit code, or -1 on unknown pid.
 */
int pproc_wait(pproc_pid_t pid);

/* Number of live (unreaped) pseudo-processes. */
int pproc_count(void);

#endif /* PPROC_H */
