#include "bio.h"
#include <dlfcn.h>

/* 解释器 */
void var_set(VarMap *m, const char *name, Value *v) {
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->names[i], name) == 0) { m->vals[i] = v; return; }
    if (m->n < 256) { m->names[m->n] = name; m->vals[m->n] = v; m->n++; }
}

/* 删除字段（Objstream 属性被冲走） */
void var_del(VarMap *m, const char *name) {
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->names[i], name) == 0) {
            m->names[i] = m->names[m->n - 1];
            m->vals[i] = m->vals[m->n - 1];
            m->n--;
            return;
        }
}

/* 单层查找（智能引用：跟随哪层就在哪层找，不沿链） */
Value *var_get_layer(VarMap *m, const char *name) {
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->names[i], name) == 0) return m->vals[i];
    return NULL;
}

/* 沿作用域链查找（方法 → area → 全局） */
Value *var_get(VarMap *m, const char *name) {
    for (VarMap *s = m; s; s = s->parent) {
        for (int i = 0; i < s->n; i++)
            if (strcmp(s->names[i], name) == 0) return s->vals[i];
    }
    return NULL;
}

Interp *interp_new(Decl *decls) {
    Interp *in = aalloc(sizeof(Interp));
    in->decls = decls;
    in->streams = NULL;
    memset(&in->globals, 0, sizeof(VarMap));
    memset(&in->consts, 0, sizeof(VarMap));
    memset(&in->main_area, 0, sizeof(VarMap));
    in->main_area.is_area = 1;
    in->main_area.parent = &in->globals;   /* 链：方法 → 字段 → area → globals → consts */
    in->globals.parent = &in->consts;      /* Constantstream 公共常量在最顶，全局可见 */
    in->cur_area = &in->main_area;
    in->arrays = mk_arr(16);   /* Arrays 注册表：所有 Array/Vector 实例 */
    return in;
}

Stream *stream_new(const char *name, int builtin) {
    Stream *s = aalloc(sizeof(Stream));
    s->name = name; s->builtin = builtin; s->methods = NULL; s->next = NULL;
    s->fields = aalloc(sizeof(VarMap));
    memset(s->fields, 0, sizeof(VarMap));
    return s;
}

void stream_add(Interp *in, Stream *s) { s->next = in->streams; in->streams = s; }

Stream *stream_find(Interp *in, const char *name) {
    for (Stream *s = in->streams; s; s = s->next)
        if (strcmp(s->name, name) == 0) return s;
    return NULL;
}

Method *method_find(Stream *s, const char *name) {
    for (MethodEntry *e = s->methods; e; e = e->next)
        if (strcmp(e->m->name, name) == 0) return e->m;
    return NULL;
}

/* 前向声明 */
Value *eval_expr(Interp *in, Node *e, VarMap *scope);

void exec_stmt(Interp *in, Node *st, VarMap *scope, Flow *fl);

void exec_stmts(Interp *in, Node **stmts, int n, VarMap *scope, Flow *fl);

/* 执行类方法（Objstream：Obj::call / new 的 __init__） */
Result *interp_exec_method(Interp *in, Method *m, Value **args, int nargs, VarMap *parent, Value *self) {
    if (m->nparams != nargs) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s requires %d arguments, got %d", m->name, m->nparams, nargs);
        return mk_ref(astrdup(buf));
    }
    if (m->builtin) {
        /* 内置类方法：通用分派，self 作为第一个参数 */
        Value **all = aalloc(sizeof(Value *) * (size_t)(nargs + 1));
        all[0] = self;
        for (int i = 0; i < nargs; i++) all[i + 1] = args[i];
        return builtin_request(m->builtin, m->name, all, nargs + 1);
    }
    VarMap scope;
    memset(&scope, 0, sizeof(VarMap));
    if (self && (self->kind == V_OBJ || (self->kind == V_ARR && self->obj_fields))) {
        /* 作用域链：方法 → 对象/流字段 → area → globals（内部裸读属性） */
        scope.parent = self->obj_fields;
        self->obj_fields->parent = parent;
    } else {
        scope.parent = parent;
    }
    if (self) var_set(&scope, "this", self);   /* 对象方法里 this = 对象本身 */
    for (int i = 0; i < nargs; i++) {
        Value *a = args[i];
        /* 智能引用参数指向流：&r f CIO → 目标 CIO 是流 → 绑定为流引用，方法内可 io::println */
        if (a && a->kind == V_REF) {
            Stream *ts = stream_find(in, a->ref_name);
            if (ts) { a = mk_streamref(ts); goto bind; }
        }
        if (a && a->kind == V_RES && a->res && !a->res->ref && a->res->res &&
            a->res->res->kind == V_REF) {
            Stream *ts = stream_find(in, a->res->res->ref_name);
            if (ts) { a = mk_streamref(ts); goto bind; }
        }
    bind:
        var_set(&scope, m->params[i], a);
    }
    VarMap *saved_scope = in->cur_scope;
    Stream *saved_stream = in->cur_stream;
    in->cur_scope = &scope;
    /* 当前流：this 对象反查（类/流名；数组对象 → Array 类），供裸调用优先 + 内部属性 */
    in->cur_stream = self ? stream_find(in, self->kind == V_ARR ? "Array" : self->obj_cls) : NULL;
    Flow fl = {0};
    exec_stmts(in, m->stmts, m->nstmts, &scope, &fl);
    in->cur_scope = saved_scope;
    in->cur_stream = saved_stream;
    return fl.ret ? fl.ret : mk_ref(NOTHING);
}

