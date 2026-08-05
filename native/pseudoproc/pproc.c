#define _POSIX_C_SOURCE 200809L

#include "pproc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PPROCS 256

enum pproc_state { SLOT_FREE, PROC_RUNNING, PROC_EXITED };

struct pproc {
    pproc_pid_t pid;
    char *name;
    int argc;
    char **argv;
    pproc_entry_t entry;
    pthread_t main_thread;
    enum pproc_state state;
    int exit_code;
    int exit_requested;
    int aux_threads;            /* live auxiliary threads */
    pthread_cond_t aux_done;    /* signaled when aux_threads hits 0 */
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t state_changed;
    pthread_key_t current;      /* thread -> struct pproc* */
    struct pproc table[MAX_PPROCS];
    pproc_pid_t next_pid;
    int live;
    int initialized;
} g;

/* ---- internals ------------------------------------------------------- */

static struct pproc *find_locked(pproc_pid_t pid)
{
    for (int i = 0; i < MAX_PPROCS; i++)
        if (g.table[i].state != SLOT_FREE && g.table[i].pid == pid)
            return &g.table[i];
    return NULL;
}

static void free_slot_locked(struct pproc *p)
{
    for (int i = 0; i < p->argc; i++) free(p->argv[i]);
    free(p->argv);
    free(p->name);
    pthread_cond_destroy(&p->aux_done);
    memset(p, 0, sizeof(*p));
    p->state = SLOT_FREE;
}

struct trampoline_arg {
    struct pproc *proc;
    void *(*aux_fn)(void *);
    void *aux_arg;
};

static void *main_trampoline(void *opaque)
{
    struct trampoline_arg *t = opaque;
    struct pproc *p = t->proc;
    free(t);

    pthread_setspecific(g.current, p);
    int code = p->entry(p->argc, p->argv);

    pthread_mutex_lock(&g.lock);
    /* pproc_exit() may already have recorded a code from another thread. */
    if (!p->exit_requested) p->exit_code = code;
    p->exit_requested = 1;
    while (p->aux_threads > 0)
        pthread_cond_wait(&p->aux_done, &g.lock);
    p->state = PROC_EXITED;
    pthread_cond_broadcast(&g.state_changed);
    pthread_mutex_unlock(&g.lock);
    return NULL;
}

static void *aux_trampoline(void *opaque)
{
    struct trampoline_arg *t = opaque;
    struct pproc *p = t->proc;
    void *(*fn)(void *) = t->aux_fn;
    void *arg = t->aux_arg;
    free(t);

    pthread_setspecific(g.current, p);
    fn(arg);

    pthread_mutex_lock(&g.lock);
    if (--p->aux_threads == 0) pthread_cond_broadcast(&p->aux_done);
    pthread_mutex_unlock(&g.lock);
    return NULL;
}

/* ---- API ------------------------------------------------------------- */

int pproc_init(void)
{
    if (g.initialized) return 0;
    if (pthread_key_create(&g.current, NULL)) return -1;
    pthread_mutex_init(&g.lock, NULL);
    pthread_cond_init(&g.state_changed, NULL);
    g.next_pid = 1;
    g.initialized = 1;
    return 0;
}

pproc_pid_t pproc_spawn(const char *name, pproc_entry_t entry,
                        int argc, char *const *argv)
{
    if (!g.initialized || !entry || argc < 0) return PPROC_INVALID_PID;

    pthread_mutex_lock(&g.lock);
    struct pproc *p = NULL;
    for (int i = 0; i < MAX_PPROCS; i++) {
        if (g.table[i].state == SLOT_FREE) { p = &g.table[i]; break; }
    }
    if (!p) {
        pthread_mutex_unlock(&g.lock);
        return PPROC_INVALID_PID;
    }

    p->pid = g.next_pid++;
    p->name = strdup(name ? name : "(anonymous)");
    p->argc = argc;
    p->argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) p->argv[i] = strdup(argv[i]);
    p->entry = entry;
    p->state = PROC_RUNNING;
    p->exit_code = -1;
    p->exit_requested = 0;
    p->aux_threads = 0;
    pthread_cond_init(&p->aux_done, NULL);

    struct trampoline_arg *t = malloc(sizeof(*t));
    t->proc = p;
    if (pthread_create(&p->main_thread, NULL, main_trampoline, t)) {
        free(t);
        free_slot_locked(p);
        pthread_mutex_unlock(&g.lock);
        return PPROC_INVALID_PID;
    }
    g.live++;
    pproc_pid_t pid = p->pid;
    pthread_mutex_unlock(&g.lock);
    return pid;
}

