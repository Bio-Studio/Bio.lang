#include "bio.h"

/* ═══════════════ 内置流 ═══════════════
 * CIO —— 控制台（Console）
 * FIO —— 文件
 * SIO —— 字符串
 * IO  —— 父流，聚合以上全部（旧写法 IO::println 仍可用）
 */


static Value *arg(Value **args, int n, int i) { return i < n ? args[i] : NULL; }
static const char *arg_str(Value **args, int n, int i, const char *dflt) {
    Value *v = arg(args, n, i);
    if (!v) return dflt;
    if (v->kind == V_STR) return v->str;
    if (v->kind == V_NUM) { static char b[32]; snprintf(b, sizeof b, "%g", v->num); return b; }
    return dflt;
}

/* ---------- CIO：控制台 ---------- */
static Result *cio_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "println") == 0) {
        for (int i = 0; i < nargs; i++) { if (i) printf(" "); print_value(args[i]); }
        printf("\n");
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "print") == 0) {
        for (int i = 0; i < nargs; i++) print_value(args[i]);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "error") == 0) {
        for (int i = 0; i < nargs; i++) fprintf(stderr, "%s", arg_str(args, nargs, i, ""));
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "read") == 0 || strcmp(method, "readInt") == 0 ||
        strcmp(method, "readNumber") == 0) {
        char buf[512];
        if (nargs > 0) print_value(args[0]);
        if (!fgets(buf, sizeof buf, stdin)) buf[0] = 0;
        size_t len = strlen(buf);
        while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
        char *end;
        if (strcmp(method, "readInt") == 0) {
            long v = strtol(buf, &end, 10);
            if (end == buf || *end) return mk_ref("CIO 拒绝：readInt 输入不是整数");
            return mk_res(mk_num((double)v));
        }
        if (strcmp(method, "readNumber") == 0) {
            double v = strtod(buf, &end);
            if (end == buf || *end) return mk_ref("CIO 拒绝：readNumber 输入不是数字");
            return mk_res(mk_num(v));
        }
        return mk_res(mk_str(astrdup(buf)));
    }
    return mk_ref("CIO 拒绝：没有该方法");
}

/* ---------- FIO：文件 ---------- */
static Result *fio_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "readFile") == 0) {
        const char *path = arg_str(args, nargs, 0, "");
        FILE *f = fopen(path, "r");
        if (!f) return mk_ref("FIO 拒绝：无法打开文件（不存在或无权限）");
        char buf[4096]; size_t total = 0; char *out = aalloc(1); out[0] = 0;
        while (fgets(buf, sizeof buf, f)) {
            size_t l = strlen(buf);
            char *n2 = aalloc(total + l + 1);
            memcpy(n2, out, total); memcpy(n2 + total, buf, l + 1);
            out = n2; total += l;
        }
        fclose(f);
        return mk_res(mk_str(out));
    }
    if (strcmp(method, "writeFile") == 0 || strcmp(method, "appendFile") == 0) {
        const char *path = arg_str(args, nargs, 0, "");
        const char *content = arg_str(args, nargs, 1, "");
        FILE *f = fopen(path, strcmp(method, "writeFile") == 0 ? "w" : "a");
        if (!f) return mk_ref("FIO 拒绝：无法写入文件");
        fputs(content, f);
        fclose(f);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "exists") == 0) {
        const char *path = arg_str(args, nargs, 0, "");
        FILE *f = fopen(path, "r");
        if (f) { fclose(f); return mk_res(mk_num(1)); }
        return mk_res(mk_num(0));
    }
    return mk_ref("FIO 拒绝：没有该方法");
}

/* ---------- SIO：字符串 ---------- */
static char *s_dup_range(const char *s, int start, int end) {
    int len = (int)strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end || start >= len) return astrdup("");
    char *r = aalloc(end - start + 1);
    memcpy(r, s + start, end - start); r[end - start] = 0;
    return r;
}

