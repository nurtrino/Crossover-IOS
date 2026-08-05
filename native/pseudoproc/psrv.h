/*
 * psrv — proof that a wineserver-shaped service runs as a *thread* in the
 * same address space as its clients, keeping wineserver's wire model.
 *
 * Real wineserver talks to each Wine process over a unix socketpair; the
 * request/reply framing doesn't care whether the peer is another process
 * or a thread. This demo keeps exactly that shape: per-client socketpair,
 * poll loop, request/reply structs — so the Wine fork can keep server.c
 * largely intact and only change how the "processes" come to exist.
 */
#ifndef PSRV_H
#define PSRV_H

#include <stdint.h>

enum psrv_op {
    PSRV_PING = 1,
    PSRV_NEW_HANDLE,       /* allocate a globally unique kernel handle */
    PSRV_DUP_HANDLE,       /* validate + duplicate an existing handle  */
};

struct psrv_request {
    uint32_t op;
    uint32_t pid;          /* pproc pid of the requesting client */
    uint64_t arg;
};

struct psrv_reply {
    int32_t status;        /* 0 = ok */
    uint64_t value;
};

/* Start the server thread. */
int psrv_start(void);

/* Stop the server thread and close all connections. */
void psrv_stop(void);

/*
 * Connect the calling pseudo-process to the server (one connection per
 * caller; equivalent of Wine's per-process server socket). Returns a
 * client fd to use with psrv_call, or -1.
 */
int psrv_connect(void);

/* Synchronous request/reply round-trip, like wine_server_call. */
int psrv_call(int fd, const struct psrv_request *req, struct psrv_reply *reply);

#endif /* PSRV_H */
