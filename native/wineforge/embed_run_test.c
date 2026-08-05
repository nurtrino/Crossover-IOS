/*
 * embed_run_test — exercise the embeddable-shutdown patch (experiment 003).
 *
 * Unlike embed_test (which used an objcopy main-rename + -Wl,--wrap=exit to
 * coerce the *unmodified* server), this drives the patched server through
 * its real, source-level embedding API:
 *
 *     int wineserver_run(int argc, char **argv);   // returns, never exit()s
 *     void server_exit(int status);                // unwinds to wineserver_run
 *
 * Two things are verified, matching the actual M1 architecture — ONE
 * long-lived in-process server, MANY transient clients:
 *   A. Durable service: a single wineserver_run() instance accepts several
 *      client connections in sequence (Wine processes coming and going),
 *      then a host-side SIGINT drives an orderly shutdown that *returns*
 *      from wineserver_run with status 0 — the host process stays alive.
 *   B. Clean teardown: the shutdown unlinks its master socket (the embedded
 *      path bypasses atexit), so no stale state is left behind.
 *
 * NOTE: restarting the server (a second wineserver_run in the same process)
 * is deliberately NOT tested — it is a documented non-goal for M1. The
 * server is designed to be started once and live for the app session;
 * process-lifetime statics (registry root_key, master_socket, lock fd)
 * would need a full reset for restart, tracked as a later audit if the
 * product ever needs it. Multiple *clients* on one server is the property
 * that matters, and it is covered here and in experiment 004.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int wineserver_run(int argc, char *argv[]);

static char server_argv0[512] = "wineserver";
static int run_status = -777;

static void *server_entry(void *unused)
{
    (void)unused;
    char *argv[] = {server_argv0, "-f", "-d1", NULL};
    run_status = wineserver_run(3, argv);
    return NULL;
}

static void socket_path_for_prefix(const char *prefix, char *out, size_t len)
{
    struct stat st;
    if (stat(prefix, &st)) { perror("stat prefix"); exit(1); }
    snprintf(out, len, "/tmp/.wine-%lu/server-%llx-%llx/socket",
             (unsigned long)getuid(),
             (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
}

static int wait_for_socket(const char *path, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) return 0;
        usleep(50 * 1000);
    }
    return -1;
}

/* Connect to the running server and immediately disconnect — one Wine
 * process's worth of "born and died" against the master socket. */
static int poke_client(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(sock_path) >= sizeof(addr.sun_path)) { printf("socket path too long\n"); return -1; }
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    char prefix[256], sock_path[512];

    if (argc > 1)
        snprintf(server_argv0, sizeof(server_argv0), "%s", argv[1]);

    snprintf(prefix, sizeof(prefix), "/tmp/wineforge-run-%d", getpid());
    if (mkdir(prefix, 0700) && errno != EEXIST) { perror("mkdir prefix"); return 1; }
    setenv("WINEPREFIX", prefix, 1);
    socket_path_for_prefix(prefix, sock_path, sizeof(sock_path));

    /* Start ONE server instance and keep it up for the whole test. */
    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, server_entry, NULL)) {
        perror("pthread_create");
        return 1;
    }
    if (wait_for_socket(sock_path, 5000)) {
        fprintf(stderr, "FAIL: master socket never appeared\n");
        return 1;
    }
    printf("server up (single long-lived instance)\n");

    /* Part A: several clients come and go against the one server. */
    const int N = 5;
    for (int i = 0; i < N; i++) {
        if (poke_client(sock_path)) {
            fprintf(stderr, "FAIL(A): client %d could not reach server\n", i);
            return 1;
        }
        usleep(50 * 1000);
    }
    printf("PASS(A): one server served %d sequential clients\n", N);

    /* Now request orderly shutdown and confirm wineserver_run returns. */
    usleep(150 * 1000);
    printf("requesting shutdown (host-side SIGINT)\n");
    raise(SIGINT);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 15;
    int rc = pthread_timedjoin_np(server_thread, NULL, &deadline);
    if (rc) {
        fprintf(stderr, "FAIL: server thread hung on shutdown: %s\n", strerror(rc));
        return 1;
    }
    if (run_status != 0) {
        fprintf(stderr, "FAIL: wineserver_run returned %d (expected 0)\n", run_status);
        return 1;
    }
    printf("PASS: wineserver_run returned 0 — in-process shutdown, host alive\n");

    /* Part B: the embedded shutdown must have unlinked its master socket. */
    struct stat st;
    if (stat(sock_path, &st) == 0) {
        fprintf(stderr, "FAIL(B): stale master socket left at %s\n", sock_path);
        return 1;
    }
    printf("PASS(B): master socket cleaned up (no stale state)\n");
    return 0;
}