static Result *sio_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "format") == 0) {   /* 手写 %d %i %s %f 替换 */
        const char *fmt = arg_str(args, nargs, 0, "");
        char out[2048]; int oi = 0; int ai = 1;
        for (int i = 0; fmt[i] && oi < 2040; i++) {
            if (fmt[i] == '%' && fmt[i+1] && strchr("disfx", fmt[i+1]) && ai < nargs) {
                char c = fmt[++i];
                Value *v = args[ai++];
                if (c == 's') { const char *s = arg_str(args, nargs, ai-1, ""); oi += snprintf(out+oi, 2048-oi, "%s", s); }
                else if (c == 'f') oi += snprintf(out+oi, 2048-oi, "%g", v->kind == V_NUM ? v->num : 0);
                else oi += snprintf(out+oi, 2048-oi, "%ld", v->kind == V_NUM ? (long)v->num : 0);
            } else {
                out[oi++] = fmt[i];
            }
        }
        out[oi] = 0;
        return mk_res(mk_str(astrdup(out)));
    }
    if (strcmp(method, "length") == 0) {
        return mk_res(mk_num((double)strlen(arg_str(args, nargs, 0, ""))));
    }
    if (strcmp(method, "upper") == 0) {
        const char *s = arg_str(args, nargs, 0, ""); char *r = astrdup(s);
        for (char *p = r; *p; p++) *p = (char)toupper((unsigned char)*p);
        return mk_res(mk_str(r));
    }
    if (strcmp(method, "lower") == 0) {
        const char *s = arg_str(args, nargs, 0, ""); char *r = astrdup(s);
        for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
        return mk_res(mk_str(r));
    }
    if (strcmp(method, "trim") == 0) {
        const char *s = arg_str(args, nargs, 0, "");
        while (*s && isspace((unsigned char)*s)) s++;
        size_t l = strlen(s);
        while (l && isspace((unsigned char)s[l-1])) l--;
        return mk_res(mk_str(s_dup_range(s, 0, (int)l)));
    }
    if (strcmp(method, "contains") == 0) {
        const char *s = arg_str(args, nargs, 0, "");
        const char *sub = arg_str(args, nargs, 1, "");
        return mk_res(mk_num(strstr(s, sub) ? 1 : 0));
    }
    if (strcmp(method, "substring") == 0) {
        const char *s = arg_str(args, nargs, 0, "");
        int a = arg(args, nargs, 1) && arg(args, nargs, 1)->kind == V_NUM ? (int)arg(args, nargs, 1)->num : 0;
        int b = arg(args, nargs, 2) && arg(args, nargs, 2)->kind == V_NUM ? (int)arg(args, nargs, 2)->num : (int)strlen(s);
        return mk_res(mk_str(s_dup_range(s, a, b)));
    }
    if (strcmp(method, "replace") == 0) {
        const char *s = arg_str(args, nargs, 0, "");
        const char *old = arg_str(args, nargs, 1, "");
        const char *neu = arg_str(args, nargs, 2, "");
        if (!*old) return mk_res(mk_str(astrdup(s)));
        char out[2048]; int oi = 0; size_t ol = strlen(old);
        for (const char *p = s; *p && oi < 2000; ) {
            if (strncmp(p, old, ol) == 0) {
                oi += snprintf(out+oi, 2048-oi, "%s", neu); p += ol;
            } else out[oi++] = *p++;
        }
        out[oi] = 0;
        return mk_res(mk_str(astrdup(out)));
    }
    return mk_ref("SIO 拒绝：没有该方法");
}

/* ---------- Array：数组流 ---------- */
/* 数组参数解包：裸 V_ARR 或 Result 包装（数组对象即 V_ARR） */
/* 数组对象解包：裸 V_ARR / Result 包装 / 数组类对象（data 字段 = Solid 连续流） */
static Value *arr_unwrap(Value *v) {
    if (v->kind == V_ARR) return v;
    if (v->kind == V_RES && v->res && !v->res->ref) v = v->res->res;
    if (v && v->kind == V_OBJ && v->obj_fields) {
        Value *d = var_get_layer(v->obj_fields, "data");
        if (d && d->kind == V_ARR) return d;
    }
    return NULL;
}

/* Array/Vector 实例注册进 Arrays（由 Array/Vector 类的 __init__ 调用，Bio 代码可见） */
static void arrays_register(Value *a) {
    Interp *in = g_interp;
    if (in->arrays->len >= in->arrays->cap) {
        int ncap = in->arrays->cap * 2;
        Value **ni = aalloc(sizeof(Value *) * (size_t)ncap);
        memcpy(ni, in->arrays->items, sizeof(Value *) * (size_t)in->arrays->len);
        in->arrays->items = ni; in->arrays->cap = ncap;
    }
    in->arrays->items[in->arrays->len++] = a;
}

