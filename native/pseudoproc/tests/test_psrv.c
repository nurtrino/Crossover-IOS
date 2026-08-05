/*
 * Proves the wineserver-as-thread model: multiple pseudo-processes make
 * concurrent server calls over per-client socketpairs and observe a
 * consistent global handle table, all inside one host process.
 */
#include "../pproc.h"
#include "../psrv.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HANDLES_PER_CLIENT 50
#define NCLIENTS 8

/* Each client records the handles it was granted here (indexed by pid). */
static uint64_t granted[NCLIENTS + 2][HANDLES_PER_CLIENT];

static int entry_client(int argc, char **argv)
{
    (void)argc; (void)argv;
    int fd = psrv_connect();
    assert(fd >= 0);

    struct psrv_request req;
    struct psrv_reply reply;

    /* Round-trip sanity. */
    req.op = PSRV_PING;
    req.pid = pproc_getpid();
    req.arg = 0xC0FFEE;
    assert(psrv_call(fd, &req, &reply) == 0);
    assert(reply.status == 0 && reply.value == 0xC0FFEE);

    /* Allocate handles; server must never hand out duplicates. */
    for (int i = 0; i < HANDLES_PER_CLIENT; i++) {
        req.op = PSRV_NEW_HANDLE;
        req.arg = 0;
        assert(psrv_call(fd, &req, &reply) == 0);
        assert(reply.status == 0 && reply.value != 0);
        granted[pproc_getpid()][i] = reply.value;
    }

    /* Duplicate one of our handles; must yield a new valid handle. */
    req.op = PSRV_DUP_HANDLE;
    req.arg = granted[pproc_getpid()][0];
    assert(psrv_call(fd, &req, &reply) == 0);
    assert(reply.status == 0 && reply.value != req.arg);

    /* Duplicating garbage must fail without wedging the server. */
    req.op = PSRV_DUP_HANDLE;
    req.arg = 0;
    assert(psrv_call(fd, &req, &reply) == 0);
    assert(reply.status != 0);

    return 0;
}

int main(void)
{
    assert(pproc_init() == 0);
    assert(psrv_start() == 0);

    pproc_pid_t pids[NCLIENTS];
    for (int i = 0; i < NCLIENTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "client%d.exe", i);
        char *argv[] = {name};
        pids[i] = pproc_spawn(name, entry_client, 1, argv);
        assert(pids[i] != PPROC_INVALID_PID);
    }
    for (int i = 0; i < NCLIENTS; i++)
        assert(pproc_wait(pids[i]) == 0);

    /* Cross-check: no handle value was granted to two clients. */
    for (int a = 1; a <= NCLIENTS; a++)
        for (int b = a + 1; b <= NCLIENTS; b++)
            for (int i = 0; i < HANDLES_PER_CLIENT; i++)
                for (int j = 0; j < HANDLES_PER_CLIENT; j++)
                    assert(granted[a][i] != granted[b][j]);

    psrv_stop();
    printf("test_psrv: all assertions passed\n");
    return 0;
}
