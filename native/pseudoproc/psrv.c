#include "psrv.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 128
#define MAX_HANDLES 4096

static struct {
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    int wakeup[2];                  /* self-pipe: new client / shutdown */
    int client_fds[MAX_CLIENTS];    /* server-side ends */
    int nclients;
    /* Global handle table: owner pid per handle, 0 = free. */
    uint32_t handle_owner[MAX_HANDLES];
    uint64_t next_handle;
} s;

static ssize_t read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
    size_t put = 0;
    while (put < len) {
        ssize_t n = write(fd, (const char *)buf + put, len - put);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        put += (size_t)n;
    }
    return (ssize_t)put;
}

static void handle_request(const struct psrv_request *req, struct psrv_reply *reply)
{
    memset(reply, 0, sizeof(*reply));
    pthread_mutex_lock(&s.lock);
    switch (req->op) {
    case PSRV_PING:
        reply->value = req->arg;
        break;
    case PSRV_NEW_HANDLE: {
        uint64_t h = s.next_handle++;
        if (h >= MAX_HANDLES) {
            reply->status = -1;
            break;
        }
        s.handle_owner[h] = req->pid;
        reply->value = h;
        break;
    }
    case PSRV_DUP_HANDLE: {
        uint64_t h = req->arg;
        if (h >= MAX_HANDLES || s.handle_owner[h] == 0) {
            reply->status = -1;  /* invalid handle */
            break;
        }
        uint64_t dup = s.next_handle++;
        if (dup >= MAX_HANDLES) {
            reply->status = -1;
            break;
        }
        s.handle_owner[dup] = req->pid;
        reply->value = dup;
        break;
    }
    default:
        reply->status = -2;
    }
    pthread_mutex_unlock(&s.lock);
}

static void *server_loop(void *unused)
{
    (void)unused;
    for (;;) {
        struct pollfd fds[MAX_CLIENTS + 1];
        int nfds;

        pthread_mutex_lock(&s.lock);
        fds[0].fd = s.wakeup[0];
        fds[0].events = POLLIN;
        for (int i = 0; i < s.nclients; i++) {
            fds[i + 1].fd = s.client_fds[i];
            fds[i + 1].events = POLLIN;
        }
        nfds = s.nclients + 1;
        pthread_mutex_unlock(&s.lock);

        if (poll(fds, (nfds_t)nfds, -1) < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }

        if (fds[0].revents & POLLIN) {
            char cmd;
            if (read(s.wakeup[0], &cmd, 1) == 1 && cmd == 'q') return NULL;
            continue;  /* client list changed; rebuild pollfds */
        }

        for (int i = 1; i < nfds; i++) {
            if (!(fds[i].revents & (POLLIN | POLLHUP))) continue;
            struct psrv_request req;
            ssize_t n = read_full(fds[i].fd, &req, sizeof(req));
            if (n <= 0) {           /* client vanished */
                pthread_mutex_lock(&s.lock);
                for (int j = 0; j < s.nclients; j++) {
                    if (s.client_fds[j] == fds[i].fd) {
                        close(s.client_fds[j]);
                        s.client_fds[j] = s.client_fds[--s.nclients];
                        break;
                    }
                }
                pthread_mutex_unlock(&s.lock);
                continue;
            }
            struct psrv_reply reply;
            handle_request(&req, &reply);
            write_full(fds[i].fd, &reply, sizeof(reply));
        }
    }
}

int psrv_start(void)
{
    if (s.running) return 0;
    memset(&s, 0, sizeof(s));
    pthread_mutex_init(&s.lock, NULL);
    s.next_handle = 1;  /* handle 0 reserved as invalid */
    if (pipe(s.wakeup)) return -1;
    if (pthread_create(&s.thread, NULL, server_loop, NULL)) {
        close(s.wakeup[0]);
        close(s.wakeup[1]);
        return -1;
    }
    s.running = 1;
    return 0;
}

void psrv_stop(void)
{
    if (!s.running) return;
    char q = 'q';
    if (write(s.wakeup[1], &q, 1) != 1) return;
    pthread_join(s.thread, NULL);
    pthread_mutex_lock(&s.lock);
    for (int i = 0; i < s.nclients; i++) close(s.client_fds[i]);
    s.nclients = 0;
    pthread_mutex_unlock(&s.lock);
    close(s.wakeup[0]);
    close(s.wakeup[1]);
    s.running = 0;
}

int psrv_connect(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return -1;

    pthread_mutex_lock(&s.lock);
    if (!s.running || s.nclients == MAX_CLIENTS) {
        pthread_mutex_unlock(&s.lock);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    s.client_fds[s.nclients++] = sv[0];
    pthread_mutex_unlock(&s.lock);

    char n = 'n';                    /* wake poll loop to pick up client */
    if (write(s.wakeup[1], &n, 1) != 1) {
        close(sv[1]);
        return -1;
    }
    return sv[1];
}

int psrv_call(int fd, const struct psrv_request *req, struct psrv_reply *reply)
{
    if (write_full(fd, req, sizeof(*req)) != sizeof(*req)) return -1;
    if (read_full(fd, reply, sizeof(*reply)) != sizeof(*reply)) return -1;
    return 0;
}