/* ---------- Solid：Solidstream 连续流（连续存储 + 自动分配 + 移动头指针） ---------- */
Result *solid_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "new") == 0) return mk_res(mk_arr(0));
    Value *s = arr_unwrap(nargs > 0 ? args[0] : NULL);
    if (!s) return mk_ref("Solid 拒绝：第一个参数必须是连续流（Solid::new 创建）");
    if (strcmp(method, "len") == 0) return mk_res(mk_num((double)(s->len - s->head)));
    if (strcmp(method, "get") == 0) {
        int i = (nargs > 1 && args[1]->kind == V_NUM) ? (int)args[1]->num : -1;
        if (i < 0 || s->head + i >= s->len) return mk_ref("Solid 拒绝：索引越界");
        return mk_res(s->items[s->head + i]);
    }
    if (strcmp(method, "set") == 0) {
        int i = (nargs > 1 && args[1]->kind == V_NUM) ? (int)args[1]->num : -1;
        if (i < 0 || s->head + i >= s->len) return mk_ref("Solid 拒绝：索引越界");
        s->items[s->head + i] = args[2];
        return mk_res(s);
    }
    if (strcmp(method, "push") == 0) {
        if (s->len >= s->cap) {
            int ncap = s->cap * 2;
            Value **ni = aalloc(sizeof(Value *) * (size_t)ncap);
            memcpy(ni, s->items, sizeof(Value *) * (size_t)s->len);
            s->items = ni; s->cap = ncap;
        }
        s->items[s->len++] = args[1];
        return mk_res(s);
    }
    if (strcmp(method, "pop") == 0) {
        if (s->len <= s->head) return mk_ref("Solid 拒绝：连续流为空");
        return mk_res(s->items[--s->len]);
    }
    if (strcmp(method, "read") == 0) {          /* 移动头指针：读一个并前进 */
        if (s->head >= s->len) return mk_ref("Solid 拒绝：连续流为空");
        return mk_res(s->items[s->head++]);
    }
    if (strcmp(method, "peek") == 0) {
        if (s->head >= s->len) return mk_ref("Solid 拒绝：连续流为空");
        return mk_res(s->items[s->head]);
    }
    if (strcmp(method, "head") == 0) return mk_res(mk_num((double)s->head));
    if (strcmp(method, "resetHead") == 0) { s->head = 0; return mk_res(s); }
    if (strcmp(method, "clear") == 0) { s->len = 0; s->head = 0; return mk_res(s); }
    if (strcmp(method, "join") == 0) {
        const char *sep = (nargs > 1 && args[1]->kind == V_STR) ? args[1]->str : ",";
        char out[2048]; int oi = 0;
        for (int i = s->head; i < s->len && oi < 2000; i++) {
            if (i > s->head) oi += snprintf(out + oi, 2048 - oi, "%s", sep);
            if (s->items[i]->kind == V_NUM)
                oi += snprintf(out + oi, 2048 - oi, "%g", s->items[i]->num);
            else if (s->items[i]->kind == V_STR)
                oi += snprintf(out + oi, 2048 - oi, "%s", s->items[i]->str);
        }
        out[oi] = 0;
        return mk_res(mk_str(astrdup(out)));
    }
    return mk_ref("Solid 拒绝：没有该方法（new/len/get/set/push/pop/read/peek/head/resetHead/clear/join）");
}

