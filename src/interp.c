#include "bio.h"
#include "platform.h"
#include <string.h>

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

/* Find the map that owns a variable (used when a reference takes &name). */
VarMap *var_find_map(VarMap *scope, const char *name) {
    for (VarMap *s = scope; s; s = s->parent) {
        for (int i = 0; i < s->n; i++)
            if (strcmp(s->names[i], name) == 0) return s;
    }
    return NULL;
}

/* ---- @onlyread write-method detection ---- */

static int fio_write_method(const char *m) {
    return strcmp(m, "open") == 0 || strcmp(m, "writeFile") == 0 ||
           strcmp(m, "appendFile") == 0 || strcmp(m, "write") == 0 ||
           strcmp(m, "print") == 0 || strcmp(m, "println") == 0;
}

Method *method_find(Stream *s, const char *name);
static int method_writes(Interp *in, Stream *s, Method *m, int depth);

static int node_writes(Interp *in, Node *n, Stream *s, int depth) {
    if (!n || depth > 32) return 0;
    if (n->kind == N_CALL) {
        if (n->qual && strcmp(n->qual, "FIO") == 0 && fio_write_method(n->mname)) return 1;
        if (n->qual && s && strcmp(n->qual, s->name) == 0) {
            Method *mm = method_find(s, n->mname);
            if (mm && method_writes(in, s, mm, depth + 1)) return 1;
        }
    }
    if (node_writes(in, n->l, s, depth + 1)) return 1;
    if (node_writes(in, n->r, s, depth + 1)) return 1;
    if (node_writes(in, n->expr, s, depth + 1)) return 1;
    if (node_writes(in, n->target, s, depth + 1)) return 1;
    if (node_writes(in, n->cond, s, depth + 1)) return 1;
    if (node_writes(in, n->init, s, depth + 1)) return 1;
    if (node_writes(in, n->update, s, depth + 1)) return 1;
    for (int i = 0; i < n->nargs; i++)
        if (node_writes(in, n->args[i], s, depth + 1)) return 1;
    for (int i = 0; i < n->nrets; i++)
        if (node_writes(in, n->rets[i], s, depth + 1)) return 1;
    for (int i = 0; i < n->nstmts; i++)
        if (node_writes(in, n->stmts[i], s, depth + 1)) return 1;
    for (int i = 0; i < n->n_else; i++)
        if (node_writes(in, n->else_stmts[i], s, depth + 1)) return 1;
    return 0;
}

static int method_writes(Interp *in, Stream *s, Method *m, int depth) {
    (void)in;
    if (!m) return 0;
    if (m->write) return 1;
    if (m->read) return 0;
    if (depth > 32) return 0;
    for (int i = 0; i < m->nstmts; i++)
        if (node_writes(in, m->stmts[i], s, depth)) return 1;
    return 0;
}

static int stream_method_is_write(Interp *in, Stream *s, const char *method) {
    Method *m = method_find(s, method);
    /* Explicit @read/@write annotations take priority over heuristics. */
    if (m && (m->write || m->read)) return m->write;
    if ((s->builtin == B_FIO || s->sig_builtin == B_FIO) && fio_write_method(method)) return 1;
    if (m) return method_writes(in, s, m, 0);
    /* Signature streams carry their contract in Decls, not on the stream:
     * check the signature and its fork implementations too. */
    for (Decl *d = in->decls; d; d = d->next) {
        if (d->kind == D_SIG && strcmp(d->name, s->name) == 0) {
            for (int i = 0; i < d->nmethods; i++)
                if (strcmp(d->methods[i].name, method) == 0)
                    return method_writes(in, s, &d->methods[i], 0);
            break;
        }
        if (d->kind == D_FORK && strcmp(d->sig, s->name) == 0) {
            for (int i = 0; i < d->nmethods; i++)
                if (strcmp(d->methods[i].name, method) == 0)
                    return method_writes(in, s, &d->methods[i], 0);
        }
    }
    return 0;
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
    if (in->stream_cache && in->stream_cache_name == name) return in->stream_cache;
    if (in->stream_cache && in->stream_cache_name &&
        strcmp(in->stream_cache_name, name) == 0) return in->stream_cache;
    for (Stream *s = in->streams; s; s = s->next)
        if (strcmp(s->name, name) == 0) {
            in->stream_cache = s;
            in->stream_cache_name = s->name;
            return s;
        }
    return NULL;
}

