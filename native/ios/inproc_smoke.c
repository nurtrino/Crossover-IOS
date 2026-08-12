/*
 * inproc_smoke — proves CrossoverPad's in-process execution model.
 *
 * iOS forbids an app from spawning a separate loader process, so the app must
 * run the Wine runtime INSIDE its own process: dlopen the embedded ntdll.so and
 * call __wine_main on a background thread (the app's main thread stays with
 * UIKit; Wine's apple_main_thread() no-ops off the main thread). This harness is
 * a NON-wine host that does exactly that, so a green run here means the app's
 * bridge is sound before any Swift is written.
 *
 * It also verifies the crucial property that makes an in-app console possible:
 * when the guest exits, the host process must SURVIVE. The forkless build routes
 * a guest process-exit to pthread_exit() (not exit()) when WINE_INPROCESS is set,
 * so pthread_join() returns and we print the survival marker instead of the
 * whole host dying.
 *
 * ntdll self-locates the rest of the runtime via dladdr(init_paths), so the host
 * executable need not be `wine`.
 *
 * usage: inproc_smoke <path/to/ntdll.so> <path/to/loader/wine>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_ntdll;
static const char *g_loader;

static void *wine_thread(void *arg)
{
    void *h;
    void (*wine_main)(int, char **);
    char *wargv[] = { NULL, "cmd.exe", "/c",
                      "echo INPROC_RAN_%PROCESSOR_ARCHITECTURE%", NULL };

    (void)arg;
    h = dlopen(g_ntdll, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "INPROC_FAIL dlopen: %s\n", dlerror()); return (void *)1; }
    wine_main = (void (*)(int, char **))dlsym(h, "__wine_main");
    if (!wine_main) { fprintf(stderr, "INPROC_FAIL no __wine_main\n"); return (void *)1; }

    wargv[0] = (char *)g_loader;   /* argv[0]: the loader path, as `wine` passes */
    fflush(stdout);
    wine_main(4, wargv);

    /* Only reached if __wine_main returns without the process exiting. */
    fprintf(stderr, "INPROC_NOTE __wine_main returned normally\n");
    return (void *)0;
}

int main(int argc, char **argv)
{
    pthread_t t;
    void *ret = NULL;

    if (argc < 3) { fprintf(stderr, "usage: %s <ntdll.so> <loader/wine>\n", argv[0]); return 2; }
    g_ntdll = argv[1];
    g_loader = argv[2];

    setenv("WINE_INPROCESS", "1", 1);   /* route guest exit -> pthread_exit, not exit() */

    if (pthread_create(&t, NULL, wine_thread, NULL)) { perror("pthread_create"); return 1; }
    pthread_join(t, &ret);

    /* Reaching here means the guest exit did NOT tear down the host process —
     * exactly what an in-app console needs. */
    printf("INPROC_HOST_SURVIVED code=%ld\n", (long)ret);
    fflush(stdout);
    return 0;
}