/* ---------- Arrays：Array 集合流（包含 Array/Vector 实例） ---------- */
Result *arrays_request(const char *method, Value **args, int nargs) {
    Interp *in = g_interp;
    if (strcmp(method, "count") == 0) return mk_res(mk_num((double)in->arrays->len));
    if (strcmp(method, "all") == 0) return mk_res(in->arrays);
    if (strcmp(method, "get") == 0) {
        int i = (nargs > 0 && args[0]->kind == V_NUM) ? (int)args[0]->num : -1;
        if (i < 0 || i >= in->arrays->len) return mk_ref("Arrays 拒绝：索引越界");
        return mk_res(in->arrays->items[i]);
    }
    if (strcmp(method, "add") == 0) {           /* 注册（Array/Vector __init__ 调用） */
        Value *o = nargs > 0 ? args[0] : NULL;
        if (o && o->kind == V_RES && o->res && !o->res->ref) o = o->res->res;
        if (!o || o->kind != V_OBJ || !var_get_layer(o->obj_fields, "data"))
            return mk_ref("Arrays 拒绝：add 需要数组对象");
        arrays_register(o);
        return mk_res(o);
    }
    if (strcmp(method, "vector") == 0) {        /* 动态数组：new Vector()（Bio 类） */
        Value *cls = mk_str("Vector");
        return obj_request("new", &cls, 1);
    }
    if (strcmp(method, "forget") == 0) {  /* 从注册表移除 */
        Value *o = nargs > 0 ? args[0] : NULL;
        if (o && o->kind == V_RES && o->res && !o->res->ref) o = o->res->res;
        if (!o || o->kind != V_OBJ) return mk_ref("Arrays 拒绝：需要数组对象");
        for (int i = 0; i < in->arrays->len; i++) {
            if (in->arrays->items[i] == o) {
                in->arrays->items[i] = in->arrays->items[in->arrays->len - 1];
                in->arrays->len--;
                return mk_res(mk_num(1));
            }
        }
        return mk_ref("Arrays 拒绝：数组不在注册表中");
    }
    return mk_ref("Arrays 拒绝：没有该方法（count/all/get/add/vector/forget）");
}

/* ---------- Obj：Objstream 对象流 ---------- */
Decl *find_class(Interp *in, const char *name) {
    for (Decl *d = in->decls; d; d = d->next)
        if (d->kind == D_CLASS && strcmp(d->name, name) == 0) return d;
    return NULL;
}
Method *class_method(Decl *cls, const char *mname) {
    for (int i = 0; i < cls->nmethods; i++)
        if (strcmp(cls->methods[i].name, mname) == 0) return &cls->methods[i];
    return NULL;
}
Value *mk_obj(const char *cls) {
    Value *v = aalloc(sizeof(Value));
    v->kind = V_OBJ;
    v->obj_cls = cls;
    v->obj_fields = aalloc(sizeof(VarMap));
    memset(v->obj_fields, 0, sizeof(VarMap));
    return v;
}

/* new 的结果是 V_RES 包对象（请求模型），对象 API 统一解包（数组也是对象） */
static Value *obj_unwrap(Value *v) {
    if (v->kind == V_OBJ || v->kind == V_ARR) return v;
    if (v->kind == V_RES && v->res && !v->res->ref && v->res->res &&
        (v->res->res->kind == V_OBJ || v->res->res->kind == V_ARR))
        return v->res->res;
    return NULL;
}

