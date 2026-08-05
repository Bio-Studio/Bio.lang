/*
 * bts.c — BTS (Bio Threads) 线程系统
 *
 * 协作式用户线程（ucontext 实现，无抢占）：
 *   BTS::spawn("方法名", 参数...)  → 创建线程，返回线程 id
 *   BTS::yield()                   → 让出 CPU，调度其他就绪线程
 *   BTS::join(id)                  → 等待线程完成并取回其 res/ref
 *   BTS::active()                  → 存活线程数
 *   BTS::self()                    → 当前线程 id（主线程为 0）
 *
 * 线程执行任意流的裸方法调用（方法名全局搜索）。
 */

#define _XOPEN_SOURCE 700
#include "bio.h"
#include <ucontext.h>
#include <time.h>

typedef struct BThread {
    int id;
    ucontext_t ctx;
    char *stack;
    int state;               /* 0=就绪 1=运行 2=完成 */
    Result *result;
    const char *mname;       /* 要执行的方法名 */
    Value **args; int nargs;
    unsigned char round_ran; /* Taskm：本轮已运行 */
    VarMap area;             /* 线程自己的作用域（跟随 a 解析层） */
    struct BThread *next;
} BThread;

static BThread *threads = NULL;      /* 线程表 */
static BThread *current = NULL;      /* 当前运行的线程（NULL=主线程） */
static ucontext_t sched_ctx;         /* 调度器上下文 */
static int next_id = 1;
static BThread *boot = NULL;         /* makecontext 启动参数 */

Interp *g_interp = NULL;             /* 全局解释器（bts 线程执行方法用） */

/* 线程入口 */
static void bts_entry(void) {
    BThread *t = boot;
    boot = NULL;
    VarMap *saved_area = g_interp->cur_area;
    g_interp->cur_area = &t->area;            /* 线程作用域：跟随 a 指向这里 */
    t->result = interp_call_global(g_interp, t->mname, t->args, t->nargs);
    g_interp->cur_area = saved_area;
    t->state = 2;
    current = NULL;
    swapcontext(&t->ctx, &sched_ctx);   /* 回调度器 */
}

/* 找线程 */
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

/* 调度一轮：跑所有就绪线程直到全部完成或让出后无就绪 */
static void bts_run(void) {
    int pass;
    do {
        pass = 0;
        for (BThread *t = threads; t; t = t->next) {
            if (t->state == 0) {
                pass = 1;
                t->state = 1;
                current = t;
                boot = t;                       /* 入口从这里取线程数据 */
                swapcontext(&sched_ctx, &t->ctx);
                current = NULL;
            }
        }
    } while (pass && bts_alive() > 0);
}

/* 一轮调度：每个就绪线程跑一次（Taskm 用，配合间隔形成时间片轮转） */
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

/* 调度到目标线程完成（join 用） */
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
        if (!ran) break;   /* 无就绪线程且目标未完成 → 死等保护 */
    }
}

/* BTS 内置流方法 */
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
        t->stack = aalloc(65536);
        getcontext(&t->ctx);
        t->ctx.uc_stack.ss_sp = t->stack;
        t->ctx.uc_stack.ss_size = 65536;
        t->ctx.uc_link = NULL;
        makecontext(&t->ctx, bts_entry, 0);   /* 入口数据 boot 在调度时设置 */
        t->next = threads;
        threads = t;
        return mk_res(mk_num((double)t->id));
    }
    if (strcmp(method, "yield") == 0) {
        if (current) {
            current->state = 0;                   /* 线程让出：现场存 t->ctx，切回调度循环 */
            swapcontext(&current->ctx, &sched_ctx);
        } else {
            bts_run();                            /* 主线程让出：直接跑一轮调度循环 */
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


/* ═══════════════ Taskm：任务管理器 ═══════════════
 * Taskm::add("方法名", 参数...)  注册任务（复用 Threads 线程）
 * Taskm::interval(毫秒)         设置轮转间隔（默认 0）
 * Taskm::run()                 自动调度循环：轮流跑所有任务直到完成，轮间等待 interval
 * Taskm::stop()                停止调度循环（线程内调用后让出即可）
 * Taskm::active()              未完成任务数
 */
static int taskm_running = 0;
static long taskm_interval_ns = 0;

static void taskm_sleep(void) {
    if (taskm_interval_ns <= 0) return;
    struct timespec ts;
    ts.tv_sec = taskm_interval_ns / 1000000000L;
    ts.tv_nsec = taskm_interval_ns % 1000000000L;
    nanosleep(&ts, NULL);
}

Result *taskm_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "add") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR)
            return mk_ref("Taskm refused: add requires a method name string (Taskm::add(\"name\", args...))");
        return bts_request("spawn", args, nargs);   /* 复用线程创建 */
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
            bts_round();              /* 每线程跑一次（线程内 yield 会提前换出） */
            taskm_sleep();            /* 轮间间隔 */
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
