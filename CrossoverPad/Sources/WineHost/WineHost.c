#include "WineHost.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct wine_console_session {
    char ntdll_path[1024];
    char prefix_path[1024];
    char exe_name[256];    /* guest program; "cmd.exe" for the console */
    int  guest_stdin_rd;   /* fd 0 inside the guest */
    int  guest_stdout_wr;  /* fd 1/2 inside the guest */
    pthread_t thread;
    atomic_int finished;
    atomic_int exit_code;
};

/* void __wine_main(int argc, char *argv[]) — the ntdll entry the loader calls. */
typedef void (*wine_main_fn)(int, char **);

static void *wine_thread(void *arg)
{
    wine_console_session *s = (wine_console_session *)arg;
    void *h;
    wine_main_fn wine_main;
    /* argv[0] is conventionally the loader path; ntdll only uses dladdr for
     * path detection, so any stable value works. cmd.exe with piped stdin runs
     * commands line-by-line and writes results to the piped stdout. */
    char *wargv[] = { (char *)"wine", s->exe_name, NULL };

    /* Route the guest's standard handles onto our pipes. */
    dup2(s->guest_stdin_rd, STDIN_FILENO);
    dup2(s->guest_stdout_wr, STDOUT_FILENO);
    dup2(s->guest_stdout_wr, STDERR_FILENO);

    h = dlopen(s->ntdll_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        const char *e = dlerror();
        if (e) { ssize_t r = write(STDERR_FILENO, e, strlen(e)); (void)r; }
        atomic_store(&s->exit_code, 255);
        atomic_store(&s->finished, 1);
        return NULL;
    }
    wine_main = (wine_main_fn)dlsym(h, "__wine_main");
    if (!wine_main) {
        const char *msg = "wine: __wine_main not found in ntdll.so\n";
        ssize_t r = write(STDERR_FILENO, msg, strlen(msg)); (void)r;
        atomic_store(&s->exit_code, 255);
        atomic_store(&s->finished, 1);
        return NULL;
    }

    wine_main(2, wargv);

    /* Reached only if the guest exit did not pthread_exit this thread. */
    atomic_store(&s->finished, 1);
    return NULL;
}

/* Shared starter: guest_stdin_rd/guest_stdout_wr must already be set. */
static wine_console_session *session_launch(wine_console_session *s)
{
    /* The environment the runtime needs. Set before the thread dlopen's ntdll so
     * virtual_init/init_environment pick it up. */
    setenv("WINEPREFIX", s->prefix_path, 1);
    setenv("WINE_INPROCESS", "1", 1);      /* guest exit -> pthread_exit, not exit() */
    setenv("WINEDEBUG", "-all", 0);        /* quiet unless the caller overrode it */
    setenv("WINEDLLOVERRIDES", "mscoree=d;mshtml=d", 0);
    /* No X11 display; GUI presentation goes through the wineios.drv bridge. */
    unsetenv("DISPLAY");

    if (pthread_create(&s->thread, NULL, wine_thread, s) != 0) return NULL;
    pthread_detach(s->thread);
    return s;
}

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
    strncpy(s->exe_name, "cmd.exe", sizeof(s->exe_name) - 1);
    atomic_init(&s->finished, 0);
    atomic_init(&s->exit_code, 0);

    if (pipe(inpipe) != 0) { free(s); return NULL; }
    if (pipe(outpipe) != 0) { close(inpipe[0]); close(inpipe[1]); free(s); return NULL; }

    s->guest_stdin_rd = inpipe[0];
    s->guest_stdout_wr = outpipe[1];

    if (!session_launch(s)) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        free(s);
        return NULL;
    }

    /* App-side ends. The guest-side ends are owned by the wine thread (dup2'd). */
    *out_stdin_fd = inpipe[1];
    *out_stdout_fd = outpipe[0];
    return s;
}

wine_console_session *wine_gui_start(const char *ntdll_so_path,
                                     const char *prefix_path,
                                     const char *exe_name,
                                     int *out_log_fd)
{
    wine_console_session *s;
    int devnull;
    int outpipe[2];  /* guest writes outpipe[1] -> app reads outpipe[0] (log) */

    if (!ntdll_so_path || !prefix_path || !exe_name || !out_log_fd) return NULL;
    if (access(ntdll_so_path, R_OK) != 0) return NULL;

    s = (wine_console_session *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    strncpy(s->ntdll_path, ntdll_so_path, sizeof(s->ntdll_path) - 1);
    strncpy(s->prefix_path, prefix_path, sizeof(s->prefix_path) - 1);
    strncpy(s->exe_name, exe_name, sizeof(s->exe_name) - 1);
    atomic_init(&s->finished, 0);
    atomic_init(&s->exit_code, 0);

    if ((devnull = open("/dev/null", O_RDONLY)) < 0) { free(s); return NULL; }
    if (pipe(outpipe) != 0) { close(devnull); free(s); return NULL; }

    s->guest_stdin_rd = devnull;
    s->guest_stdout_wr = outpipe[1];

    if (!session_launch(s)) {
        close(devnull);
        close(outpipe[0]); close(outpipe[1]);
        free(s);
        return NULL;
    }

    *out_log_fd = outpipe[0];
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
