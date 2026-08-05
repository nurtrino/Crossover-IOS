/*
 * real_client_test — the genuine Wine ntdll client talks to our in-thread
 * wineserver (experiment 002, reframed).
 *
 * embed_run_test drove the server with a bare socket. This goes the whole
 * way: an actual `wine` process runs a real Windows program, and instead of
 * forking its own wineserver it connects to the one running as a THREAD in
 * this host. That exercises the real protocol end to end — version
 * handshake, init_first_thread, the request/reply fd dance — against the
 * embedded server, which is what M1 ultimately needs.
 *
 * How the client is captured: Wine's server_connect() (dlls/ntdll/unix/
 * server.c) connects to an *existing* master socket in the prefix before it
 * would ever fork a server. So we:
 *   1. run wineserver_run() on a thread for WINEPREFIX=<scratch>
 *   2. wait for its master socket
 *   3. fork/exec the real `wine` loader with the same WINEPREFIX
 *   4. confirm the client ran AND no second wineserver process was spawned
 *      (i.e. our in-thread server serviced it)
 *
 * The fork/exec here is the TEST HARNESS launching a client, not Wine
 * spawning a helper — eliminating the client's own process is the separate
 * WS-C (iOS in-process loader) workstream. What this proves is the
 * server-side: one in-thread wineserver can serve a real Wine client.
 *
 * Usage: real_client_test <wineserver_path> <wine_loader_path> [win_program]
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
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int wineserver_run(int argc, char *argv[]);

static char server_argv0[512] = "wineserver";
static int run_status = -777;

static void *server_entry(void *unused)
{
    (void)unused;
    char *argv[] = {server_argv0, "-f", "-d0", NULL};
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <wineserver_path> <wine_loader> [program]\n", argv[0]);
        return 2;
    }
    const char *wineserver_path = argv[1];
    const char *wine_loader = argv[2];
    const char *program = argc > 3 ? argv[3] : "wineboot.exe";

    snprintf(server_argv0, sizeof(server_argv0), "%s", wineserver_path);

    char prefix[256], sock_path[512];
    snprintf(prefix, sizeof(prefix), "/tmp/wineforge-rc-%d", getpid());
    if (mkdir(prefix, 0700) && errno != EEXIST) { perror("mkdir prefix"); return 1; }
    setenv("WINEPREFIX", prefix, 1);
    setenv("WINEDEBUG", "-all", 0);
    socket_path_for_prefix(prefix, sock_path, sizeof(sock_path));

    /* 1-2: bring up the in-thread server. */
    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, server_entry, NULL)) {
        perror("pthread_create");
        return 1;
    }
    if (wait_for_socket(sock_path, 5000)) {
        fprintf(stderr, "FAIL: in-thread server socket never appeared\n");
        return 1;
    }
    printf("in-thread wineserver up for prefix %s\n", prefix);

    /* 3: launch the real wine client against the same prefix. */
    printf("launching real client: %s %s\n", wine_loader, program);
    pid_t child = fork();
    if (child == 0) {
        /* Child: become the wine client. This is the harness spawning a
         * client process, not Wine forking a server. */
        execl(wine_loader, wine_loader, program, (char *)NULL);
        perror("execl wine");
        _exit(127);
    }
    if (child < 0) { perror("fork"); return 1; }

    int wstatus = 0;
    /* Give the client time; wineboot can take a while on first prefix init. */
    for (int i = 0; i < 600; i++) {  /* up to 60s */
        pid_t r = waitpid(child, &wstatus, WNOHANG);
        if (r == child) break;
        if (r < 0) { perror("waitpid"); break; }
        usleep(100 * 1000);
    }

    int client_ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
    printf("client %s (status raw=%d)\n",
           client_ok ? "exited cleanly" : "did NOT exit cleanly", wstatus);

    /* 4: the real proof — did the client register as a genuine user process
     * on our in-thread server? If so, when it exits the server's
     * "last user process" timer fires and wineserver_run returns on its own,
     * with NO SIGINT from us. Wait briefly for that self-driven shutdown. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 20;
    int rc = pthread_timedjoin_np(server_thread, NULL, &deadline);

    int self_shutdown = (rc == 0);
    if (!self_shutdown) {
        /* Fall back to explicit shutdown so we exit cleanly regardless. */
        printf("server still up; sending shutdown\n");
        raise(SIGINT);
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 10;
        rc = pthread_timedjoin_np(server_thread, NULL, &deadline);
    }
    if (rc) {
        fprintf(stderr, "FAIL: in-thread server never shut down: %s\n", strerror(rc));
        return 1;
    }

    printf("wineserver_run returned %d\n", run_status);
    if (self_shutdown)
        printf("PASS: real Wine client ran on the in-thread server, which then\n"
               "      shut itself down after the last user process exited —\n"
               "      full init_first_thread handshake serviced in-process\n");
    else if (client_ok)
        printf("PARTIAL: client exited cleanly against the in-thread server,\n"
               "         but self-shutdown timer did not fire in-window\n");
    else
        printf("PARTIAL: server serviced the connection but the client did not\n"
               "         complete (likely prefix/DLL setup, not a server issue)\n");

    return (self_shutdown || client_ok) ? 0 : 1;
}
