#include "bio.h"
#include <dlfcn.h>

/* Interpreter */
void var_set(VarMap *m, const char *name, Value *v) {
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->names[i], name) == 0) { m->vals[i] = v; return; }
    if (m->n < 256) { m->names[m->n] = name; m->vals[m->n] = v; m->n++; }
}

/* Delete a field (Objstream property washed away) */
void var_del(VarMap *m, const char *name) {
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->names[i], name) == 0) {
            m->names[i] = m->names[m->n - 1];
            m->vals[i] = m->vals[m->n - 1];
            m->n--;
            return;
        }
}

/* Single-layer lookup (smart reference: search only the layer it follows, not up the chain) */
Value *var_get_layer(VarMap *m, const char *name) {
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->names[i], name) == 0) return m->vals[i];
    return NULL;
}

/* Search up the scope chain (method → area → globals) */
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
    in->main_area.parent = &in->globals;   /* Chain: method → fields → area → globals → consts */
    in->globals.parent = &in->consts;      /* Constantstream public constants at the very top, visible globally */
    in->cur_area = &in->main_area;
    in->arrays = mk_arr(16);   /* Arrays registry: all Array/Vector instances */
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

/* Forward declarations */
Value *eval_expr(Interp *in, Node *e, VarMap *scope);

void exec_stmt(Interp *in, Node *st, VarMap *scope, Flow *fl);

void exec_stmts(Interp *in, Node **stmts, int n, VarMap *scope, Flow *fl);

