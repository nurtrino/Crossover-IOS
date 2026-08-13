#ifndef CROSSOVER_WINE_HOST_H
#define CROSSOVER_WINE_HOST_H

/*
 * WineHost — the in-process bridge that lets the iOS app run a Windows console
 * program (cmd.exe) INSIDE its own process.
 *
 * iOS forbids an app from spawning a separate loader process, so instead of
 * exec'ing `wine`, the app dlopen's the embedded ntdll.so and calls __wine_main
 * on a background thread (Wine's apple_main_thread() no-ops off the main thread,
 * so UIKit keeps the main thread; ntdll self-locates the rest of the runtime via
 * dladdr, so the host being CrossoverPad instead of `wine` is fine).
 *
 * The guest's stdin/stdout/stderr are wired to pipes so the SwiftUI console can
 * stream output and send typed commands. A guest exit does not tear down the app
 * (the forkless build routes process-exit to pthread_exit under WINE_INPROCESS).
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wine_console_session wine_console_session;

/*
 * Start cmd.exe in-process.
 *
 *  ntdll_so_path : absolute path to the embedded ntdll.so (Wine self-locates the
 *                  rest of the runtime relative to it).
 *  prefix_path   : absolute path to a WRITABLE Wine prefix (the app copies the
 *                  read-only bundled prefix into Documents first).
 *  out_stdin_fd  : receives the fd the app WRITES to (guest stdin).
 *  out_stdout_fd : receives the fd the app READS from (guest stdout+stderr).
 *
 * Returns a session handle, or NULL on failure (out fds untouched).
 */
wine_console_session *wine_console_start(const char *ntdll_so_path,
                                         const char *prefix_path,
                                         int *out_stdin_fd,
                                         int *out_stdout_fd);

/* Non-zero once the guest has exited (the Wine thread returned/pthread_exited). */
int wine_console_is_finished(wine_console_session *s);

/* Guest exit code (valid once finished). */
int wine_console_exit_code(wine_console_session *s);

/* Release the session. Does not force-kill a live guest; close its stdin first. */
void wine_console_free(wine_console_session *s);

/*
 * Start a GUI program (e.g. "notepad.exe") in-process.
 *
 * Same in-process model as wine_console_start, but the guest's windows are
 * presented through the wineios.drv display bridge (WineDisplayHost.h) instead
 * of a terminal: call wine_display_host_configure() with the screen pixel size
 * BEFORE this, then poll wine_display_* from the presenter.
 *
 * The guest's stdout+stderr stream to *out_log_fd (read it like the console's
 * stdout pipe); stdin is /dev/null. Lifecycle queries and free are shared with
 * the console API (wine_console_is_finished / _exit_code / _free).
 */
wine_console_session *wine_gui_start(const char *ntdll_so_path,
                                     const char *prefix_path,
                                     const char *exe_name,
                                     int *out_log_fd);

#ifdef __cplusplus
}
#endif

#endif /* CROSSOVER_WINE_HOST_H */
