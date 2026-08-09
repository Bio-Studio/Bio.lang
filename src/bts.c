/*
 * bts.c — BTS (Bio Threads) thread system
 *
 * Cooperative user threads (ucontext-based, non-preemptive):
 *   BTS::spawn("method", args...)  → create a thread, returns thread id
 *   BTS::yield()                   → yield the CPU, schedule other ready threads
 *   BTS::join(id)                  → wait for the thread to finish and get back its res/ref
 *   BTS::active()                  → number of alive threads
 *   BTS::self()                    → current thread id (main thread is 0)
 *
 * A thread executes a bare method call on any stream (method name is searched globally).
 */

#define _XOPEN_SOURCE 700
#include "bio.h"
#include "platform.h"
#if defined(_WIN32)
/* Windows has no ucontext: cooperative threads use Win32 fibers instead. */
#include <windows.h>
typedef struct BioUCtx {
    LPVOID fiber;
    struct { void *ss_sp; size_t ss_size; } uc_stack;
    void *uc_link;
} BioUCtx;
#define ucontext_t BioUCtx
static LPVOID bio_sched_fiber = NULL;
static void __stdcall bio_fiber_entry(PVOID p) {
    void (*fn)(void) = (void (*)(void))p;
    fn();
}
static void bio_swapcontext(BioUCtx *from, BioUCtx *to) {
    (void)from;
    if (!bio_sched_fiber) bio_sched_fiber = ConvertThreadToFiber(NULL);
    SwitchToFiber(to->fiber ? to->fiber : bio_sched_fiber);
}
#define getcontext(c) ((c)->fiber = NULL)
#define makecontext(c, fn, argc) ((c)->fiber = CreateFiber(BIO_STACK_SIZE, bio_fiber_entry, (void (*)(void))(fn)))
#define swapcontext(a, b) bio_swapcontext((a), (b))
#else
#include <ucontext.h>
#endif
#include <time.h>

typedef struct BThread {
    int id;
    ucontext_t ctx;
    char *stack;
    int state;               /* 0=ready 1=running 2=done */
    Result *result;
    const char *mname;       /* method name to execute */
    Value **args; int nargs;
    unsigned char round_ran; /* Taskm: has already run this round */
    VarMap area;             /* the thread's own scope (followed by the `a` resolution layer) */
    BoothArena *booths;      /* this thread's @call phone booths (one per method) */
    struct BThread *next;
} BThread;

static BThread *threads = NULL;      /* thread table */
static BThread *current = NULL;      /* currently running thread (NULL = main thread) */
static ucontext_t sched_ctx;         /* scheduler context */
static int next_id = 1;
static BThread *boot = NULL;         /* makecontext startup parameter */

Interp *g_interp = NULL;             /* global interpreter (used by bts threads to execute methods) */

/* thread entry point */
static void bts_entry(void) {
    BThread *t = boot;
    boot = NULL;
    VarMap *saved_area = g_interp->cur_area;
    g_interp->cur_area = &t->area;            /* thread scope: `a` resolution points here */
    t->result = interp_call_global(g_interp, t->mname, t->args, t->nargs);
    g_interp->cur_area = saved_area;
    booth_free_list(t->booths);               /* the thread's phone booths die with it */
    t->booths = NULL;
    t->state = 2;
    current = NULL;
    swapcontext(&t->ctx, &sched_ctx);   /* back to scheduler */
}

/* The calling thread's @call booth list (NULL for the main thread — the
 * interpreter keeps the main thread's booths in Interp::main_booths). */
BoothArena **bts_current_booth_list(void) {
    return current ? &current->booths : NULL;
}

/* find a thread */
static BThread *bts_find(int id) {
    for (BThread *t = threads; t; t = t->next)
        if (t->id == id) return t;
    return NULL;
}

static int bts_alive(void) {
    int n = 0;
    for (BThread *t = threads; t; t = t->next)
        if (t->state != 2) n++;
    return n;
}

/* run one scheduling pass: run all ready threads until all are done or none is ready after yields */
static void bts_run(void) {
    int pass;
    do {
        pass = 0;
        for (BThread *t = threads; t; t = t->next) {
            if (t->state == 0) {
                pass = 1;
                t->state = 1;
                current = t;
                boot = t;                       /* entry point takes thread data from here */
                swapcontext(&sched_ctx, &t->ctx);
                current = NULL;
            }
        }
    } while (pass && bts_alive() > 0);
}

/* one scheduling round: run each ready thread once (used by Taskm, combined with the interval to form round-robin time slicing) */
static void bts_round(void) {
    for (BThread *t = threads; t; t = t->next) t->round_ran = 0;
    for (BThread *t = threads; t; t = t->next) {
        if (t->state == 0 && !t->round_ran) {
            t->round_ran = 1;
            t->state = 1;
            current = t;
            boot = t;
            swapcontext(&sched_ctx, &t->ctx);
            current = NULL;
        }
    }
}