Result *stream_request(Interp *in, Stream *s, const char *method, Value **args, int nargs) {
    if (s->builtin == B_BIN) return bin_request(s, method, args, nargs);
    if (s->builtin) return builtin_request(s->builtin, method, args, nargs);
    Method *m = method_find(s, method);
    if (!m) {
        char buf[256];
        snprintf(buf, sizeof buf, "stream %s refuses: no method %s", s->name, method);
        return mk_ref(astrdup(buf));
    }
    /* 用户流方法：this = 流对象（属性存流字段），作用域链 方法 → 流字段 → area */
    Value *self = mk_obj(s->name);
    self->obj_fields = s->fields;
    Result *r = interp_exec_method(in, m, args, nargs, in->cur_area, self);
    return r->ref && strcmp(r->ref, NOTHING) == 0 ? mk_ref(NOTHING) : r;
}

Value *eval_expr(Interp *in, Node *e, VarMap *scope) {
    switch (e->kind) {
        case N_NUM: return mk_num(e->num);
        case N_STR: return mk_str(e->str);
        case N_VAR: {
            Value *v = var_get(scope, e->name);
            if (v) return v;
            /* 流也是一等值：CIO/IO/... 裸名可解析为流引用（流作为参数传递） */
            Stream *s = stream_find(in, e->name);
            if (s) return mk_streamref(s);
            char buf[256];
            snprintf(buf, sizeof buf, "refused: variable %s does not exist (washed away?)", e->name);
            return mk_refval(astrdup(buf));
        }
        case N_PROP: {
            Value *base = eval_expr(in, e->l, scope);
            if (base->kind == V_OBJ || (base->kind == V_ARR && base->obj_fields)) {
                /* 对象属性访问：obj.hp（数组对象也可有属性） */
                Value *f = var_get_layer(base->obj_fields, e->name);
                if (f) return f;
                char buf[256];
                snprintf(buf, sizeof buf, "refused: property %s was washed away", e->name);
                return mk_refval(astrdup(buf));
            }
            if (base->kind == V_RES) {
                if (strcmp(e->name, "res") == 0) {
                    if (base->res->ref) {
                        char buf[256];
                        snprintf(buf, sizeof buf, "refused: request refused, cannot take res (cause: %s)", base->res->ref);
                        return mk_refval(astrdup(buf));
                    }
                    return base->res->res;
                }
                if (strcmp(e->name, "cause") == 0 || strcmp(e->name, "ref") == 0) {
                    /* ALL 结构含 res 与 cause；.cause 合规，.ref 为兼容别名 */
                    if (base->res->ref) return mk_str(base->res->ref);
                    return mk_str("(no cause)");
                }
                /* 透传：成功 Result 里是对象/数组对象 → 属性访问（new 结果可直接 h.hp） */
                if (!base->res->ref && base->res->res &&
                    (base->res->res->kind == V_OBJ ||
                     (base->res->res->kind == V_ARR && base->res->res->obj_fields))) {
                    Value *f = var_get_layer(base->res->res->obj_fields, e->name);
                    if (f) return f;
                    char buf[256];
                    snprintf(buf, sizeof buf, "refused: property %s was washed away", e->name);
                    return mk_refval(astrdup(buf));
                }
                char buf[256];
                snprintf(buf, sizeof buf, "refused: Result has no property %s", e->name);
                return mk_refval(astrdup(buf));
            }
            char buf[256];
            snprintf(buf, sizeof buf, "refused: cannot access %s (property washed away)", e->name);
            return mk_refval(astrdup(buf));
        }
        case N_REF:
            return mk_refobj(e->ref_perm, e->ref_follow, e->ref_name);
        case N_UNWRAP: {
            /* res X 取结果 / cause X 取拒绝原因（原稿前缀提取运算符） */
            Value *v = eval_expr(in, e->l, scope);
            int want_cause = strcmp(e->op, "cause") == 0;
            if (is_rejected(v)) {
                if (want_cause) return mk_str(reject_reason(v));   /* 被拒 → 拒绝原因 */
                return v;                                            /* res 被拒值 → 拒绝传播 */
            }
            if (v->kind == V_RES && v->res) {
                if (want_cause) {
                    if (v->res->ref) return mk_str(v->res->ref);
                    return mk_str("(no cause)");
                }
                if (v->res->ref) return v;                          /* 拒绝传播 */
                return v->res->res;
            }
            /* 非 Result：res 即自身，cause 为无 */
            if (want_cause) return mk_str("(no cause)");
            return v;
        }
        case N_INDEX: {
            /* 数组下标读取: a[i]（Array 实例底层是 Solid 连续流） */
            Value *base = eval_expr(in, e->l, scope);
            if (base->kind == V_RES && base->res && !base->res->ref && base->res->res)
                base = base->res->res;
            Value *data = base && (base->kind == V_OBJ || base->kind == V_ARR) && base->obj_fields
                ? var_get_layer(base->obj_fields, "data") : NULL;
            if (data && data->kind == V_ARR) {
                Value *iv = eval_expr(in, e->r, scope);
                if (iv->kind == V_RES && iv->res && !iv->res->ref) iv = iv->res->res;
                if (iv->kind != V_NUM) return mk_refval("refused: array index requires a number");
                int i = (int)iv->num;
                if (i < 0 || data->head + i >= data->len) return mk_refval("refused: array index out of bounds");
                return data->items[data->head + i];
            }
            return mk_refval("refused: cannot index a non-array");
        }
        case N_BINCALL: {
            Value **vals = aalloc(sizeof(Value *) * 64);
            int nvals = 0;
            for (int i = 0; i < e->nargs; i++) {
                Value *v = eval_expr(in, e->args[i], scope);
                if (is_rejected(v)) {
                    char buf[256];
                    snprintf(buf, sizeof buf, "refused: argument %s refused, binary function &%s also refused",
                             reject_reason(v), e->mname);
                    return mk_refval(astrdup(buf));
                }
                vals[nvals++] = v;
            }
            Result *r = bin_call_global(e->mname, vals, nvals);
            Value *w = aalloc(sizeof(Value));
            w->kind = V_RES; w->res = r;
            return w;
        }
        case N_CALL: {
            Value **vals = aalloc(sizeof(Value *) * 64);
            int nvals = 0;
            for (int i = 0; i < e->nargs; i++) {
                Value *v = eval_expr(in, e->args[i], scope);
                if (is_rejected(v)) {
                    char buf[256];
                    if (e->qual)
                        snprintf(buf, sizeof buf, "refused: argument %s refused, request %s::%s also refused",
                                 reject_reason(v), e->qual, e->mname);
                    else
                        snprintf(buf, sizeof buf, "refused: argument %s refused, function %s also refused",
                                 reject_reason(v), e->mname);
                    return mk_refval(astrdup(buf));
                }
                vals[nvals++] = v;
            }
            Stream *s;
            if (e->qual) {
                s = stream_find(in, e->qual);
                if (!s) {
                    /* 流作为参数传递：qual 是 V_STREAM 变量 → 调用该流的方法 */
                    Value *sv = var_get(scope, e->qual);
                    if (sv && (sv->kind == V_STREAM || (sv->kind == V_RES && !sv->res->ref &&
                                sv->res->res && sv->res->res->kind == V_STREAM))) {
                        if (sv->kind == V_RES) sv = sv->res->res;
                        if (sv->stream_ref) {
                            Result *r2 = stream_request(in, sv->stream_ref, e->mname, vals, nvals);
                            Value *w3 = aalloc(sizeof(Value));
                            w3->kind = V_RES; w3->res = r2;
                            return w3;
                        }
                    }
                    /* 对象流调用: 变量是对象/数组 → 调其类方法（this 绑定对象） */
                    Value *ov = var_get(scope, e->qual);
                    if (ov && (ov->kind == V_OBJ || ov->kind == V_ARR ||
                               (ov->kind == V_RES && ov->res && !ov->res->ref &&
                                ov->res->res && (ov->res->res->kind == V_OBJ || ov->res->res->kind == V_ARR)))) {
                        if (ov->kind == V_RES) ov = ov->res->res;   /* 解包 */
                        const char *clsname = ov->kind == V_ARR ? "Array" : ov->obj_cls;
                        Decl *cls = find_class(in, clsname);
                        Method *m = cls ? class_method(cls, e->mname) : NULL;
                        if (!m) {
                            char buf[256];
                            snprintf(buf, sizeof buf, "refused: class %s has no method %s", clsname, e->mname);
                            return mk_refval(astrdup(buf));
                        }
                        if (ov->kind == V_OBJ || (ov->kind == V_ARR && ov->obj_fields))
                            ov->obj_fields->parent = in->cur_area;
                        Result *r = interp_exec_method(in, m, vals, nvals,
                            ov->kind == V_OBJ || (ov->kind == V_ARR && ov->obj_fields) ? ov->obj_fields : in->cur_area, ov);
                        if (r->ref && strcmp(r->ref, NOTHING) != 0) return mk_refval(r->ref);
                        Value *w2 = aalloc(sizeof(Value));
                        w2->kind = V_RES;
                        if (r->ref) w2->res = mk_res(mk_str(""));   /* 隐式完成 → 成功无值 */
                        else w2->res = mk_res(r->res);
                        return w2;
                    }
                    char buf[256];
                    snprintf(buf, sizeof buf, "refused: stream %s does not exist", e->qual);
                    return mk_refval(astrdup(buf));
                }
            } else {
                /* 裸调用：先查当前流自身方法（内部调用无需 ::），再全局搜索 */
                s = NULL;
                if (in->cur_stream && method_find(in->cur_stream, e->mname))
                    s = in->cur_stream;
                if (!s) {
                    for (Stream *t = in->streams; t; t = t->next)
                        if (!t->builtin && method_find(t, e->mname)) { s = t; break; }
                }
                if (!s) {
                    char buf[256];
                    snprintf(buf, sizeof buf, "refused: no function %s (no stream provides it)", e->mname);
                    return mk_refval(astrdup(buf));
                }
            }
            Result *r = stream_request(in, s, e->mname, vals, nvals);
            Value *w = aalloc(sizeof(Value));
            w->kind = V_RES; w->res = r;
            return w;
        }
        case N_BINOP: {
            Value *a = eval_expr(in, e->l, scope);
            if (is_rejected(a)) return a;
            Value *b = eval_expr(in, e->r, scope);
            if (is_rejected(b)) return b;
            /* 自动解包成功 Result（add(a) + add(b) 直接可用） */
            if (a->kind == V_RES && a->res && !a->res->ref && a->res->res) a = a->res->res;
            if (b->kind == V_RES && b->res && !b->res->ref && b->res->res) b = b->res->res;
            const char *op = e->op;
            if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
                double av = a->kind == V_NUM ? a->num : (a->kind == V_STR ? 0 : 0);
                double bv = b->kind == V_NUM ? b->num : 0;
                int eq = av == bv;
                if (a->kind == V_STR && b->kind == V_STR) eq = strcmp(a->str, b->str) == 0;
                return mk_num((strcmp(op, "==") == 0) ? eq : !eq);
            }
            if (a->kind != V_NUM || b->kind != V_NUM) {
                return mk_refval("refused: arithmetic requires numbers");
            }
            double r;
            if (strcmp(op, "+") == 0) r = a->num + b->num;
            else if (strcmp(op, "-") == 0) r = a->num - b->num;
            else if (strcmp(op, "*") == 0) r = a->num * b->num;
            else if (strcmp(op, "/") == 0) {
                if (b->num == 0) return mk_refval("refused: division by zero");
                r = a->num / b->num;
            }
            else if (strcmp(op, "<") == 0) r = a->num < b->num;
            else if (strcmp(op, ">") == 0) r = a->num > b->num;
            else if (strcmp(op, "<=") == 0) r = a->num <= b->num;
            else if (strcmp(op, ">=") == 0) r = a->num >= b->num;
            else return mk_refval("refused: unknown operator");
            return mk_num(r);
        }
        default:
            fprintf(stderr, "internal error: unexpected expression node\n");
            exit(1);
    }
}

