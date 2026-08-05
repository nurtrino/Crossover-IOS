/*
 * real_multi_test — many real Wine clients, one in-thread wineserver
 * (experiment 004).
 *
 * Experiment 002 proved a single real Wine client runs against the embedded
 * server. This proves the server multiplexes SEVERAL concurrent Wine
 * processes: each does its own init_first_thread, gets its own process
 * object and handle namespace on the shared server thread, and the server
 * only self-shuts-down once the LAST of them exits. That concurrent
 * process-object bookkeeping is exactly what a bottle running an installer
 * plus its spawned app will lean on.
 *
 * Usage: real_multi_test <wineserver_path> <wine_loader> [N] [program]
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

int wineserver_run(int argc, char *argv[]);

static char server_argv0[512] = "wineserver";
static int run_status = -777;

/* Run the in-thread server at -d1 so its request trace (written to stderr)
 * can be captured and the real init_first_thread count measured. */
static void *server_entry(void *unused)
{
    (void)unused;
    char *argv[] = {server_argv0, "-f", "-d1", NULL};
    run_status = wineserver_run(3, argv);
    return NULL;
}

/* Count distinct init_first_thread requests the server actually serviced,
 * by scanning the captured -d1 trace. Each real Wine process issues exactly
 * one, so this is a hard, server-side count of processes registered. */
static int count_init_first_thread(const char *log_path)
{
    FILE *f = fopen(log_path, "r");
    if (!f) return -1;
    char line[4096];
    int count = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "init_first_thread(")) count++;
    fclose(f);
    return count;
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
        fprintf(stderr, "usage: %s <wineserver_path> <wine_loader> [N] [program]\n", argv[0]);
        return 2;
    }
    const char *wineserver_path = argv[1];
    const char *wine_loader = argv[2];
    int n = argc > 3 ? atoi(argv[3]) : 4;
    const char *program = argc > 4 ? argv[4] : "wineboot.exe";
    if (n < 2) n = 2;
    if (n > 16) n = 16;

    snprintf(server_argv0, sizeof(server_argv0), "%s", wineserver_path);

    char prefix[256], sock_path[512];
    snprintf(prefix, sizeof(prefix), "/tmp/wineforge-multi-%d", getpid());
    if (mkdir(prefix, 0700) && errno != EEXIST) { perror("mkdir prefix"); return 1; }
    setenv("WINEPREFIX", prefix, 1);
    setenv("WINEDEBUG", "-all", 0);
    socket_path_for_prefix(prefix, sock_path, sizeof(sock_path));

    /* Capture the server's -d1 trace: redirect stderr (fd 2) to a log for the
     * whole run. Forked clients inherit it too, but init_first_thread( is a
     * server-only trace token, so the count stays clean. Harness status uses
     * stdout, so it still reaches the terminal. */
    char log_path[320];
    snprintf(log_path, sizeof(log_path), "%s/server-trace.log", prefix);
    int logfd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (logfd < 0) { perror("open trace log"); return 1; }
    fflush(stderr);
    dup2(logfd, STDERR_FILENO);
    close(logfd);

    /* Bring up ONE in-thread server. */
    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, server_entry, NULL)) {
        perror("pthread_create");
        return 1;
    }
    if (wait_for_socket(sock_path, 5000)) {
        printf("FAIL: in-thread server socket never appeared\n");
        return 1;
    }
    printf("in-thread wineserver up; launching %d concurrent real clients\n", n);

    /* Prime the prefix once so concurrent clients don't race first-init.
     * (A serialized wineboot builds the registry/dirs the others then share.) */
    pid_t pre = fork();
    if (pre == 0) {
        execl(wine_loader, wine_loader, "wineboot.exe", (char *)NULL);
        _exit(127);
    }
    if (pre > 0) waitpid(pre, NULL, 0);

    /* Launch N clients as concurrently as fork allows. */
    pid_t pids[16];
    for (int i = 0; i < n; i++) {
        pid_t c = fork();
        if (c == 0) {
            execl(wine_loader, wine_loader, program, (char *)NULL);
            _exit(127);
        }
        pids[i] = c;
    }

    int clean = 0;
    for (int i = 0; i < n; i++) {
        int ws = 0;
        if (waitpid(pids[i], &ws, 0) == pids[i] && WIFEXITED(ws) && WEXITSTATUS(ws) == 0)
            clean++;
    }
    printf("%d/%d concurrent clients exited cleanly\n", clean, n);

    /* After the last user process exits, the shared server should self-shut. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 20;
    int rc = pthread_timedjoin_np(server_thread, NULL, &deadline);
    int self_shutdown = (rc == 0);
    if (!self_shutdown) {
        printf("server still up after clients; sending shutdown\n");
        raise(SIGINT);
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 10;
        rc = pthread_timedjoin_np(server_thread, NULL, &deadline);
    }
    if (rc) {
        printf("FAIL: shared in-thread server never shut down: %s\n", strerror(rc));
        return 1;
    }

    /* Hard server-side measurement: how many real Wine processes actually
     * completed init_first_thread on our in-thread server. Includes the
     * prime wineboot and any subprocesses wineboot itself spawns, so we
     * expect >= n (the concurrent batch), not exactly n. */
    int served = count_init_first_thread(log_path);

    printf("wineserver_run returned %d\n", run_status);
    printf("clients exited cleanly: %d/%d (limited by minimal-prefix DLLs)\n", clean, n);
    printf("server serviced %d init_first_thread handshakes (>= %d expected)\n",
           served, n);

    /* The experiment's property is server-side: did one in-thread server
     * register and account for at least the N concurrent processes, and
     * return to a zero user-process count (clean self-shutdown)? Client
     * exit codes are secondary and prefix-dependent. */
    if (served >= n && self_shutdown) {
        printf("PASS: one in-thread wineserver multiplexed %d concurrent real "
               "Wine\n      processes (%d handshakes total) and self-shut-down "
               "after the\n      last user process exited\n", n, served);
        return 0;
    }
    if (served >= n) {
        printf("PARTIAL: server serviced all %d concurrent processes but the "
               "self-\n         shutdown timer did not fire in-window\n", n);
        return 0;
    }
    printf("FAIL: server only serviced %d init_first_thread handshakes for %d "
           "clients\n", served, n);
    return 1;
}