Result *obj_request(const char *method, Value **args, int nargs) {
    Interp *in = g_interp;
    if (strcmp(method, "new") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR) return mk_ref("Obj 拒绝：new 需要类名");
        Decl *cls = find_class(in, args[0]->str);
        if (!cls) return mk_ref("Obj 拒绝：没有这个类（需先 Class 声明）");
        Value *o = mk_obj(args[0]->str);
        o->obj_fields->parent = in->cur_area;
        /* 对象也是流，储存各种属性：把类声明的字段物化到实例（默认值） */
        for (int i = 0; i < cls->nfields; i++)
            var_set(o->obj_fields, cls->fields[i].name, field_default(cls->fields[i].type));
        Method *init = class_method(cls, "__init__");
        if (init) {
            Result *r = interp_exec_method(in, init, args + 1, nargs - 1, o->obj_fields, o);
            if (r->ref && strcmp(r->ref, NOTHING) != 0) {   /* ref(无)=隐式完成 */
                char buf[256];
                snprintf(buf, sizeof buf, "Obj 拒绝：__init__ 被拒绝（%s）", r->ref);
                return mk_ref(astrdup(buf));
            }
        }
        return mk_res(o);
    }
    Value *o = obj_unwrap(args[0]);
    if (!o) return mk_ref("Obj 拒绝：需要对象（Obj::new / new 创建）");
    if (strcmp(method, "get") == 0) {
        if (nargs < 2 || args[1]->kind != V_STR) return mk_ref("Obj 拒绝：get 需要属性名");
        Value *f = var_get_layer(o->obj_fields, args[1]->str);
        if (!f) {
            char buf[256];
            snprintf(buf, sizeof buf, "Obj 拒绝：属性 %s 被冲走了", args[1]->str);
            return mk_ref(astrdup(buf));
        }
        return mk_res(f);
    }
    if (strcmp(method, "set") == 0) {
        if (nargs < 2 || args[1]->kind != V_STR) return mk_ref("Obj 拒绝：set 需要（属性名, 值）");
        var_set(o->obj_fields, args[1]->str, args[2]);
        return mk_res(o);
    }
    if (strcmp(method, "forget") == 0) {
        if (nargs < 2 || args[1]->kind != V_STR) return mk_ref("Obj 拒绝：forget 需要属性名");
        var_del(o->obj_fields, args[1]->str);      /* 属性被冲走 */
        return mk_res(o);
    }
    if (strcmp(method, "call") == 0) {
        if (nargs < 2 || args[1]->kind != V_STR) return mk_ref("Obj 拒绝：call 需要方法名");
        const char *clsname = o->kind == V_ARR ? "Array" : o->obj_cls;
        Decl *cls = find_class(in, clsname);
        Method *m = cls ? class_method(cls, args[1]->str) : NULL;
        if (!m) {
            char buf[256];
            snprintf(buf, sizeof buf, "Obj 拒绝：类 %s 没有方法 %s", clsname, args[1]->str);
            return mk_ref(astrdup(buf));
        }
        if (o->kind == V_OBJ) o->obj_fields->parent = in->cur_area;
        Result *r = interp_exec_method(in, m, args + 2, nargs - 2, o->kind == V_OBJ ? o->obj_fields : in->cur_area, o);
        if (r->ref && strcmp(r->ref, NOTHING) != 0) return mk_ref(r->ref);
        if (r->ref) return mk_res(mk_str(""));      /* 隐式完成 → 成功无值 */
        return mk_res(r->res);
    }
    if (strcmp(method, "class") == 0) return mk_res(mk_str(o->obj_cls));
    return mk_ref("Obj 拒绝：没有该方法（new/get/set/forget/call/class）");
}

/* ---------- Time：Timestream 计时流 ----------
 * 原稿：一个 Timestream 可以同时拥有多个计时器；默认第一个计时器归线程所有，
 * 不允许归零；分叉出的（第二个、第三个……）允许归零。
 */
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct { int id; double start; int forked; } Timer;
static Timer timers[64];
static int ntimer = 0;
static int next_timer_id = 1000;   /* fork 分叉计时器的独立 id（避开线程 id） */

static Timer *timer_find(int id) {
    for (int i = 0; i < ntimer; i++)
        if (timers[i].id == id) return &timers[i];
    return NULL;
}

/* 拿到/新建计时器：first=1 表示线程默认首个计时器（不允许归零），否则视为分叉 */
static Timer *timer_obtain(int id, int first) {
    Timer *t = timer_find(id);
    if (t) return t;
    if (ntimer < 64) {
        timers[ntimer].id = id;
        timers[ntimer].start = now_sec();
        timers[ntimer].forked = !first;
        return &timers[ntimer++];
    }
    return NULL;
}

Result *time_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "now") == 0) return mk_res(mk_num(now_sec()));
    if (strcmp(method, "sleep") == 0) {
        double ms = (nargs > 0 && args[0]->kind == V_NUM) ? args[0]->num : 0;
        struct timespec ts;
        ts.tv_sec = (time_t)(ms / 1000.0);
        ts.tv_nsec = (long)((ms - ts.tv_sec * 1000.0) * 1e6);
        nanosleep(&ts, NULL);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "start") == 0) {
        /* 默认第一个计时器归线程所有：无参数时用当前线程 id */
        int id = (nargs > 0 && args[0]->kind == V_NUM) ? (int)args[0]->num
                 : (int)bts_request("self", NULL, 0)->res->num;
        int first = nargs < 1 || args[0]->kind != V_NUM;
        if (!timer_obtain(id, first)) return mk_ref("Time 拒绝：计时器已满");
        return mk_res(mk_num((double)id));
    }
    if (strcmp(method, "fork") == 0) {
        /* 分叉新计时器：独立 id，允许归零（第一个并不允许，分叉出的允许） */
        Timer *t = timer_obtain(next_timer_id++, 0);
        if (!t) return mk_ref("Time 拒绝：计时器已满");
        return mk_res(mk_num((double)t->id));
    }
    if (strcmp(method, "elapsed") == 0) {
        /* 默认第一个计时器归线程所有：无参数时用当前线程 id */
        int id = (nargs > 0 && args[0]->kind == V_NUM) ? (int)args[0]->num
                 : (int)bts_request("self", NULL, 0)->res->num;
        Timer *t = timer_obtain(id, nargs < 1 || args[0]->kind != V_NUM);
        if (!t) return mk_ref("Time 拒绝：计时器已满");
        return mk_res(mk_num(now_sec() - t->start));
    }
    if (strcmp(method, "reset") == 0) {
        /* 原稿：默认第一个计时器归线程所有，不允许归零；分叉出的允许归零 */
        if (nargs < 1 || args[0]->kind != V_NUM)
            return mk_ref("Time 拒绝：第一个计时器（线程默认）不允许归零，请 fork 分叉计时器");
        int id = (int)args[0]->num;
        Timer *t = timer_find(id);
        if (!t) return mk_ref("Time 拒绝：没有这个计时器（Time::start / Time::fork 创建）");
        if (!t->forked)
            return mk_ref("Time 拒绝：第一个计时器（线程默认）不允许归零，请用 Time::fork() 分叉");
        t->start = now_sec();
        return mk_res(mk_str(""));
    }
    return mk_ref("Time 拒绝：没有该方法（now/sleep/start/fork/elapsed/reset）");
}