Method *method_find(Stream *s, const char *name) {
    if (s->method_cache && s->method_cache_name == name) return s->method_cache;
    if (s->method_cache && s->method_cache_name &&
        strcmp(s->method_cache_name, name) == 0) return s->method_cache;
    for (MethodEntry *e = s->methods; e; e = e->next)
        if (strcmp(e->m->name, name) == 0) {
            s->method_cache = e->m;
            s->method_cache_name = e->m->name;
            return e->m;
        }
    return NULL;
}

/* Forward declarations */
Value *eval_expr(Interp *in, Node *e, VarMap *scope);

void exec_stmt(Interp *in, Node *st, VarMap *scope, Flow *fl);

void exec_stmts(Interp *in, Node **stmts, int n, VarMap *scope, Flow *fl);

/* Execute a class method (Objstream: Obj::call / __init__ for new) */
Result *interp_exec_method(Interp *in, Method *m, Value **args, int nargs, VarMap *parent, Value *self) {
    if (m->nparams != nargs) {
        char buf[BIO_MSG_MAX];
        snprintf(buf, sizeof buf, "%s requires %d arguments, got %d", m->name, m->nparams, nargs);
        return mk_ref(astrdup(buf));
    }
    if (m->builtin) {
        /* Builtin class method: generic dispatch, self as the first argument */
        Value **all = aalloc(sizeof(Value *) * (size_t)(nargs + 1));
        all[0] = self;
        for (int i = 0; i < nargs; i++) all[i + 1] = args[i];
        Stream *bs = self ? stream_find(in, self->kind == V_ARR ? "Array" : self->obj_cls) : NULL;
        if (!bs) bs = in->cur_stream;
        return builtin_request(bs, m->builtin, m->name, all, nargs + 1);
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
        /* Smart-reference argument pointing to a stream variable → bind as a stream reference
         * so io::println-style calls work inside the method. */
        if (a && a->kind == V_REF) {
            if (a->ref_tgt && a->ref_tgt->kind == 0) {
                Value *tv = var_get_layer(a->ref_tgt->map, a->ref_tgt->name);
                if (tv && (tv->kind == V_STREAM ||
                           (tv->kind == V_RES && !tv->res->ref && tv->res->res &&
                            tv->res->res->kind == V_STREAM))) {
                    if (tv->kind == V_RES) tv = tv->res->res;
                    a = mk_streamref(tv->stream_ref);
                    goto bind;
                }
            }
        }
        if (a && a->kind == V_RES && a->res && !a->res->ref && a->res->res &&
            a->res->res->kind == V_REF) {
            Value *rv = a->res->res;
            if (rv->ref_tgt && rv->ref_tgt->kind == 0) {
                Value *tv = var_get_layer(rv->ref_tgt->map, rv->ref_tgt->name);
                if (tv && (tv->kind == V_STREAM ||
                           (tv->kind == V_RES && !tv->res->ref && tv->res->res &&
                            tv->res->res->kind == V_STREAM))) {
                    if (tv->kind == V_RES) tv = tv->res->res;
                    a = mk_streamref(tv->stream_ref);
                    goto bind;
                }
            }
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
    if (s->builtin == B_BIN) {
        /* De-special-cased: a Bio method implemented in the stream body runs as a
         * normal method; signatures and anything else dispatch to the exported
         * symbol of the attached library. */
        Method *bm = method_find(s, method);
        if (!bm || !bm->stmts) return bin_request(s, method, args, nargs);
    } else if (s->builtin) {
        return builtin_request(s, s->builtin, method, args, nargs);
    }
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
        /* Fork of a builtin signature (e.g. FIO r {}): fall back to the
         * builtin implementation with this stream's own file state. */
        if (s->sig_builtin != B_NONE)
            return builtin_request(s, s->sig_builtin, method, args, nargs);
        char buf[BIO_MSG_MAX];
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
            char buf[BIO_MSG_MAX];
            snprintf(buf, sizeof buf, "refused: variable %s does not exist (washed away?)", e->name);
            return mk_refval(astrdup(buf));
        }
        case N_PROP: {
            Value *base = eval_expr(in, e->l, scope);
            if (base->kind == V_OBJ || (base->kind == V_ARR && base->obj_fields)) {
                /* Object property access: obj.hp (array objects can also have properties) */
                Value *f = var_get_layer(base->obj_fields, e->name);
                if (f) return f;
                char buf[BIO_MSG_MAX];
                snprintf(buf, sizeof buf, "refused: property %s was washed away", e->name);
                return mk_refval(astrdup(buf));
            }
            if (base->kind == V_RES) {
                /* Pass-through: a successful Result holding an object/array object → property access (a new result can be used directly as h.hp) */
                if (!base->res->ref && base->res->res &&
                    (base->res->res->kind == V_OBJ ||
                     (base->res->res->kind == V_ARR && base->res->res->obj_fields))) {
                    Value *f = var_get_layer(base->res->res->obj_fields, e->name);
                    if (f) return f;
                    char buf[BIO_MSG_MAX];
                    snprintf(buf, sizeof buf, "refused: property %s was washed away", e->name);
                    return mk_refval(astrdup(buf));
                }
                char buf[BIO_MSG_MAX];
                snprintf(buf, sizeof buf, "refused: Result has no property %s", e->name);
                return mk_refval(astrdup(buf));
            }
            char buf[BIO_MSG_MAX];
            snprintf(buf, sizeof buf, "refused: cannot access %s (property washed away)", e->name);
            return mk_refval(astrdup(buf));
        }
        case N_REF: {
            /* address-of &<lvalue>: build a reference target for a variable,
             * an array element (a[0]) or an object field (obj.field). */
            Node *tg = e->l;
            if (tg->kind == N_VAR) {
                VarMap *map = var_find_map(scope, tg->name);
                if (!map) {
                    Value *self = var_get(scope, "this");
                    if (self && (self->kind == V_OBJ || (self->kind == V_ARR && self->obj_fields)) &&
                        self->obj_fields && var_get_layer(self->obj_fields, tg->name))
                        return mk_refobj(e->ref_perm, e->ref_follow, ref_target_field(self, tg->name));
                    char buf[BIO_MSG_MAX];
                    snprintf(buf, sizeof buf, "refused: cannot take address of unknown variable %s", tg->name);
                    return mk_refval(astrdup(buf));
                }
                return mk_refobj(e->ref_perm, e->ref_follow, ref_target_var(map, tg->name));
            }
            if (tg->kind == N_INDEX) {
                Value *base = eval_expr(in, tg->l, scope);
                if (base->kind == V_RES && base->res && !base->res->ref && base->res->res)
                    base = base->res->res;
                Value *iv = eval_expr(in, tg->r, scope);
                if (iv->kind == V_RES && iv->res && !iv->res->ref) iv = iv->res->res;
                if (iv->kind != V_NUM) return mk_refval("refused: reference index requires a number");
                return mk_refobj(e->ref_perm, e->ref_follow, ref_target_elem(base, (int)iv->num));
            }
            if (tg->kind == N_PROP) {
                Value *base = eval_expr(in, tg->l, scope);
                if (base->kind == V_RES && base->res && !base->res->ref && base->res->res)
                    base = base->res->res;
                if (base->kind == V_OBJ || (base->kind == V_ARR && base->obj_fields))
                    return mk_refobj(e->ref_perm, e->ref_follow, ref_target_field(base, tg->name));
                return mk_refval("refused: cannot take address of a non-object property");
            }
            return mk_refval("refused: cannot take address of this expression");
        }
        case N_UNWRAP: {
            /* get X takes the actual returned value / cause X takes the refusal reason */
            Value *v = eval_expr(in, e->l, scope);
            int want_cause = strcmp(e->op, "cause") == 0;
            if (is_rejected(v)) {
                if (want_cause) return mk_str(reject_reason(v));   /* Rejected → refusal reason */
                return v;                                            /* get on a rejected value → refusal propagation */
            }
            if (v->kind == V_RES && v->res) {
                if (want_cause) {
                    if (v->res->ref) return mk_str(v->res->ref);
                    return mk_str("(no cause)");
                }
                if (v->res->ref) return v;                          /* Refusal propagation */
                return v->res->res;
            }
            if (v->kind == V_REF) {
                if (want_cause) return mk_str("(no cause)");
                if (!strchr(v->ref_perm, 'r'))
                    return mk_refval("refused: reference is write-only, cannot read");
                const char *err = NULL;
                Value *rv = ref_read(v->ref_tgt, &err);
                if (!rv) return mk_refval(astrdup(err ? err : "reference read failed"));
                return rv;
            }
            /* Not a Result: get is itself, cause is none */
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
        case N_CALL: {
            Value **vals = aalloc(sizeof(Value *) * 64);
            int nvals = 0;
            for (int i = 0; i < e->nargs; i++) {
                Value *v = eval_expr(in, e->args[i], scope);
                if (is_rejected(v)) {
                    char buf[BIO_MSG_MAX];
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
                            char buf[BIO_MSG_MAX];
                            snprintf(buf, sizeof buf, "refused: class %s has no method %s", clsname, e->mname);
                            return mk_refval(astrdup(buf));
                        }
                        if (ov->kind == V_OBJ || (ov->kind == V_ARR && ov->obj_fields))
                            ov->obj_fields->parent = in->cur_area;
                        Result *r = interp_exec_method(in, m, vals, nvals, in->cur_area, ov);
                        if (r->ref && strcmp(r->ref, NOTHING) != 0) return mk_refval(r->ref);
                        Value *w2 = aalloc(sizeof(Value));
                        w2->kind = V_RES;
                        if (r->ref) w2->res = mk_res(mk_str(""));   /* Implicit completion → success with no value */
                        else w2->res = mk_res(r->res);
                        return w2;
                    }
                    char buf[BIO_MSG_MAX];
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
                    char buf[BIO_MSG_MAX];
                    snprintf(buf, sizeof buf, "refused: no function %s (no stream provides it)", e->mname);
                    return mk_refval(astrdup(buf));
                }
            }
            /* @onlyread streams: users may not execute write methods (the
             * stream's own methods may, so internal calls are allowed). */
            if (s && s->onlyread && in->cur_stream != s &&
                stream_method_is_write(in, s, e->mname)) {
                char buf[BIO_MSG_MAX];
                snprintf(buf, sizeof buf,
                         "refused: stream %s is @onlyread — %s() is a write method",
                         s->name, e->mname);
                return mk_refval(astrdup(buf));
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
            /* Reference variable declaration:
             * &rw u int p = &a[0];  — the whole reference is a typed value. */
            Value *iv = eval_expr(in, st->init, scope);
            if (is_rejected(iv)) { fl->ret = mk_ref(reject_reason(iv)); break; }
            if (iv->kind != V_REF) {
                fl->ret = mk_ref("refused: reference declaration requires &<expression>");
                break;
            }
            iv->ref_type = st->ref_type;
            var_set(scope, st->name, iv);
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
            /* Assignment through a reference variable: p = v writes the target. */
            if (!st->target && st->name) {
                Value *old = var_get(scope, st->name);
                if (old && old->kind == V_REF) {
                    if (st->op && strcmp(st->op, "=") != 0) {
                        fl->ret = mk_ref("refused: compound assignment on a reference is not supported (use p = v or p++)");
                        break;
                    }
                    if (!strchr(old->ref_perm, 'w')) {
                        fl->ret = mk_ref("refused: reference is read-only, cannot write");
                        break;
                    }
                    const char *err = NULL;
                    if (ref_write(old->ref_tgt, v, &err) != 0) {
                        fl->ret = mk_ref(astrdup(err ? err : "reference write failed"));
                        break;
                    }
                    break;
                }
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
                    char buf[BIO_MSG_MAX];
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
        case N_INC: {
            /* i++ / i--: numeric variables increment; reference variables move
             * their pointer (m permission, like C's a++). */
            Value *old = var_get(scope, st->name);
            if (!old || (old->kind == V_RES && old->res->ref)) {
                fl->ret = mk_ref("refused: increment target does not exist");
                break;
            }
            if (old->kind == V_REF) {
                if (!strchr(old->ref_perm, 'm')) {
                    fl->ret = mk_ref("refused: moving the pointer requires m permission");
                    break;
                }
                const char *err = NULL;
                int delta = strcmp(st->inc_op, "++") == 0 ? 1 : -1;
                if (ref_move(old->ref_tgt, delta, &err) != 0) {
                    fl->ret = mk_ref(astrdup(err ? err : "pointer move failed"));
                    break;
                }
                break;
            }
            if (old->kind != V_NUM) {
                fl->ret = mk_ref("refused: increment/decrement requires a number or a reference");
                break;
            }
            double nv = old->num + (strcmp(st->inc_op, "++") == 0 ? 1 : -1);
            var_set(scope, st->name, mk_num(nv));
            break;
        }
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
                    /* res result: the responded value is a successful Result → take its inner value */
                    if (v->kind == V_RES && v->res && !v->res->ref && v->res->res)
                        v = v->res->res;
                    fl->ret = mk_res(v);
                } else {
                    /* ref result: a rejected Result is forwarded with its refusal reason */
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
            "        this::data = get Solid::new();\n"
            "        ALL i = 0;\n"
            "        while (i < n) { Solid::push(this::data, 0); i = i + 1; }\n"
            "        Arrays::add(this);\n"
            "    }\n"
            "    int len() { res get Solid::len(this::data); }\n"
            "    int get(i int) { res get Solid::get(this::data, i); }\n"
            "    void set(i int, v) { Solid::set(this::data, i, v); res \"\"; }\n"
            "    void push(v) { Solid::push(this::data, v); res \"\"; }\n"
            "    int pop() { res get Solid::pop(this::data); }\n"
            "    void clear() { Solid::clear(this::data); res \"\"; }\n"
            "    string join(sep string) { res get Solid::join(this::data, sep); }\n"
            "}\n"
            "Class Vector {\n"
            "    void __init__() {\n"
            "        this::data = get Solid::new();\n"
            "        Arrays::add(this);\n"
            "    }\n"
            "    int len() { res get Solid::len(this::data); }\n"
            "    int get(i int) { res get Solid::get(this::data, i); }\n"
            "    void set(i int, v) { Solid::set(this::data, i, v); res \"\"; }\n"
            "    void push(v) { Solid::push(this::data, v); res \"\"; }\n"
            "    int pop() { res get Solid::pop(this::data); }\n"
            "    void clear() { Solid::clear(this::data); res \"\"; }\n"
            "    string join(sep string) { res get Solid::join(this::data, sep); }\n"
            "}\n";
        static Decl *pre = NULL;
        static int prebuilt_done = 0;
        if (!prebuilt_done) {
            int nt2, err2 = 0;
            Tok *toks2 = tokenize(PREBUILT, &nt2);
            pre = parse_program_tokens(toks2, nt2, &err2);
            prebuilt_done = 1;
        }
        int err2 = pre ? 0 : 1;
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
    {
        Stream *tm = stream_new("Time", B_TIME);
        tm->onlyread = 1;   /* global timer is a read-only stream */
        stream_add(in, tm);
    }
    stream_add(in, stream_new("Ref", B_REF));
    stream_add(in, stream_new("Taskm", B_TASK));
    stream_add(in, stream_new("Threads", B_BTS));
    stream_add(in, stream_new("SIO", B_SIO));
    stream_add(in, stream_new("Solid", B_SOLID));
    stream_add(in, stream_new("Arrays", B_ARRAYS));
    {
        Stream *f = stream_new("FIO", B_FIO);
        f->onlyread = 1;    /* FIO itself is read-only; forks redefine the signature */
        stream_add(in, f);
    }
    stream_add(in, stream_new("CIO", B_CIO));
    stream_add(in, stream_new("Com", B_COM));       /* Comstream computation stream */
    stream_add(in, stream_new("IO", B_IO));         /* IOStream: abstract parent; implementations in CIO/FIO/SIO */
    stream_add(in, stream_new("Console", B_CIO));   /* CIO's prebuilt fork */
    for (Decl *d = in->decls; d; d = d->next) {
        if (d->kind == D_BIN) {
            /* Binary library stream: dlopen the attached library (exported symbols become
             * methods); the body may also declare normal Bio methods and fields — no special case. */
            void *h = bio_dlopen(d->file);
            if (!h) {
                fprintf(stderr, "⚠️ binary library load failed %s: %s\n", d->file, bio_dlerror());
            }
            Stream *b = stream_new(d->name, B_BIN);
            b->dl = h;
            b->onlyread = d->onlyread;
            b->unfork = d->unfork;
            for (int i = 0; i < d->nmethods; i++) {
                MethodEntry *e = aalloc(sizeof(MethodEntry));
                e->m = &d->methods[i];
                e->next = b->methods;
                b->methods = e;
            }
            for (int i = 0; i < d->nfields; i++)
                var_set(b->fields, d->fields[i].name, field_default(d->fields[i].type));
            stream_add(in, b);
            continue;
        }
        if (d->kind == D_FORK) {
            Stream *s = stream_new(d->name, 0);
            s->onlyread = d->onlyread;
            s->unfork = d->unfork;
            if (strcmp(d->sig, "FIO") == 0) s->sig_builtin = B_FIO;
            else if (strcmp(d->sig, "CIO") == 0) s->sig_builtin = B_CIO;
            else if (strcmp(d->sig, "SIO") == 0) s->sig_builtin = B_SIO;
            for (Decl *sd = in->decls; sd; sd = sd->next)
                if (sd->unfork && (sd->kind == D_SIG || sd->kind == D_BIN ||
                                   sd->kind == D_FORK || sd->kind == D_CLASS) &&
                    strcmp(sd->name, d->sig) == 0) {
                    fprintf(stderr, "refused: stream %s is @unfork, cannot fork\n", d->sig);
                    s = NULL;
                    break;
                }
            if (!s) continue;
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
            s->onlyread = d->onlyread;
            s->unfork = d->unfork;
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
                s->onlyread = d->onlyread;
                s->unfork = d->unfork;
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