void exec_stmt(Interp *in, Node *st, VarMap *scope, Flow *fl) {
    switch (st->kind) {
        case N_REALME: {
            /* 引用变量声明（原稿 realme）：名字 &权限 跟随 [类型] [= 初值] */
            Value *ref = mk_refobj(st->ref_perm, st->ref_follow, st->name);
            if (st->init) {
                Value *iv = eval_expr(in, st->init, scope);
                if (is_rejected(iv)) { fl->ret = mk_ref(reject_reason(iv)); break; }
                var_set(ref_layer_get(in, st->ref_follow), st->name, iv);   /* 初值写入跟随层 */
            }
            var_set(scope, st->name, ref);
            break;
        }
        case N_ASSIGN: {
            Value *v = eval_expr(in, st->expr, scope);
            if (st->is_const) {
                /* const 修饰：加入 Constantstream，重复声明拒绝 */
                if (var_get_layer(&in->consts, st->name)) {
                    fl->ret = mk_ref("refused: constant already declared");
                    break;
                }
                var_set(&in->consts, st->name, v);
                break;
            }
            if (st->is_thread) {
                /* thread 修饰：线程变量 → 当前线程作用域 */
                var_set(in->cur_area, st->name, v);
                break;
            }
            /* 复合赋值 += / -= / *= / /= / %=：目标旧值 运算符 v */
            int is_compound = st->op && strcmp(st->op, "=") != 0;
            if (is_compound) {
                Value *old = NULL;
                if (st->target && st->target->kind == N_PROP) {
                    Value *base = eval_expr(in, st->target->l, scope);
                    if (base->kind == V_RES && base->res && !base->res->ref && base->res->res)
                        base = base->res->res;
                    if (base->kind == V_OBJ || (base->kind == V_ARR && base->obj_fields))
                        old = var_get_layer(base->obj_fields, st->target->name);
                } else if (st->target && st->target->kind == N_INDEX) {
                    /* a[i] += v：读数组元素旧值 */
                    Node *old_idx = aalloc(sizeof(Node));
                    old_idx->kind = N_INDEX;
                    old_idx->l = st->target->l; old_idx->r = st->target->r;
                    Value *ov = eval_expr(in, old_idx, scope);
                    if (is_rejected(ov)) { fl->ret = mk_ref(reject_reason(ov)); break; }
                    old = ov;
                } else {
                    old = var_get(scope, st->name);
                }
                if (!old || (old->kind == V_RES && old->res->ref)) {
                    char buf[256];
                    snprintf(buf, sizeof buf, "refused: %s target %s has no old value", st->op, st->name ? st->name : "property");
                    fl->ret = mk_ref(astrdup(buf)); break;
                }
                if (old->kind == V_RES && !old->res->ref) old = old->res->res;
                if (old->kind != V_NUM || v->kind != V_NUM) {
                    fl->ret = mk_ref("refused: compound assignment requires numbers");
                    break;
                }
                double nv2;
                if (strcmp(st->op, "+=") == 0) nv2 = old->num + v->num;
                else if (strcmp(st->op, "-=") == 0) nv2 = old->num - v->num;
                else if (strcmp(st->op, "*=") == 0) nv2 = old->num * v->num;
                else if (strcmp(st->op, "/=") == 0) { if (v->num == 0) { fl->ret = mk_ref("refused: division by zero"); break; } nv2 = old->num / v->num; }
                else if (strcmp(st->op, "%=") == 0) { if (v->num == 0) { fl->ret = mk_ref("refused: modulo by zero"); break; } nv2 = (double)((long)old->num % (long)v->num); }
                else { fl->ret = mk_ref("refused: unknown compound operator"); break; }
                v = mk_num(nv2);
            }
            if (st->target && st->target->kind == N_PROP) {
                /* 属性赋值: this::base = v → 写对象/流字段 */
                Value *base = eval_expr(in, st->target->l, scope);
                if (base->kind == V_RES && base->res && !base->res->ref && base->res->res)
                    base = base->res->res;
                if (base->kind == V_OBJ || (base->kind == V_ARR && base->obj_fields)) {
                    var_set(base->obj_fields, st->target->name, v);
                    break;
                }
                fl->ret = mk_ref("refused: property assignment target is not an object");
                break;
            }
            if (st->target && st->target->kind == N_INDEX) {
                /* 数组下标赋值: a[i] = v → 写 Array 底层 Solid 连续流 */
                Value *base = eval_expr(in, st->target->l, scope);
                if (base->kind == V_RES && base->res && !base->res->ref && base->res->res)
                    base = base->res->res;
                Value *data = base && (base->kind == V_OBJ || base->kind == V_ARR) && base->obj_fields
                    ? var_get_layer(base->obj_fields, "data") : NULL;
                if (data && data->kind == V_ARR) {
                    Value *iv = eval_expr(in, st->target->r, scope);
                    if (iv->kind == V_RES && iv->res && !iv->res->ref) iv = iv->res->res;
                    if (iv->kind != V_NUM) { fl->ret = mk_ref("refused: array index requires a number"); break; }
                    int i = (int)iv->num;
                    if (i < 0 || data->head + i >= data->len) { fl->ret = mk_ref("refused: array index out of bounds"); break; }
                    data->items[data->head + i] = v;
                    break;
                }
                fl->ret = mk_ref("refused: array index target is not an array");
                break;
            }
            if (st->name && var_get_layer(&in->consts, st->name)) {
                /* 常量只读：修改拒绝 */
                fl->ret = mk_ref("refused: constant is read-only (const)");
                break;
            }
            /* 对象流裸名赋值（通用）：this 对象已储存该属性 → 写入实例字段。
             * 这样 __init__(x int) { x = x; } 里 x 是已声明字段时，裸赋值写实例属性，
             * 而读取仍优先取参数。适用于类实例与流对象。 */
            if (!st->is_const && !st->is_thread) {
                Value *thisv = var_get(scope, "this");
                if (thisv && (thisv->kind == V_OBJ || (thisv->kind == V_ARR && thisv->obj_fields)) &&
                    thisv->obj_fields && var_get_layer(thisv->obj_fields, st->name)) {
                    var_set(thisv->obj_fields, st->name, v);
                    break;
                }
            }
            var_set(scope, st->name, v);
            break;
        }
        case N_CALLSTMT:
            eval_expr(in, st->l, scope);
            break;
        case N_RET: {
            if (strcmp(st->retkind, "res") == 0 && st->nrets > 1) {
                /* res 多值 → 数组 */
                Value *arr = mk_arr(st->nrets);
                for (int i = 0; i < st->nrets; i++)
                    arr->items[arr->len++] = eval_expr(in, st->rets[i], scope);
                fl->ret = mk_res(arr);
            } else {
                Value *v = eval_expr(in, st->expr, scope);
                if (strcmp(st->retkind, "res") == 0) {
                    /* res result; 解包：被回应值是成功 Result → 取其内层 res */
                    if (v->kind == V_RES && v->res && !v->res->ref && v->res->res)
                        v = v->res->res;
                    fl->ret = mk_res(v);
                } else {
                    /* cause result; 解包：被回应值是被拒 Result → 转发其拒绝原因 */
                    if (v->kind == V_RES && v->res && v->res->ref)
                        fl->ret = mk_ref(v->res->ref);
                    else if (v->kind == V_RES && v->res && !v->res->ref)
                        fl->ret = mk_ref("(no cause)");
                    else
                        fl->ret = mk_ref(v->kind == V_STR ? v->str : "refused");
                }
            }
            break;
        }
        case N_IF:
            if (truthy(eval_expr(in, st->cond, scope)))
                exec_stmts(in, st->stmts, st->nstmts, scope, fl);
            else if (st->has_else)
                exec_stmts(in, st->else_stmts, st->n_else, scope, fl);
            break;
        case N_WHILE: {
            while (!fl->brk && truthy(eval_expr(in, st->cond, scope))) {
                Flow inner = {0};
                exec_stmts(in, st->stmts, st->nstmts, scope, &inner);
                if (inner.ret) { fl->ret = inner.ret; return; }
                if (inner.brk) break;              /* continue → 重新判断条件 */
            }
            fl->brk = 0;                            /* 消费 break */
            break;
        }
        case N_FOR: {
            Flow f0 = {0};
            if (st->init) exec_stmt(in, st->init, scope, &f0);
            for (;;) {
                if (st->cond && !truthy(eval_expr(in, st->cond, scope))) break;
                Flow inner = {0};
                exec_stmts(in, st->stmts, st->nstmts, scope, &inner);
                if (inner.ret) { fl->ret = inner.ret; return; }
                if (inner.brk) break;
                if (st->update) { Flow f1 = {0}; exec_stmt(in, st->update, scope, &f1); }
            }
            fl->brk = 0;
            break;
        }
        case N_BREAK:
            fl->brk = 1;
            break;
        case N_CONTINUE:
            fl->cont = 1;
            break;
        default:
            fprintf(stderr, "internal error: unknown statement node\n");
            exit(1);
    }
}

