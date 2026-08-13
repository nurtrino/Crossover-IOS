#include "WineHost.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * The in-process device path was, until now, only ever exercised in the iOS
 * *simulator* (which allows RWX freely and has no 4GB __PAGEZERO), so a crash on
 * a real device landed with no trace. This file is deliberately paranoid: it
 * leaves an fsync'd breadcrumb at every startup stage in
 * "<prefix>.startup.log", and installs a triage signal handler that records the
 * faulting signal/address/backtrace before the process dies. On the next launch
 * the Swift side reads that log back so the console shows *where* it died.
 */

struct wine_console_session {
    char ntdll_path[1024];
    char prefix_path[1024];
    char log_path[1100];
    int  guest_stdin_rd;   /* fd 0 inside the guest */
    int  guest_stdout_wr;  /* fd 1/2 inside the guest */
    pthread_t thread;
    atomic_int finished;
    atomic_int exit_code;
};

/* void __wine_main(int argc, char *argv[]) — the ntdll entry the loader calls. */
typedef void (*wine_main_fn)(int, char **);

/* Statics the async-signal handler is allowed to touch (one console at a time). */
static int g_log_fd = -1;      /* the breadcrumb log */
static int g_pipe_fd = -1;     /* the guest stdout pipe (mirrors to the UI) */
static _Atomic(atomic_int *) g_finished_ptr = NULL;

/* --- breadcrumb logging (best-effort, fsync'd so a crash preserves it) ------ */

static void log_line(int log_fd, int mirror_fd, const char *msg)
{
    char buf[1200];
    time_t t = time(NULL);
    int n = snprintf(buf, sizeof(buf), "[%ld] %s\n", (long)t, msg);
    if (n < 0) return;
    if ((size_t)n > sizeof(buf)) n = sizeof(buf);
    if (log_fd >= 0) { ssize_t r = write(log_fd, buf, n); (void)r; fsync(log_fd); }
    if (mirror_fd >= 0) { ssize_t r = write(mirror_fd, buf, n); (void)r; }
}

/* --- triage signal handler -------------------------------------------------- */

static const char *signame(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    case SIGFPE:  return "SIGFPE";
    default:      return "SIG?";
    }
}

static void crash_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)uctx;
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "\n*** in-process crash: %s at fault addr %p ***",
                     signame(sig), info ? info->si_addr : (void *)0);
    if (n > 0) {
        if (g_log_fd >= 0)  { ssize_t r = write(g_log_fd, hdr, n);  (void)r; write(g_log_fd, "\n", 1); fsync(g_log_fd); }
        if (g_pipe_fd >= 0) { ssize_t r = write(g_pipe_fd, hdr, n); (void)r; write(g_pipe_fd, "\n", 1); }
    }
    void *frames[64];
    int fc = backtrace(frames, 64);
    if (g_log_fd >= 0)  backtrace_symbols_fd(frames, fc, g_log_fd);
    if (g_pipe_fd >= 0) backtrace_symbols_fd(frames, fc, g_pipe_fd);
    if (g_log_fd >= 0) fsync(g_log_fd);

    if (g_finished_ptr) atomic_store(g_finished_ptr, 1);

    /* Restore the default disposition and re-raise: we still crash (the fault is
     * not recoverable here), but now the breadcrumb + backtrace are on disk. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_triage_handlers(void)
{
    /* An altstack so a stack-overflow SIGSEGV can still run the handler.
     * Fixed 512 KB (SIGSTKSZ is a runtime value on newer Darwin, unusable for a
     * static array). */
    static char altstack[512 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0 };
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
    /* NB: Wine installs its own SIGSEGV/SIGBUS handlers during __wine_main init,
     * overwriting ours. So this catches faults BEFORE Wine's init (our bridge,
     * dlopen, early relocs) — which is exactly the blind spot we had. Faults
     * inside running Wine are handled by Wine's own segv_handler. The breadcrumb
     * markers below cover the "crashed somewhere inside __wine_main" case. */
}

/* --- the Wine thread -------------------------------------------------------- */

