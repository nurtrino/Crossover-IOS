/* Regression tests for the pseudo-process model. */
#include "../pproc.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static atomic_int aux_ran;

/* Each aux thread must see its owning pseudo-process's identity. */
static void *aux_check_identity(void *arg)
{
    pproc_pid_t expected = (pproc_pid_t)(uintptr_t)arg;
    assert(pproc_getpid() == expected);
    atomic_fetch_add(&aux_ran, 1);
    return NULL;
}

static int entry_basic(int argc, char **argv)
{
    assert(argc == 2);
    assert(strcmp(argv[0], "child.exe") == 0);
    assert(pproc_getpid() != PPROC_INVALID_PID);
    assert(strcmp(pproc_getname(), "child.exe") == 0);
    return 42;
}

static int entry_with_threads(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < 4; i++)
        assert(pproc_thread_create(aux_check_identity,
                                   (void *)(uintptr_t)pproc_getpid()) == 0);
    return 7;
}

static int entry_explicit_exit(int argc, char **argv)
{
    (void)argc; (void)argv;
    pproc_exit(99);
    /* not reached */
}

static int entry_grandchild(int argc, char **argv)
{
    (void)argc; (void)argv;
    return 5;
}

/* A pseudo-process spawning another one — installer → app pattern. */
static int entry_parent(int argc, char **argv)
{
    (void)argc; (void)argv;
    char *cargv[] = {"grandchild.exe"};
    pproc_pid_t child = pproc_spawn("grandchild.exe", entry_grandchild, 1, cargv);
    assert(child != PPROC_INVALID_PID);
    assert(child != pproc_getpid());
    return pproc_wait(child) == 5 ? 0 : 1;
}

int main(void)
{
    assert(pproc_init() == 0);

    /* Host main() is not a pseudo-process. */
    assert(pproc_getpid() == PPROC_INVALID_PID);
    assert(pproc_getname() == NULL);

    /* Basic spawn/wait with argv passing and exit-code plumbing. */
    char *argv1[] = {"child.exe", "--flag"};
    pproc_pid_t p1 = pproc_spawn("child.exe", entry_basic, 2, argv1);
    assert(p1 != PPROC_INVALID_PID);
    assert(pproc_wait(p1) == 42);
    assert(pproc_count() == 0);

    /* Concurrent pseudo-processes get distinct identities. */
    char *argv2[] = {"a.exe"};
    char *argv3[] = {"b.exe"};
    pproc_pid_t pa = pproc_spawn("a.exe", entry_with_threads, 1, argv2);
    pproc_pid_t pb = pproc_spawn("b.exe", entry_with_threads, 1, argv3);
    assert(pa != pb);
    assert(pproc_wait(pa) == 7);
    assert(pproc_wait(pb) == 7);
    assert(atomic_load(&aux_ran) == 8);  /* all aux threads ran, none crossed */

    /* ExitProcess-style termination from inside the process. */
    char *argv4[] = {"quitter.exe"};
    pproc_pid_t p4 = pproc_spawn("quitter.exe", entry_explicit_exit, 1, argv4);
    assert(pproc_wait(p4) == 99);

    /* Nested spawn: pseudo-process creating a pseudo-process. */
    char *argv5[] = {"parent.exe"};
    pproc_pid_t p5 = pproc_spawn("parent.exe", entry_parent, 1, argv5);
    assert(pproc_wait(p5) == 0);

    /* Waiting on a reaped/unknown pid fails cleanly. */
    assert(pproc_wait(p1) == -1);
    assert(pproc_wait(12345) == -1);
    assert(pproc_count() == 0);

    printf("test_pproc: all assertions passed\n");
    return 0;
}