void exec_stmts(Interp *in, Node **stmts, int n, VarMap *scope, Flow *fl) {
    for (int i = 0; i < n && !fl->ret && !fl->brk && !fl->cont; i++)
        exec_stmt(in, stmts[i], scope, fl);
}

/* ═══════════════ 构建 + 假设检查 + 运行 ═══════════════ */
void build(Interp *in) {
    /* 预置 Bio 代码：Array/Vector 类用 Bio 语言实现（原稿：非解释器里），底层调 Solid 连续流 */
    {
        static const char *PREBUILT =
            "Class Array {\n"
            "    void __init__(n int) {\n"
            "        this::data = Solid::new().res;\n"
            "        ALL i = 0;\n"
            "        while (i < n) { Solid::push(this::data, 0); i = i + 1; }\n"
            "        Arrays::add(this);\n"
            "    }\n"
            "    int len() { res Solid::len(this::data).res; }\n"
            "    int get(i int) { res Solid::get(this::data, i).res; }\n"
            "    void set(i int, v) { Solid::set(this::data, i, v); res \"\"; }\n"
            "    void push(v) { Solid::push(this::data, v); res \"\"; }\n"
            "    int pop() { res Solid::pop(this::data).res; }\n"
            "    void clear() { Solid::clear(this::data); res \"\"; }\n"
            "    string join(sep string) { res Solid::join(this::data, sep).res; }\n"
            "}\n"
            "Class Vector {\n"
            "    void __init__() {\n"
            "        this::data = Solid::new().res;\n"
            "        Arrays::add(this);\n"
            "    }\n"
            "    int len() { res Solid::len(this::data).res; }\n"
            "    int get(i int) { res Solid::get(this::data, i).res; }\n"
            "    void set(i int, v) { Solid::set(this::data, i, v); res \"\"; }\n"
            "    void push(v) { Solid::push(this::data, v); res \"\"; }\n"
            "    int pop() { res Solid::pop(this::data).res; }\n"
            "    void clear() { Solid::clear(this::data); res \"\"; }\n"
            "    string join(sep string) { res Solid::join(this::data, sep).res; }\n"
            "}\n";
        int nt2, err2 = 0;
        Tok *toks2 = tokenize(PREBUILT, &nt2);
        Decl *pre = parse_program_tokens(toks2, nt2, &err2);
        if (err2) {
            fprintf(stderr, "⚠️ prebuilt Array/Vector parse failed, arrays unavailable\n");
        } else {
            Decl *tail = in->decls;
            while (tail && tail->next) tail = tail->next;
            if (tail) tail->next = pre; else in->decls = pre;
        }
    }

    /* 内置流：CIO(控制台) / FIO(文件) / SIO(字符串) / IO(父流) / Com(计算流)
     * / Solid(连续流) / Arrays(数组集合) / Time(计时流) / Rem(记忆流) + Console 分叉 */
    stream_add(in, stream_new("Obj", B_OBJ));
    stream_add(in, stream_new("Const", B_CONST));
    stream_add(in, stream_new("Rem", B_REM));
    stream_add(in, stream_new("Time", B_TIME));
    stream_add(in, stream_new("Ref", B_REF));
    stream_add(in, stream_new("Taskm", B_TASK));
    stream_add(in, stream_new("Threads", B_BTS));
    stream_add(in, stream_new("SIO", B_SIO));
    stream_add(in, stream_new("Solid", B_SOLID));
    stream_add(in, stream_new("Arrays", B_ARRAYS));
    stream_add(in, stream_new("FIO", B_FIO));
    stream_add(in, stream_new("CIO", B_CIO));
    stream_add(in, stream_new("Com", B_COM));       /* Comstream 计算流 */
    stream_add(in, stream_new("IO", B_IO));         /* IOStream 父流：聚合 CIO/FIO/SIO */
    stream_add(in, stream_new("Console", B_CIO));   /* CIO 的预置分叉 */
    for (Decl *d = in->decls; d; d = d->next) {
        if (d->kind == D_BIN) {
            /* 二进制库流：dlopen 加载，导出函数自动成为流方法 */
            void *h = dlopen(d->file, RTLD_LAZY | RTLD_GLOBAL);
            if (!h) {
                fprintf(stderr, "⚠️ binary library load failed %s: %s\n", d->file, dlerror());
            }
            Stream *b = aalloc(sizeof(Stream));
            b->name = d->name; b->builtin = B_BIN; b->dl = h; b->methods = NULL; b->next = NULL;
            stream_add(in, b);
            if (h) binlib_register(b);
            continue;
        }
        if (d->kind == D_FORK) {
            Stream *s = stream_new(d->name, 0);
            for (int i = 0; i < d->nmethods; i++) {
                MethodEntry *e = aalloc(sizeof(MethodEntry));
                e->m = &d->methods[i];
                e->next = s->methods;
                s->methods = e;
            }
            /* 继承签名流声明的字段 + 自身字段（分叉流可自定义），以默认值物化 */
            for (Decl *t = in->decls; t; t = t->next)
                if (t->kind == D_SIG && strcmp(t->name, d->sig) == 0) {
                    for (int i = 0; i < t->nfields; i++)
                        var_set(s->fields, t->fields[i].name, field_default(t->fields[i].type));
                    break;
                }
            for (int i = 0; i < d->nfields; i++)
                var_set(s->fields, d->fields[i].name, field_default(d->fields[i].type));
            stream_add(in, s);
        } else if (d->kind == D_CLASS) {
            /* 类也是流：方法（含带返回类型）注册为流方法 */
            Stream *s = stream_new(d->name, 0);
            for (int i = 0; i < d->nmethods; i++) {
                MethodEntry *e = aalloc(sizeof(MethodEntry));
                e->m = &d->methods[i];
                e->next = s->methods;
                s->methods = e;
            }
            for (int i = 0; i < d->nfields; i++)
                var_set(s->fields, d->fields[i].name, field_default(d->fields[i].type));
            stream_add(in, s);
        } else if (d->kind == D_SIG) {
            if (!stream_find(in, d->name)) {
                Stream *s = stream_new(d->name, 0);
                for (int i = 0; i < d->nfields; i++)
                    var_set(s->fields, d->fields[i].name, field_default(d->fields[i].type));
                stream_add(in, s);
            }
        } else if (d->kind == D_MAIN) {
            /* Main 也是流（主程序流）：其方法进入全局裸调用搜索 */
            if (!stream_find(in, "Main")) {
                Stream *s = stream_new("Main", 0);
                for (int i = 0; i < d->nmethods; i++) {
                    MethodEntry *e = aalloc(sizeof(MethodEntry));
                    e->m = &d->methods[i];
                    e->next = s->methods;
                    s->methods = e;
                }
                stream_add(in, s);
            }
        } else if (d->kind == D_CONST) {
            /* 顶层 const int x = 10; → Constantstream 公共常量 */
            VarMap s0;
            memset(&s0, 0, sizeof s0);
            s0.parent = &in->consts;
            Value *v = eval_expr(in, d->init, &s0);
            if (is_rejected(v)) {
                fprintf(stderr, "⛔ constant %s initial value refused: %s\n", d->name, reject_reason(v));
            } else {
                var_set(&in->consts, d->name, v);
            }
        }
    }
}