/* ---------- Rem：Remstream 记忆流（默认持久化到内存，可 save/load） ---------- */
typedef struct { const char *key; Value *val; } Mem;
static Mem mems[256];
static int nmem = 0;

Result *rem_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "save") == 0) {
        if (nargs < 2 || args[0]->kind != V_STR) return mk_ref("Rem 拒绝：save 需要（名字, 值）");
        for (int i = 0; i < nmem; i++)
            if (strcmp(mems[i].key, args[0]->str) == 0) { mems[i].val = args[1]; return mk_res(mk_str("")); }
        if (nmem < 256) { mems[nmem].key = args[0]->str; mems[nmem].val = args[1]; nmem++; return mk_res(mk_str("")); }
        return mk_ref("Rem 拒绝：记忆已满");
    }
    if (strcmp(method, "load") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR) return mk_ref("Rem 拒绝：load 需要名字");
        for (int i = 0; i < nmem; i++)
            if (strcmp(mems[i].key, args[0]->str) == 0) return mk_res(mems[i].val);
        return mk_ref("Rem 拒绝：没有这条记忆");
    }
    if (strcmp(method, "forget") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR) return mk_ref("Rem 拒绝：forget 需要名字");
        for (int i = 0; i < nmem; i++)
            if (strcmp(mems[i].key, args[0]->str) == 0) {
                mems[i] = mems[--nmem]; return mk_res(mk_str(""));
            }
        return mk_ref("Rem 拒绝：没有这条记忆");
    }
    return mk_ref("Rem 拒绝：没有该方法（save/load/forget）");
}

/* ---------- Const：Constantstream 常量流（公共常量，设后不可改） ---------- */
static Mem consts[128];
static int nconst = 0;

Result *const_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "set") == 0) {
        if (nargs < 2 || args[0]->kind != V_STR) return mk_ref("Const 拒绝：set 需要（名字, 值）");
        for (int i = 0; i < nconst; i++)
            if (strcmp(consts[i].key, args[0]->str) == 0)
                return mk_ref("Const 拒绝：常量已存在，不可覆盖");
        if (nconst < 128) { consts[nconst].key = args[0]->str; consts[nconst].val = args[1]; nconst++; return mk_res(mk_str("")); }
        return mk_ref("Const 拒绝：常量表已满");
    }
    if (strcmp(method, "get") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR) return mk_ref("Const 拒绝：get 需要名字");
        for (int i = 0; i < nconst; i++)
            if (strcmp(consts[i].key, args[0]->str) == 0) return mk_res(consts[i].val);
        return mk_ref("Const 拒绝：没有这个常量");
    }
    return mk_ref("Const 拒绝：没有该方法（set/get）");
}

/* ---------- Ref：智能引用（&权限 跟随 真名） ---------- */
/* 跟随层（原稿）：u 程序级(Unistream) / f 方法级(Functionstream) / a 作用域级(Areastream) */
VarMap *ref_layer_get(Interp *in, const char *follow) {
    if (strcmp(follow, "u") == 0) return &in->globals;
    if (strcmp(follow, "a") == 0) return in->cur_area;
    return in->cur_scope;                                    /* f = 当前方法 */
}