pproc_pid_t pproc_getpid(void)
{
    struct pproc *p = g.initialized ? pthread_getspecific(g.current) : NULL;
    return p ? p->pid : PPROC_INVALID_PID;
}

const char *pproc_getname(void)
{
    struct pproc *p = g.initialized ? pthread_getspecific(g.current) : NULL;
    return p ? p->name : NULL;
}

int pproc_thread_create(void *(*fn)(void *), void *arg)
{
    struct pproc *p = pthread_getspecific(g.current);
    if (!p || !fn) return -1;

    struct trampoline_arg *t = malloc(sizeof(*t));
    t->proc = p;
    t->aux_fn = fn;
    t->aux_arg = arg;

    pthread_mutex_lock(&g.lock);
    p->aux_threads++;
    pthread_mutex_unlock(&g.lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, aux_trampoline, t)) {
        pthread_mutex_lock(&g.lock);
        if (--p->aux_threads == 0) pthread_cond_broadcast(&p->aux_done);
        pthread_mutex_unlock(&g.lock);
        free(t);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

void pproc_exit(int code)
{
    struct pproc *p = pthread_getspecific(g.current);
    if (!p) pthread_exit(NULL);

    pthread_mutex_lock(&g.lock);
    if (!p->exit_requested) {
        p->exit_requested = 1;
        p->exit_code = code;
    }
    int is_main = pthread_equal(pthread_self(), p->main_thread);
    if (is_main) {
        /*
         * pthread_exit() unwinds past the trampoline epilogue, so the
         * main thread runs it here: drain aux threads, then flip state.
         * Prototype rule: aux entry points return promptly once
         * pproc_exiting() is set; the Wine integration will instead
         * unwind via NtTerminateProcess semantics.
         */
        while (p->aux_threads > 0)
            pthread_cond_wait(&p->aux_done, &g.lock);
        p->state = PROC_EXITED;
        pthread_cond_broadcast(&g.state_changed);
    } else if (p->aux_threads > 0 && --p->aux_threads == 0) {
        pthread_cond_broadcast(&p->aux_done);
    }
    pthread_mutex_unlock(&g.lock);
    pthread_exit(NULL);
}

int pproc_exiting(void)
{
    struct pproc *p = g.initialized ? pthread_getspecific(g.current) : NULL;
    if (!p) return 0;
    pthread_mutex_lock(&g.lock);
    int e = p->exit_requested;
    pthread_mutex_unlock(&g.lock);
    return e;
}

int pproc_wait(pproc_pid_t pid)
{
    if (!g.initialized) return -1;

    pthread_mutex_lock(&g.lock);
    struct pproc *p = find_locked(pid);
    if (!p) {
        pthread_mutex_unlock(&g.lock);
        return -1;
    }
    while (p->state != PROC_EXITED)
        pthread_cond_wait(&g.state_changed, &g.lock);

    pthread_t main_thread = p->main_thread;
    pthread_mutex_unlock(&g.lock);
    pthread_join(main_thread, NULL);

    pthread_mutex_lock(&g.lock);
    int code = p->exit_code;
    free_slot_locked(p);
    g.live--;
    pthread_mutex_unlock(&g.lock);
    return code;
}

int pproc_count(void)
{
    if (!g.initialized) return 0;
    pthread_mutex_lock(&g.lock);
    int n = g.live;
    pthread_mutex_unlock(&g.lock);
    return n;
}