int check_assumptions(Interp *in) {
    int unmet = 0;
    for (Decl *d = in->decls; d; d = d->next) {
        if (d->kind != D_NEED) continue;
        if (strcmp(d->needkind, "value") == 0) {
            if (!stream_find(in, d->name) && !var_get(&in->globals, d->name)) {
                printf("⛔ assumption unmet: value %s\n", d->name); unmet = 1;
            }
        } else if (strcmp(d->needkind, "function") == 0) {
            int ok = 0;
            for (Stream *s = in->streams; s && !ok; s = s->next)
                if (method_find(s, d->name)) ok = 1;
            if (!ok) { printf("⛔ assumption unmet: function %s\n", d->name); unmet = 1; }
        } else if (strcmp(d->needkind, "Class") == 0) {
            if (!stream_find(in, d->name)) {
                printf("⛔ assumption unmet: Class %s\n", d->name); unmet = 1;
            }
        } else { /* stream */
            if (!stream_find(in, d->name)) {
                printf("⛔ assumption unmet: stream %s\n", d->name); unmet = 1;
            }
        }
    }
    if (unmet) printf("⛔ refusing to run: unmet assumptions exist\n");
    return unmet;
}

void run_program(Interp *in) {
    Decl *main = NULL;
    for (Decl *d = in->decls; d; d = d->next)
        if (d->kind == D_MAIN) { main = d; break; }
    if (!main) { printf("ℹ️ no Main::exec(), no program entry\n"); return; }
    Method *exec = NULL;
    for (int i = 0; i < main->nmethods; i++)
        if (strcmp(main->methods[i].name, "exec") == 0) exec = &main->methods[i];
    if (!exec) { printf("ℹ️ no exec() in Main\n"); return; }

    /* Main 也是流：this 可用（主程序流字段），裸调用全局搜索 */
    VarMap main_fields;
    memset(&main_fields, 0, sizeof(VarMap));
    Value *mself = aalloc(sizeof(Value));
    mself->kind = V_OBJ;
    mself->obj_cls = "Main";
    mself->obj_fields = &main_fields;
    in->cur_area = &in->main_area;
    Result *r = interp_exec_method(in, exec, NULL, 0, &in->main_area, mself);
    if (r->ref && strcmp(r->ref, NOTHING) != 0) printf("⛔ main stream refused: %s\n", r->ref);
}

/* 全局裸调用：搜索第一个提供该方法的用户流 */
Result *interp_call_global(Interp *in, const char *mname, Value **args, int nargs) {
    for (Stream *t = in->streams; t; t = t->next)
        if (!t->builtin && method_find(t, mname))
            return stream_request(in, t, mname, args, nargs);
    return mk_ref("refused: no function (BTS thread target missing)");
}

void run_source(const char *src) {
    int ntok, err = 0;
    Tok *toks = tokenize(src, &ntok);
    Decl *decls = parse_program_tokens(toks, ntok, &err);
    if (err) { fprintf(stderr, "parse failed, program not run\n"); return; }
    Interp *in = interp_new(decls);
    g_interp = in;
    build(in);
    if (check_assumptions(in)) return;
    run_program(in);
}