Result *ref_request(const char *method, Value **args, int nargs) {
    if (nargs < 1 || args[0]->kind != V_REF)
        return mk_ref("Ref 拒绝：需要引用对象（&权限 跟随 真名）");
    Value *ref = args[0];
    Interp *in = g_interp;
    const char *perm = ref->ref_perm;
    const char *follow = ref->ref_follow;
    if (!(strcmp(perm, "r") == 0 || strcmp(perm, "w") == 0 ||
          strcmp(perm, "rw") == 0 || strcmp(perm, "m") == 0))
        return mk_ref("Ref 拒绝：非法引用权限（应为 r/w/rw/m）");
    if (!(strcmp(follow, "u") == 0 || strcmp(follow, "f") == 0 || strcmp(follow, "a") == 0))
        return mk_ref("Ref 拒绝：非法引用跟随（应为 u/f/a）");
    VarMap *layer = ref_layer_get(in, follow);

    if (strcmp(method, "read") == 0) {
        if (strcmp(perm, "w") == 0)
            return mk_ref("Ref 拒绝：引用是只写权限，不能读");
        Value *v = var_get_layer(layer, ref->ref_name);
        if (!v) {
            char buf[256];
            snprintf(buf, sizeof buf, "Ref 拒绝：目标 %s 不存在（%s 层）", ref->ref_name, follow);
            return mk_ref(astrdup(buf));
        }
        return mk_res(v);
    }
    if (strcmp(method, "write") == 0) {
        if (strcmp(perm, "r") == 0)
            return mk_ref("Ref 拒绝：引用是只读权限，不能写");
        if (nargs < 2) return mk_ref("Ref 拒绝：write 需要值参数");
        var_set(layer, ref->ref_name, args[1]);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "move") == 0) {
        /* 可移动（m 权限）：从所在层取走目标并返回其值 */
        if (strcmp(perm, "m") != 0)
            return mk_ref("Ref 拒绝：move 需要可移动权限（m）");
        Value *v = var_get_layer(layer, ref->ref_name);
        if (!v) {
            char buf[256];
            snprintf(buf, sizeof buf, "Ref 拒绝：目标 %s 不存在（%s 层）", ref->ref_name, follow);
            return mk_ref(astrdup(buf));
        }
        var_del(layer, ref->ref_name);           /* 取走（冲走） */
        return mk_res(v);
    }
    if (strcmp(method, "target") == 0)
        return mk_res(mk_str(ref->ref_name));
    if (strcmp(method, "perm") == 0)
        return mk_res(mk_str(perm));
    return mk_ref("Ref 拒绝：没有该方法（read/write/move/target/perm）");
}

/* ---------- 二进制库流（B_BIN）：dlsym 调用 ---------- */
#include <dlfcn.h>

/* 二进制函数统一按 double(*)(double,...) 调用（x86-64 SysV：double 参数走 xmm0-7，返回值 xmm0） */
typedef double (*bin_fn)(double, double, double, double, double, double);

Result *bin_request(Stream *s, const char *method, Value **args, int nargs) {
    if (!s->dl) return mk_ref("二进制流拒绝：库未加载");
    bin_fn fn = (bin_fn)dlsym(s->dl, method);
    if (!fn) return mk_ref("二进制流拒绝：库中没有该符号（或不是函数）");
    if (nargs > 6) return mk_ref("二进制流拒绝：最多支持 6 个参数");
    double a[6] = {0};
    for (int i = 0; i < nargs; i++) {
        if (args[i]->kind == V_NUM) a[i] = args[i]->num;
        else return mk_ref("二进制流拒绝：二进制函数暂只支持数值参数");
    }
    double r = fn(a[0], a[1], a[2], a[3], a[4], a[5]);
    return mk_res(mk_num(r));
}