/* schedule until the target thread finishes (used by join) */
static void bts_run_until(BThread *target) {
    while (target->state != 2 && bts_alive() > 0) {
        int ran = 0;
        for (BThread *t = threads; t; t = t->next) {
            if (t->state == 0) {
                ran = 1;
                t->state = 1;
                current = t;
                boot = t;
                swapcontext(&sched_ctx, &t->ctx);
                current = NULL;
            }
        }
        if (!ran) break;   /* no ready threads and the target isn't done → guard against deadlock */
    }
}

/* BTS built-in stream methods */
Result *bts_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "spawn") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR)
            return mk_ref("BTS refused: spawn requires a method name string (BTS::spawn(\"name\", args...))");
        BThread *t = aalloc(sizeof(BThread));
        t->id = next_id++;
        t->state = 0;
        t->result = NULL;
        t->mname = args[0]->str;
        t->nargs = nargs - 1;
        t->args = aalloc(sizeof(Value *) * (t->nargs > 0 ? t->nargs : 1));
        for (int i = 1; i < nargs; i++) t->args[i - 1] = args[i];
        memset(&t->area, 0, sizeof(VarMap));
        t->area.is_area = 1;
        t->area.parent = &g_interp->globals;
        t->booths = NULL;
        t->stack = aalloc(BIO_STACK_SIZE);
        getcontext(&t->ctx);
        t->ctx.uc_stack.ss_sp = t->stack;
        t->ctx.uc_stack.ss_size = BIO_STACK_SIZE;
        t->ctx.uc_link = NULL;
        makecontext(&t->ctx, bts_entry, 0);   /* entry data (boot) is set at scheduling time */
        t->next = threads;
        threads = t;
        return mk_res(mk_num((double)t->id));
    }
    if (strcmp(method, "yield") == 0) {
        if (current) {
            current->state = 0;                   /* thread yields: context is saved in t->ctx, switch back to the scheduling loop */
            swapcontext(&current->ctx, &sched_ctx);
        } else {
            bts_run();                            /* main thread yields: just run one scheduling pass */
        }
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "join") == 0) {
        if (nargs < 1 || args[0]->kind != V_NUM)
            return mk_ref("BTS refused: join requires a thread id");
        BThread *t = bts_find((int)args[0]->num);
        if (!t) return mk_ref("BTS refused: thread does not exist");
        if (t->state != 2) {
            if (current) return mk_ref("BTS refused: join inside a thread on an unfinished target not supported yet");
            bts_run_until(t);
        }
        if (!t->result) return mk_ref("BTS refused: thread has no result");
        if (t->result->ref) return mk_ref(t->result->ref);
        return mk_res(t->result->res);
    }
    if (strcmp(method, "active") == 0)
        return mk_res(mk_num((double)bts_alive()));
    if (strcmp(method, "self") == 0)
        return mk_res(mk_num((double)(current ? current->id : 0)));
    return mk_ref("BTS refused: no such method (spawn/yield/join/active/self)");
}


/* ═══════════════ Taskm: task manager ═══════════════
 * Taskm::add("method", args...)  register a task (reuses Threads threads)
 * Taskm::interval(ms)            set the round-robin interval (default 0)
 * Taskm::run()                   automatic scheduling loop: run all tasks in rotation until done, waiting for interval between rounds
 * Taskm::stop()                  stop the scheduling loop (call inside a thread, then yield)
 * Taskm::active()                number of unfinished tasks
 */
static int taskm_running = 0;
static long taskm_interval_ns = 0;

static void taskm_sleep(void) {
    if (taskm_interval_ns <= 0) return;
    bio_sleep_ms((double)taskm_interval_ns / 1e6);
}

Result *taskm_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "add") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR)
            return mk_ref("Taskm refused: add requires a method name string (Taskm::add(\"name\", args...))");
        return bts_request("spawn", args, nargs);   /* reuse thread creation */
    }
    if (strcmp(method, "interval") == 0) {
        if (nargs < 1 || args[0]->kind != V_NUM)
            return mk_ref("Taskm refused: interval requires milliseconds");
        taskm_interval_ns = (long)(args[0]->num * 1000000.0);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "run") == 0) {
        if (taskm_running) return mk_ref("Taskm refused: scheduler loop already running");
        taskm_running = 1;
        while (taskm_running && bts_alive() > 0) {
            bts_round();              /* run each thread once (a yield inside a thread swaps out early) */
            taskm_sleep();            /* interval between rounds */
        }
        taskm_running = 0;
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "stop") == 0) {
        taskm_running = 0;
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "active") == 0)
        return mk_res(mk_num((double)bts_alive()));
    return mk_ref("Taskm refused: no such method (add/interval/run/stop/active)");
}