/* Execute a class method (Objstream: Obj::call / __init__ for new) */
Result *interp_exec_method(Interp *in, Method *m, Value **args, int nargs, VarMap *parent, Value *self) {
    if (m->nparams != nargs) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s requires %d arguments, got %d", m->name, m->nparams, nargs);
        return mk_ref(astrdup(buf));
    }
    if (m->builtin) {
        /* Builtin class method: generic dispatch, self as the first argument */
        Value **all = aalloc(sizeof(Value *) * (size_t)(nargs + 1));
        all[0] = self;
        for (int i = 0; i < nargs; i++) all[i + 1] = args[i];
        return builtin_request(m->builtin, m->name, all, nargs + 1);
    }
    VarMap scope;
    memset(&scope, 0, sizeof(VarMap));
    if (self && (self->kind == V_OBJ || (self->kind == V_ARR && self->obj_fields))) {
        /* Scope chain: method → object/stream fields → area → globals (internal bare property reads) */
        scope.parent = self->obj_fields;
        self->obj_fields->parent = parent;
    } else {
        scope.parent = parent;
    }
    if (self) var_set(&scope, "this", self);   /* Inside an object method, this = the object itself */
    for (int i = 0; i < nargs; i++) {
        Value *a = args[i];
        /* Smart-reference argument pointing to a stream: &r f CIO → target CIO is a stream → bind as a stream reference so io::println works inside the method */
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
    /* Current stream: reverse-lookup of the this object (class/stream name; array object → Array class), for bare-call priority + internal properties */
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
        /* Signature-stream method missing → fall back first to the signature stream's implementation stream (D_FORK sig == s->name);
         * if still not found, search any user stream (to avoid accidentally hitting builtin Vector::get and the like) */
        for (Decl *d = in->decls; d; d = d->next) {
            if (d->kind == D_FORK && strcmp(d->sig, s->name) == 0) {
                Stream *impl = stream_find(in, d->name);
                if (impl && (m = method_find(impl, method))) { s = impl; break; }
            }
        }
        if (!m) {
            for (Stream *t = in->streams; t; t = t->next)
                if (t != s && !t->builtin && method_find(t, method)) { s = t; m = method_find(t, method); break; }
        }
    }
    if (!m) {
        char buf[256];
        snprintf(buf, sizeof buf, "stream %s refuses: no method %s", s->name, method);
        return mk_ref(astrdup(buf));
    }
    /* User stream method: this = the stream object (properties stored in the stream's fields), scope chain method → stream fields → area */
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
            /* Streams are first-class values too: bare names like CIO/IO/... resolve to a stream reference (streams passed as arguments) */
            Stream *s = stream_find(in, e->name);
            if (s) return mk_streamref(s);
            char buf[256];
            snprintf(buf, sizeof buf, "refused: variable %s does not exist (washed away?)", e->name);
            return mk_refval(astrdup(buf));
        }
        case N_PROP: {
            Value *base = eval_expr(in, e->l, scope);
            if (base->kind == V_OBJ || (base->kind == V_ARR && base->obj_fields)) {
                /* Object property access: obj.hp (array objects can also have properties) */
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
                    /* The ALL structure holds both res and cause; .cause is the proper name, .ref is a compatibility alias */
                    if (base->res->ref) return mk_str(base->res->ref);
                    return mk_str("(no cause)");
                }
                /* Pass-through: a successful Result holding an object/array object → property access (a new result can be used directly as h.hp) */
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
            /* res X takes the result / cause X takes the refusal reason (draft prefix extraction operator) */
            Value *v = eval_expr(in, e->l, scope);
            int want_cause = strcmp(e->op, "cause") == 0;
            if (is_rejected(v)) {
                if (want_cause) return mk_str(reject_reason(v));   /* Rejected → refusal reason */
                return v;                                            /* res on a rejected value → refusal propagation */
            }
            if (v->kind == V_RES && v->res) {
                if (want_cause) {
                    if (v->res->ref) return mk_str(v->res->ref);
                    return mk_str("(no cause)");
                }
                if (v->res->ref) return v;                          /* Refusal propagation */
                return v->res->res;
            }
            /* Not a Result: res is itself, cause is none */
            if (want_cause) return mk_str("(no cause)");
            return v;
        }
        case N_INDEX: {
            /* Array index read: a[i] (Array instances are backed by a Solid contiguous stream) */
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
                    /* Stream passed as an argument: qual is a V_STREAM variable → call the method on that stream */
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
                    /* Object stream call: the variable is an object/array → call its class method (this bound to the object) */
                    Value *ov = var_get(scope, e->qual);
                    if (ov && (ov->kind == V_OBJ || ov->kind == V_ARR ||
                               (ov->kind == V_RES && ov->res && !ov->res->ref &&
                                ov->res->res && (ov->res->res->kind == V_OBJ || ov->res->res->kind == V_ARR)))) {
                        if (ov->kind == V_RES) ov = ov->res->res;   /* Unwrap */
                        const char *clsname = ov->kind == V_ARR ? "Array" : ov->obj_cls;
                        Decl *cls = find_class(in, clsname);
                        Method *m = cls ? class_method(cls, e->mname) : NULL;
                        if (!m) {
                            /* Non-Class stream (e.g. Main main program stream): when this binds a stream object, resolve from stream methods */
                            Stream *ts = stream_find(in, clsname);
                            if (ts) m = method_find(ts, e->mname);
                        }
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
                        if (r->ref) w2->res = mk_res(mk_str(""));   /* Implicit completion → success with no value */
                        else w2->res = mk_res(r->res);
                        return w2;
                    }
                    char buf[256];
                    snprintf(buf, sizeof buf, "refused: stream %s does not exist", e->qual);
                    return mk_refval(astrdup(buf));
                }
            } else {
                /* Bare call: first check the current stream's own methods (internal calls need no ::), then search globally */
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
            /* Auto-unwrap successful Results (add(a) + add(b) usable directly) */
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
            /* Reference variable declaration (draft realme): name &permission follow [type] [= initial value] */
            Value *ref = mk_refobj(st->ref_perm, st->ref_follow, st->name);
            if (st->init) {
                Value *iv = eval_expr(in, st->init, scope);
                if (is_rejected(iv)) { fl->ret = mk_ref(reject_reason(iv)); break; }
                var_set(ref_layer_get(in, st->ref_follow), st->name, iv);   /* Write the initial value into the followed layer */
            }
            var_set(scope, st->name, ref);
            break;
        }
        case N_ASSIGN: {
            Value *v = eval_expr(in, st->expr, scope);
            if (st->is_const) {
                /* const modifier: add to Constantstream, reject redeclaration */
                if (var_get_layer(&in->consts, st->name)) {
                    fl->ret = mk_ref("refused: constant already declared");
                    break;
                }
                var_set(&in->consts, st->name, v);
                break;
            }
            if (st->is_thread) {
                /* thread modifier: thread variable → current thread scope */
                var_set(in->cur_area, st->name, v);
                break;
            }
            /* Compound assignment += / -= / *= / /= / %=: target old value OP v */
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
                    /* a[i] += v: read the array element's old value */
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
                /* RHS also unwrapped: in sum += a[j], a[j] may be a Result wrapper */
                if (v->kind == V_RES && v->res && !v->res->ref) v = v->res->res;
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
                /* Property assignment: this::base = v → write an object/stream field */
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
                /* Array index assignment: a[i] = v → write to the Array's underlying Solid contiguous stream */
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
                /* Constants are read-only: refuse modification */
                fl->ret = mk_ref("refused: constant is read-only (const)");
                break;
            }
            /* Object-stream bare-name assignment (generic): if the this object already stores this property → write to the instance field.
             * This way, in __init__(x int) { x = x; }, when x is a declared field, the bare assignment writes the instance property,
             * while reads still prefer the argument. Applies to class instances and stream objects. */
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
                /* Multiple res values → array */
                Value *arr = mk_arr(st->nrets);
                for (int i = 0; i < st->nrets; i++)
                    arr->items[arr->len++] = eval_expr(in, st->rets[i], scope);
                fl->ret = mk_res(arr);
            } else {
                Value *v = eval_expr(in, st->expr, scope);
                if (strcmp(st->retkind, "res") == 0) {
                    /* res result; unwrap: the responded value is a successful Result → take its inner res */
                    if (v->kind == V_RES && v->res && !v->res->ref && v->res->res)
                        v = v->res->res;
                    fl->ret = mk_res(v);
                } else {
                    /* cause result; unwrap: the responded value is a rejected Result → forward its refusal reason */
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
                if (inner.brk) break;              /* continue → re-evaluate the condition */
            }
            fl->brk = 0;                            /* Consume the break */
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

/* ═══════════════ Build + assumption check + run ═══════════════ */
void build(Interp *in) {
    /* Prebuilt Bio code: the Array/Vector classes are implemented in the Bio language (draft: not in the interpreter), calling the Solid contiguous stream underneath */
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

    /* Builtin streams: CIO (console) / FIO (file) / SIO (string) / IO (parent stream) / Com (computation stream)
     * / Solid (contiguous stream) / Arrays (array collection) / Time (timing stream) / Rem (memory stream) + Console fork */
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
    stream_add(in, stream_new("Com", B_COM));       /* Comstream computation stream */
    stream_add(in, stream_new("IO", B_IO));         /* IOStream parent stream: aggregates CIO/FIO/SIO */
    stream_add(in, stream_new("Console", B_CIO));   /* CIO's prebuilt fork */
    for (Decl *d = in->decls; d; d = d->next) {
        if (d->kind == D_BIN) {
            /* Binary library stream: loaded via dlopen, exported functions automatically become stream methods */
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
            /* Inherit the signature stream's declared fields + its own fields (fork streams can customize), materialized with default values */
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
            /* Classes are also streams: methods (including those with return types) are registered as stream methods */
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
            /* Main is also a stream (main program stream): its methods participate in the global bare-call search */
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
            /* Top-level const int x = 10; → Constantstream public constant */
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

    /* Main is also a stream: this is available (main program stream fields), bare calls search globally */
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

/* Global bare call: search for the first user stream providing the method */
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

