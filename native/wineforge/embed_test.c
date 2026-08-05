/*
 * embed_test — run the real, unmodified wineserver as a THREAD.
 *
 * This is the first live experiment of the M1 surgery (docs/NATIVE_PORT.md,
 * Blocker 1 step 2): wineserver's objects are linked into this host binary
 * with two link-time transforms and zero source patches:
 *
 *   - objcopy --redefine-sym main=wineserver_main   (server/main.o)
 *   - -Wl,--wrap=exit : wineserver shuts down via exit(0) in
 *     close_socket_timeout(); in a single-process world that must become
 *     "the server thread ends". __wrap_exit() records the code and turns
 *     exit() into pthread_exit() when called on the server thread.
 *
 * Test sequence:
 *   1. spawn wineserver_main("-f -d1") on a thread (foreground: no fork)
 *   2. wait for the master socket to appear under the scratch WINEPREFIX
 *   3. connect a client over AF_UNIX and disconnect — exercises the accept
 *      path (wineserver creates and tears down a process object for us)
 *   4. request shutdown the way an embedding host would (SIGINT -> the
 *      server's self-pipe -> shutdown_master_socket), then join the thread
 *      and check the recorded "exit" code is 0 — the whole lifecycle ran
 *      in-process
 */
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int wineserver_main(int argc, char *argv[]);

static pthread_t server_thread;
static int server_exit_code = -1;
static int exit_was_wrapped;

void __real_exit(int code);

void __wrap_exit(int code)
{
    if (pthread_equal(pthread_self(), server_thread)) {
        server_exit_code = code;
        exit_was_wrapped = 1;
        pthread_exit(NULL);
    }
    __real_exit(code);
}

/*
 * wineserver derives its data directories (nls files etc.) from argv[0],
 * so the embedding must hand it a path inside the wine build tree. In the
 * iOS app this becomes the bundle's wine resource path.
 */
static char server_argv0[512] = "wineserver";

static void *server_entry(void *unused)
{
    (void)unused;
    char *argv[] = {server_argv0, "-f", "-d1", NULL};
    wineserver_main(3, argv);
    /* normal return also means clean shutdown */
    if (server_exit_code < 0) server_exit_code = 0;
    return NULL;
}

/* Replicate ntdll's server-dir derivation: /tmp/.wine-<uid>/server-<dev>-<ino> */
static void socket_path_for_prefix(const char *prefix, char *out, size_t len)
{
    struct stat st;
    if (stat(prefix, &st)) {
        perror("stat prefix");
        __real_exit(1);
    }
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
    char prefix[256], sock_path[512];

    if (argc > 1)
        snprintf(server_argv0, sizeof(server_argv0), "%s", argv[1]);

    snprintf(prefix, sizeof(prefix), "/tmp/wineforge-test-%d", getpid());
    if (mkdir(prefix, 0700) && errno != EEXIST) {
        perror("mkdir prefix");
        return 1;
    }
    setenv("WINEPREFIX", prefix, 1);

    if (pthread_create(&server_thread, NULL, server_entry, NULL)) {
        perror("pthread_create");
        return 1;
    }

    socket_path_for_prefix(prefix, sock_path, sizeof(sock_path));
    if (wait_for_socket(sock_path, 5000)) {
        fprintf(stderr, "FAIL: master socket never appeared at %s\n", sock_path);
        return 1;
    }
    printf("embed_test: wineserver thread is up, socket at %s\n", sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(sock_path) >= sizeof(addr.sun_path)) { printf("socket path too long\n"); return -1; }
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("FAIL: connect to wineserver");
        return 1;
    }
    printf("embed_test: client connected (a Wine process is born)\n");
    close(fd);
    printf("embed_test: client disconnected\n");

    /*
     * A raw client never registers as a user process, so it can't drive the
     * "last process exited" shutdown timer. Instead exercise the shutdown
     * path an embedding actually needs: host-initiated termination.
     * wineserver's handlers are self-pipe based — any thread may raise the
     * signal; the server thread consumes it and runs shutdown_master_socket
     * -> close_socket_timeout -> exit(0), which __wrap_exit turns into the
     * server thread ending.
     */
    usleep(200 * 1000);  /* let the EOF get processed first */
    printf("embed_test: requesting shutdown (host-side SIGINT)\n");
    raise(SIGINT);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 15;  /* master-socket timeout is ~3 s */
    int rc = pthread_timedjoin_np(server_thread, NULL, &deadline);
    if (rc) {
        fprintf(stderr, "FAIL: server thread did not shut down: %s\n", strerror(rc));
        __real_exit(1);
    }

    if (server_exit_code != 0) {
        fprintf(stderr, "FAIL: server exit code %d\n", server_exit_code);
        return 1;
    }
    printf("embed_test: PASS — wineserver lived and died as a thread "
           "(exit(0) %s)\n", exit_was_wrapped ? "intercepted" : "not needed");
    return 0;
}