/* 全局二进制库表（&func() 全局调用用） */
typedef struct BinLib { Stream *s; struct BinLib *next; } BinLib;
static BinLib *binlibs = NULL;
void binlib_register(Stream *s) {
    BinLib *b = aalloc(sizeof(BinLib));
    b->s = s; b->next = binlibs; binlibs = b;
}
/* 全局搜 &func()：第一个提供该符号的库 */
Result *bin_call_global(const char *method, Value **args, int nargs) {
    for (BinLib *b = binlibs; b; b = b->next) {
        Result *r = bin_request(b->s, method, args, nargs);
        if (!r->ref) return r;              /* 找到并成功 */
        if (strcmp(r->ref, "二进制流拒绝：库中没有该符号（或不是函数）") != 0)
            return r;                       /* 找到符号但调用失败 */
    }
    return mk_ref("二进制调用拒绝：没有库提供函数 &func");
}

/* ---------- Com：Comstream 计算流（瞬时流的一个分支，处理各种瞬时计算） ---------- */
#include <math.h>

static double arg_num(Value **args, int n, int i, double dflt) {
    Value *v = i < n ? args[i] : NULL;
    if (v && v->kind == V_NUM) return v->num;
    if (v && v->kind == V_STR) return strtod(v->str, NULL);
    return dflt;
}

static Result *com_request(const char *method, Value **args, int nargs) {
    double x = arg_num(args, nargs, 0, 0);
    double y = arg_num(args, nargs, 1, 0);
    if (strcmp(method, "abs") == 0) return mk_res(mk_num(fabs(x)));
    if (strcmp(method, "min") == 0) return mk_res(mk_num(x < y ? x : y));
    if (strcmp(method, "max") == 0) return mk_res(mk_num(x > y ? x : y));
    if (strcmp(method, "pow") == 0) return mk_res(mk_num(pow(x, y)));
    if (strcmp(method, "sqrt") == 0) {
        if (x < 0) return mk_ref("Com 拒绝：sqrt 的实参不能为负");
        return mk_res(mk_num(sqrt(x)));
    }
    if (strcmp(method, "floor") == 0) return mk_res(mk_num(floor(x)));
    if (strcmp(method, "ceil") == 0) return mk_res(mk_num(ceil(x)));
    if (strcmp(method, "round") == 0) return mk_res(mk_num(round(x)));
    if (strcmp(method, "sign") == 0) return mk_res(mk_num(x > 0 ? 1 : (x < 0 ? -1 : 0)));
    if (strcmp(method, "sin") == 0) return mk_res(mk_num(sin(x)));
    if (strcmp(method, "cos") == 0) return mk_res(mk_num(cos(x)));
    if (strcmp(method, "tan") == 0) return mk_res(mk_num(tan(x)));
    if (strcmp(method, "log") == 0) {
        if (x <= 0) return mk_ref("Com 拒绝：log 的实参必须为正");
        return mk_res(mk_num(log(x)));
    }
    if (strcmp(method, "exp") == 0) return mk_res(mk_num(exp(x)));
    return mk_ref("Com 拒绝：没有该方法（abs/min/max/pow/sqrt/floor/ceil/round/sign/sin/cos/tan/log/exp）");
}

/* ---------- IO：IOStream（默认存在，可以读取和输出） ----------
 * 父流，聚合 CIO（控制台）/ FIO（文件）/ SIO（字符串），按序尝试分派 */
static Result *io_request(const char *method, Value **args, int nargs) {
    Result *r = cio_request(method, args, nargs);
    if (!r->ref) return r;
    r = fio_request(method, args, nargs);
    if (!r->ref) return r;
    r = sio_request(method, args, nargs);
    if (!r->ref) return r;
    return mk_ref("IO 拒绝：没有该方法（聚合 CIO/FIO/SIO）");
}

Result *builtin_request(int kind, const char *method, Value **args, int nargs) {
    switch (kind) {
        case B_CIO: return cio_request(method, args, nargs);
        case B_FIO: return fio_request(method, args, nargs);
        case B_SIO: return sio_request(method, args, nargs);
        case B_SOLID: return solid_request(method, args, nargs);
        case B_ARRAYS: return arrays_request(method, args, nargs);
        case B_BTS: return bts_request(method, args, nargs);
        case B_TASK: return taskm_request(method, args, nargs);
        case B_REF: return ref_request(method, args, nargs);
        case B_TIME: return time_request(method, args, nargs);
        case B_REM: return rem_request(method, args, nargs);
        case B_CONST: return const_request(method, args, nargs);
        case B_OBJ: return obj_request(method, args, nargs);
        case B_COM: return com_request(method, args, nargs);
        case B_IO: return io_request(method, args, nargs);
        default:
            return mk_ref("未知内置流");
    }
}