static void *wine_thread(void *arg)
{
    wine_console_session *s = (wine_console_session *)arg;
    void *h;
    wine_main_fn wine_main;
    char *wargv[] = { (char *)"wine", (char *)"cmd.exe", NULL };

    /* Route the guest's standard handles onto our pipes. */
    dup2(s->guest_stdin_rd, STDIN_FILENO);
    dup2(s->guest_stdout_wr, STDOUT_FILENO);
    dup2(s->guest_stdout_wr, STDERR_FILENO);

    /* Truncate/create the breadcrumb log for this run. */
    g_log_fd = open(s->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_pipe_fd = s->guest_stdout_wr;
    g_finished_ptr = &s->finished;
    install_triage_handlers();

    log_line(g_log_fd, g_pipe_fd, "stage: wine thread started");
    {
        char m[1200];
        snprintf(m, sizeof(m), "stage: ntdll=%s", s->ntdll_path);
        log_line(g_log_fd, g_pipe_fd, m);
        snprintf(m, sizeof(m), "stage: WINEPREFIX=%s", s->prefix_path);
        log_line(g_log_fd, g_pipe_fd, m);
    }

    log_line(g_log_fd, g_pipe_fd, "stage: dlopen(ntdll.so) begin");
    h = dlopen(s->ntdll_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        const char *e = dlerror();
        char m[1200];
        snprintf(m, sizeof(m), "FATAL: dlopen failed: %s", e ? e : "(null)");
        log_line(g_log_fd, g_pipe_fd, m);
        atomic_store(&s->exit_code, 255);
        atomic_store(&s->finished, 1);
        return NULL;
    }
    log_line(g_log_fd, g_pipe_fd, "stage: dlopen ok");

    wine_main = (wine_main_fn)dlsym(h, "__wine_main");
    if (!wine_main) {
        log_line(g_log_fd, g_pipe_fd, "FATAL: __wine_main not found in ntdll.so");
        atomic_store(&s->exit_code, 255);
        atomic_store(&s->finished, 1);
        return NULL;
    }
    log_line(g_log_fd, g_pipe_fd, "stage: __wine_main resolved; calling into Wine (cmd.exe)");

    wine_main(2, wargv);

    /* Reached only if the guest exit did not pthread_exit this thread. */
    log_line(g_log_fd, g_pipe_fd, "stage: __wine_main returned (guest exited)");
    atomic_store(&s->finished, 1);
    return NULL;
}

/* --- public API ------------------------------------------------------------- */

wine_console_session *wine_console_start(const char *ntdll_so_path,
                                         const char *prefix_path,
                                         int *out_stdin_fd,
                                         int *out_stdout_fd)
{
    wine_console_session *s;
    int inpipe[2];   /* app writes inpipe[1] -> guest reads inpipe[0] (stdin)  */
    int outpipe[2];  /* guest writes outpipe[1] -> app reads outpipe[0] (stdout) */

    if (!ntdll_so_path || !prefix_path || !out_stdin_fd || !out_stdout_fd) return NULL;
    if (access(ntdll_so_path, R_OK) != 0) return NULL;

    s = (wine_console_session *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    strncpy(s->ntdll_path, ntdll_so_path, sizeof(s->ntdll_path) - 1);
    strncpy(s->prefix_path, prefix_path, sizeof(s->prefix_path) - 1);
    snprintf(s->log_path, sizeof(s->log_path), "%s.startup.log", prefix_path);
    atomic_init(&s->finished, 0);
    atomic_init(&s->exit_code, 0);

    if (pipe(inpipe) != 0) { free(s); return NULL; }
    if (pipe(outpipe) != 0) { close(inpipe[0]); close(inpipe[1]); free(s); return NULL; }

    s->guest_stdin_rd = inpipe[0];
    s->guest_stdout_wr = outpipe[1];

    /* The environment the runtime needs. Set before the thread dlopen's ntdll so
     * virtual_init/init_environment pick it up. */
    setenv("WINEPREFIX", s->prefix_path, 1);
    setenv("WINE_INPROCESS", "1", 1);      /* guest exit -> pthread_exit, not exit() */
    /* Diagnostic default: trace DLL loads so the console shows how far init got
     * before a fault. The caller can still override WINEDEBUG in the environment. */
    setenv("WINEDEBUG", "+loaddll", 0);
    setenv("WINEDLLOVERRIDES", "mscoree=d;mshtml=d", 0);
    /* No desktop/display: keep cmd on the piped console. */
    unsetenv("DISPLAY");

    /* iOS gives secondary pthreads only a 512 KB stack; Wine's initial-thread
     * init (PE mapping, relocations, deep call chains) can overrun it and fault
     * with no trace. Give the Wine thread a generous 32 MB stack. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024 * 1024);

    int rc = pthread_create(&s->thread, &attr, wine_thread, s);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        free(s);
        return NULL;
    }
    pthread_detach(s->thread);

    /* App-side ends. The guest-side ends are owned by the wine thread (dup2'd). */
    *out_stdin_fd = inpipe[1];
    *out_stdout_fd = outpipe[0];
    return s;
}

int wine_console_is_finished(wine_console_session *s)
{
    return s ? atomic_load(&s->finished) : 1;
}

int wine_console_exit_code(wine_console_session *s)
{
    return s ? atomic_load(&s->exit_code) : -1;
}

void wine_console_free(wine_console_session *s)
{
    if (!s) return;
    free(s);
}
