/*
 * llvm.c — Stage-1 LLVM backend for BioLang.
 *
 * This is a real AST -> LLVM IR -> native object file pipeline for a small
 * subset of BioLang. Unsupported constructs are rejected with a clear error
 * instead of silently falling back to the interpreter.
 */
#include "bio.h"
#include "llvm.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Value categories tracked by the backend. */
typedef enum {
    TY_VOID,
    TY_INT,
    TY_FLOAT,
    TY_DOUBLE,
    TY_BOOL,
    TY_STR
} TyKind;

typedef struct {
    LLVMValueRef v;
    TyKind ty;
} LV;

typedef struct Var {
    const char *name;
    LLVMValueRef alloca;
    TyKind ty;
    struct Var *next;
} Var;

typedef struct Loop {
    LLVMBasicBlockRef cont;
    LLVMBasicBlockRef brk;
    struct Loop *next;
} Loop;

typedef struct GConst {
    const char *name;
    long long val;
    struct GConst *next;
} GConst;

typedef struct FnInfo {
    const char *name;
    Method *m;
    LLVMValueRef fn;
    LLVMTypeRef fnty;
    TyKind ret;
    struct FnInfo *next;
} FnInfo;

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef builder;
    LLVMTypeRef i32ty;
    LLVMTypeRef floatty;
    LLVMTypeRef doublety;
    LLVMTypeRef voidty;
    LLVMTypeRef boolty;
    LLVMTypeRef ptrty;
    LLVMValueRef printf_fn;
    LLVMTypeRef printf_fnty;

    Decl *main_decl;
    GConst *consts;
    FnInfo *fns;

    LLVMValueRef cur_fn;
    LLVMBasicBlockRef entry;
    TyKind cur_ret;
    Var *vars;
    Loop *loops;
    int terminated;
    int block_seq;
    int err;
    char errbuf[512];
} Gen;

static void set_error(Gen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->errbuf, sizeof g->errbuf, fmt, ap);
    va_end(ap);
    g->err = 1;
}

static TyKind type_from_name(Gen *g, const char *name) {
    if (!name) {
        set_error(g, "missing type declaration");
        return TY_INT;
    }
    if (strcmp(name, "void") == 0) return TY_VOID;
    if (strcmp(name, "int") == 0) return TY_INT;
    if (strcmp(name, "float") == 0) return TY_FLOAT;
    if (strcmp(name, "double") == 0) return TY_DOUBLE;
    set_error(g, "type '%s' is not yet supported by LLVM backend", name);
    return TY_INT;
}

static LLVMTypeRef ltype(Gen *g, TyKind ty) {
    switch (ty) {
        case TY_VOID: return g->voidty;
        case TY_INT: return g->i32ty;
        case TY_FLOAT: return g->floatty;
        case TY_DOUBLE: return g->doublety;
        case TY_BOOL: return g->boolty;
        case TY_STR: return g->ptrty;
    }
    return g->i32ty;
}

static LV zero_lv(Gen *g, TyKind ty) {
    LV z;
    z.ty = ty;
    z.v = ty == TY_DOUBLE ? LLVMConstReal(g->doublety, 0.0)
        : ty == TY_FLOAT ? LLVMConstReal(g->floatty, 0.0)
        : LLVMConstInt(ltype(g, ty), 0, ty == TY_INT);
    return z;
}

static LV one_lv(Gen *g, TyKind ty) {
    LV z = zero_lv(g, ty);
    if (ty == TY_DOUBLE) z.v = LLVMConstReal(g->doublety, 1.0);
    else if (ty == TY_FLOAT) z.v = LLVMConstReal(g->floatty, 1.0);
    else z.v = LLVMConstInt(ltype(g, ty), 1, 1);
    return z;
}

static int is_fp(TyKind ty) {
    return ty == TY_FLOAT || ty == TY_DOUBLE;
}

static TyKind common_type(TyKind a, TyKind b) {
    if (a == TY_DOUBLE || b == TY_DOUBLE) return TY_DOUBLE;
    if (a == TY_FLOAT || b == TY_FLOAT) return TY_FLOAT;
    return TY_INT;
}

static LV cast_lv(Gen *g, LV x, TyKind want) {
    if (x.ty == want) return x;
    if (x.ty == TY_STR || want == TY_STR) {
        set_error(g, "string values are only supported as CIO::print arguments");
        return zero_lv(g, TY_INT);
    }
    if (x.ty == TY_VOID) {
        set_error(g, "void value used as a number");
        return zero_lv(g, want);
    }

    if (x.ty == TY_BOOL && want == TY_INT) {
        x.v = LLVMBuildZExt(g->builder, x.v, g->i32ty, "bool2int");
        x.ty = TY_INT;
        return x;
    }
    if (x.ty == TY_INT && want == TY_BOOL) {
        x.v = LLVMBuildICmp(g->builder, LLVMIntNE, x.v,
                            LLVMConstInt(g->i32ty, 0, 1), "int2bool");
        x.ty = TY_BOOL;
        return x;
    }
    if (x.ty == TY_BOOL && is_fp(want)) {
        x = cast_lv(g, x, TY_INT);
    }
    if (is_fp(x.ty) && want == TY_BOOL) {
        x.v = LLVMBuildFCmp(g->builder, LLVMRealUNE, x.v,
                            zero_lv(g, x.ty).v, "fp2bool");
        x.ty = TY_BOOL;
        return x;
    }

    if (x.ty == TY_INT && is_fp(want)) {
        x.v = LLVMBuildSIToFP(g->builder, x.v, ltype(g, want), "int2fp");
        x.ty = want;
        return x;
    }
    if (is_fp(x.ty) && want == TY_INT) {
        x.v = LLVMBuildFPToSI(g->builder, x.v, g->i32ty, "fp2int");
        x.ty = TY_INT;
        return x;
    }
    if (x.ty == TY_FLOAT && want == TY_DOUBLE) {
        x.v = LLVMBuildFPExt(g->builder, x.v, g->doublety, "fpext");
        x.ty = TY_DOUBLE;
        return x;
    }
    if (x.ty == TY_DOUBLE && want == TY_FLOAT) {
        x.v = LLVMBuildFPTrunc(g->builder, x.v, g->floatty, "fptrunc");
        x.ty = TY_FLOAT;
        return x;
    }
    set_error(g, "cannot convert expression type to requested type");
    return zero_lv(g, want);
}

static GConst *const_find(Gen *g, const char *name) {
    for (GConst *c = g->consts; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static long long const_eval(Gen *g, Node *e) {
    if (!e) {
        set_error(g, "global const requires an initializer");
        return 0;
    }
    if (e->kind == N_NUM) {
        long long v = (long long)e->num;
        if ((double)v != e->num) {
            set_error(g, "global const int initializer must be an integer");
            return 0;
        }
        return v;
    }
    if (e->kind == N_VAR) {
        GConst *c = const_find(g, e->name);
        if (!c) {
            set_error(g, "const '%s' is not declared before use", e->name);
            return 0;
        }
        return c->val;
    }
    if (e->kind == N_BINOP) {
        long long a = const_eval(g, e->l);
        long long b = const_eval(g, e->r);
        if (strcmp(e->op, "+") == 0) return a + b;
        if (strcmp(e->op, "-") == 0) return a - b;
        if (strcmp(e->op, "*") == 0) return a * b;
        if (strcmp(e->op, "/") == 0) {
            if (b == 0) {
                set_error(g, "division by zero in global const");
                return 0;
            }
            return a / b;
        }
        if (strcmp(e->op, "%") == 0) {
            if (b == 0) {
                set_error(g, "modulo by zero in global const");
                return 0;
            }
            return a % b;
        }
    }
    set_error(g, "global const initializer is not yet supported by LLVM backend");
    return 0;
}

static FnInfo *fn_find(Gen *g, const char *name) {
    for (FnInfo *f = g->fns; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

static Var *var_find(Gen *g, const char *name) {
    for (Var *v = g->vars; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

static LLVMBasicBlockRef new_block(Gen *g, const char *base) {
    char name[80];
    snprintf(name, sizeof name, "%s.%d", base, g->block_seq++);
    return LLVMAppendBasicBlockInContext(g->ctx, g->cur_fn, name);
}

static void place(Gen *g, LLVMBasicBlockRef bb) {
    LLVMPositionBuilderAtEnd(g->builder, bb);
    g->terminated = 0;
}

static void ensure_block(Gen *g) {
    if (g->terminated) place(g, new_block(g, "after"));
}

static void maybe_br(Gen *g, LLVMBasicBlockRef target) {
    if (g->terminated) return;
    LLVMBuildBr(g->builder, target);
    g->terminated = 1;
}

static LLVMValueRef alloca_for(Gen *g, TyKind ty, const char *name) {
    LLVMBasicBlockRef old = LLVMGetInsertBlock(g->builder);
    LLVMValueRef first = LLVMGetFirstInstruction(g->entry);
    if (first)
        LLVMPositionBuilderBefore(g->builder, first);
    else
        LLVMPositionBuilderAtEnd(g->builder, g->entry);
    char aname[96];
    snprintf(aname, sizeof aname, "var.%s", name);
    LLVMValueRef a = LLVMBuildAlloca(g->builder, ltype(g, ty), aname);
    LLVMPositionBuilderAtEnd(g->builder, old);
    return a;
}

static void add_var(Gen *g, const char *name, TyKind ty) {
    if (var_find(g, name)) {
        set_error(g, "variable '%s' redeclared", name);
        return;
    }
    Var *v = calloc(1, sizeof *v);
    v->name = name;
    v->ty = ty;
    v->alloca = alloca_for(g, ty, name);
    v->next = g->vars;
    g->vars = v;
}

static LV load_var(Gen *g, Var *v) {
    LV r;
    r.ty = v->ty;
    r.v = LLVMBuildLoad2(g->builder, ltype(g, v->ty), v->alloca, v->name);
    return r;
}

static void store_var(Gen *g, Var *v, LV val) {
    val = cast_lv(g, val, v->ty);
    LLVMBuildStore(g->builder, val.v, v->alloca);
}

static LV const_int(Gen *g, long long v) {
    LV r;
    r.ty = TY_INT;
    r.v = LLVMConstInt(g->i32ty, (unsigned long long)v, 1);
    return r;
}

static LV literal_lv(Gen *g, double d) {
    long long i = (long long)d;
    if ((double)i == d && i >= -2147483648LL && i <= 2147483647LL)
        return const_int(g, i);
    LV r;
    r.ty = TY_DOUBLE;
    r.v = LLVMConstReal(g->doublety, d);
    return r;
}

static LLVMValueRef string_ptr(Gen *g, const char *s) {
    return LLVMBuildGlobalStringPtr(g->builder, s, "str");
}

static LV gen_expr(Gen *g, Node *e);
static void gen_stmt(Gen *g, Node *s);
static void gen_stmts(Gen *g, Node **stmts, int n);

static LV gen_binop(Gen *g, const char *op, LV a, LV b) {
    if (a.ty == TY_BOOL) a = cast_lv(g, a, TY_INT);
    if (b.ty == TY_BOOL) b = cast_lv(g, b, TY_INT);

    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
        LV r;
        r.ty = TY_BOOL;
        if (is_fp(a.ty) || is_fp(b.ty)) {
            TyKind ct = common_type(a.ty, b.ty);
            a = cast_lv(g, a, ct);
            b = cast_lv(g, b, ct);
            LLVMRealPredicate p = LLVMRealOEQ;
            if (strcmp(op, "==") == 0) p = LLVMRealOEQ;
            else if (strcmp(op, "!=") == 0) p = LLVMRealONE;
            else if (strcmp(op, "<") == 0) p = LLVMRealOLT;
            else if (strcmp(op, ">") == 0) p = LLVMRealOGT;
            else if (strcmp(op, "<=") == 0) p = LLVMRealOLE;
            else if (strcmp(op, ">=") == 0) p = LLVMRealOGE;
            r.v = LLVMBuildFCmp(g->builder, p, a.v, b.v, "cmp");
        } else {
            a = cast_lv(g, a, TY_INT);
            b = cast_lv(g, b, TY_INT);
            LLVMIntPredicate p = LLVMIntEQ;
            if (strcmp(op, "==") == 0) p = LLVMIntEQ;
            else if (strcmp(op, "!=") == 0) p = LLVMIntNE;
            else if (strcmp(op, "<") == 0) p = LLVMIntSLT;
            else if (strcmp(op, ">") == 0) p = LLVMIntSGT;
            else if (strcmp(op, "<=") == 0) p = LLVMIntSLE;
            else if (strcmp(op, ">=") == 0) p = LLVMIntSGE;
            r.v = LLVMBuildICmp(g->builder, p, a.v, b.v, "cmp");
        }
        return r;
    }

    if (strcmp(op, "%") == 0) {
        if (is_fp(a.ty) || is_fp(b.ty)) {
            set_error(g, "modulo on floating-point values is not yet supported by LLVM backend");
            return zero_lv(g, TY_INT);
        }
        a = cast_lv(g, a, TY_INT);
        b = cast_lv(g, b, TY_INT);
        LV r = { LLVMBuildSRem(g->builder, a.v, b.v, "rem"), TY_INT };
        return r;
    }

    TyKind ct = common_type(a.ty, b.ty);
    a = cast_lv(g, a, ct);
    b = cast_lv(g, b, ct);
    LV r;
    r.ty = ct;
    if (is_fp(ct)) {
        if (strcmp(op, "+") == 0)
            r.v = LLVMBuildFAdd(g->builder, a.v, b.v, "add");
        else if (strcmp(op, "-") == 0)
            r.v = LLVMBuildFSub(g->builder, a.v, b.v, "sub");
        else if (strcmp(op, "*") == 0)
            r.v = LLVMBuildFMul(g->builder, a.v, b.v, "mul");
        else
            r.v = LLVMBuildFDiv(g->builder, a.v, b.v, "div");
    } else {
        if (strcmp(op, "+") == 0)
            r.v = LLVMBuildAdd(g->builder, a.v, b.v, "add");
        else if (strcmp(op, "-") == 0)
            r.v = LLVMBuildSub(g->builder, a.v, b.v, "sub");
        else if (strcmp(op, "*") == 0)
            r.v = LLVMBuildMul(g->builder, a.v, b.v, "mul");
        else
            r.v = LLVMBuildSDiv(g->builder, a.v, b.v, "div");
    }
    return r;
}

static LV gen_call(Gen *g, Node *e) {
    if (e->qual != NULL) {
        set_error(g, "qualified call '%s::%s' is not yet supported by LLVM backend "
                  "(only CIO::print/println and bare methods)", e->qual, e->mname);
        return zero_lv(g, TY_INT);
    }
    FnInfo *fi = fn_find(g, e->mname);
    if (!fi) {
        set_error(g, "unknown method '%s'", e->mname);
        return zero_lv(g, TY_INT);
    }
    if (e->nargs != fi->m->nparams) {
        set_error(g, "method '%s' expects %d argument(s), got %d",
                  e->mname, fi->m->nparams, e->nargs);
        return zero_lv(g, fi->ret);
    }
    LLVMValueRef args[BIO_ARGS_MAX];
    for (int i = 0; i < e->nargs; i++) {
        TyKind pt = type_from_name(g, fi->m->param_types ? fi->m->param_types[i] : "void");
        LV a = gen_expr(g, e->args[i]);
        args[i] = cast_lv(g, a, pt).v;
    }
    LV r;
    r.ty = fi->ret;
    r.v = LLVMBuildCall2(g->builder, fi->fnty, fi->fn, args,
                         (unsigned)e->nargs,
                         fi->ret == TY_VOID ? "" : e->mname);
    return r;
}

static LV gen_expr(Gen *g, Node *e) {
    ensure_block(g);
    switch (e->kind) {
        case N_NUM:
            return literal_lv(g, e->num);
        case N_STR: {
            LV r;
            r.ty = TY_STR;
            r.v = NULL;
            return r;
        }
        case N_VAR: {
            Var *v = var_find(g, e->name);
            if (v) return load_var(g, v);
            GConst *c = const_find(g, e->name);
            if (c) return const_int(g, c->val);
            set_error(g, "variable '%s' is not declared", e->name);
            return zero_lv(g, TY_INT);
        }
        case N_CALL:
            return gen_call(g, e);
        case N_BINOP:
            return gen_binop(g, e->op, gen_expr(g, e->l), gen_expr(g, e->r));
        default:
            set_error(g, "expression node #%d is not yet supported by LLVM backend",
                      (int)e->kind);
            return zero_lv(g, TY_INT);
    }
}

static LLVMValueRef gen_truth(Gen *g, Node *cond) {
    LV v = gen_expr(g, cond);
    if (v.ty == TY_BOOL) return v.v;
    if (v.ty == TY_INT)
        return LLVMBuildICmp(g->builder, LLVMIntNE, v.v,
                             LLVMConstInt(g->i32ty, 0, 1), "tobool");
    if (is_fp(v.ty))
        return LLVMBuildFCmp(g->builder, LLVMRealUNE, v.v,
                             zero_lv(g, v.ty).v, "tobool");
    set_error(g, "condition must be numeric");
    return LLVMConstInt(g->boolty, 0, 0);
}

static void gen_print(Gen *g, Node *c) {
    char fmt[4096];
    size_t len = 0;
    LLVMValueRef vals[BIO_ARGS_MAX];
    int nvals = 0;

    for (int i = 0; i < c->nargs; i++) {
        if (i > 0 && len + 1 < sizeof fmt) fmt[len++] = ' ';
        Node *arg = c->args[i];
        if (arg->kind == N_STR) {
            if (len + 2 >= sizeof fmt) {
                set_error(g, "CIO print format string too long");
                return;
            }
            fmt[len++] = '%';
            fmt[len++] = 's';
            vals[nvals++] = string_ptr(g, arg->str);
        } else {
            LV v = gen_expr(g, arg);
            if (v.ty == TY_STR) {
                set_error(g, "string variables are not yet supported by LLVM backend");
                return;
            }
            if (v.ty == TY_INT || v.ty == TY_BOOL) {
                if (len + 2 >= sizeof fmt) {
                    set_error(g, "CIO print format string too long");
                    return;
                }
                fmt[len++] = '%';
                fmt[len++] = 'd';
                vals[nvals++] = cast_lv(g, v, TY_INT).v;
            } else {
                if (len + 2 >= sizeof fmt) {
                    set_error(g, "CIO print format string too long");
                    return;
                }
                fmt[len++] = '%';
                fmt[len++] = 'g';
                vals[nvals++] = cast_lv(g, v, TY_DOUBLE).v;
            }
        }
    }
    if (strcmp(c->mname, "println") == 0) {
        if (len + 1 >= sizeof fmt) {
            set_error(g, "CIO print format string too long");
            return;
        }
        fmt[len++] = '\n';
    }
    fmt[len] = 0;

    LLVMValueRef args[BIO_ARGS_MAX + 1];
    args[0] = string_ptr(g, fmt);
    for (int i = 0; i < nvals; i++) args[i + 1] = vals[i];
    LLVMBuildCall2(g->builder, g->printf_fnty, g->printf_fn, args,
                   (unsigned)(nvals + 1), "printf");
}

static void gen_assign(Gen *g, Node *s) {
    if (s->target) {
        set_error(g, "property/array assignment is not yet supported by LLVM backend");
        return;
    }
    if (s->vtype) {
        TyKind ty = type_from_name(g, s->vtype);
        if (ty == TY_VOID || ty == TY_STR) {
            set_error(g, "variable type '%s' is not yet supported by LLVM backend",
                      s->vtype);
            return;
        }
        if (s->op && strcmp(s->op, "=") != 0) {
            set_error(g, "compound initializer declaration is not supported");
            return;
        }
        add_var(g, s->name, ty);
        if (s->expr) store_var(g, var_find(g, s->name), gen_expr(g, s->expr));
        return;
    }
    Var *v = var_find(g, s->name);
    if (!v) {
        set_error(g, "assignment to undeclared variable '%s'", s->name);
        return;
    }
    LV rhs = gen_expr(g, s->expr);
    if (!s->op || strcmp(s->op, "=") == 0) {
        store_var(g, v, rhs);
        return;
    }
    LV cur = load_var(g, v);
    const char *base = s->op;
    char op[4] = { base[0], 0 };
    LV r = gen_binop(g, op, cur, rhs);
    store_var(g, v, r);
}

static void gen_inc(Gen *g, Node *s) {
    Var *v = var_find(g, s->name);
    if (!v) {
        set_error(g, "increment/decrement of undeclared variable '%s'", s->name);
        return;
    }
    LV cur = load_var(g, v);
    LV one = one_lv(g, v->ty);
    LV r;
    r.ty = v->ty;
    if (strcmp(s->inc_op, "++") == 0) {
        r.v = is_fp(v->ty)
            ? LLVMBuildFAdd(g->builder, cur.v, one.v, "inc")
            : LLVMBuildAdd(g->builder, cur.v, one.v, "inc");
    } else {
        r.v = is_fp(v->ty)
            ? LLVMBuildFSub(g->builder, cur.v, one.v, "dec")
            : LLVMBuildSub(g->builder, cur.v, one.v, "dec");
    }
    LLVMBuildStore(g->builder, r.v, v->alloca);
}

static void gen_if(Gen *g, Node *s) {
    ensure_block(g);
    LLVMValueRef cond = gen_truth(g, s->cond);
    LLVMBasicBlockRef thenbb = new_block(g, "if.then");
    LLVMBasicBlockRef elsebb = s->has_else ? new_block(g, "if.else") : NULL;
    LLVMBasicBlockRef endbb = new_block(g, "if.end");
    LLVMBuildCondBr(g->builder, cond, thenbb, s->has_else ? elsebb : endbb);

    place(g, thenbb);
    gen_stmts(g, s->stmts, s->nstmts);
    maybe_br(g, endbb);

    if (s->has_else) {
        place(g, elsebb);
        gen_stmts(g, s->else_stmts, s->n_else);
        maybe_br(g, endbb);
    }
    place(g, endbb);
}

static void gen_while(Gen *g, Node *s) {
    ensure_block(g);
    LLVMBasicBlockRef condbb = new_block(g, "while.cond");
    LLVMBasicBlockRef bodybb = new_block(g, "while.body");
    LLVMBasicBlockRef endbb = new_block(g, "while.end");
    LLVMBuildBr(g->builder, condbb);

    place(g, condbb);
    LLVMValueRef cond = gen_truth(g, s->cond);
    LLVMBuildCondBr(g->builder, cond, bodybb, endbb);

    place(g, bodybb);
    Loop lp = { condbb, endbb, g->loops };
    g->loops = &lp;
    gen_stmts(g, s->stmts, s->nstmts);
    g->loops = lp.next;
    maybe_br(g, condbb);

    place(g, endbb);
}

static void gen_for(Gen *g, Node *s) {
    ensure_block(g);
    if (s->init) gen_stmt(g, s->init);

    LLVMBasicBlockRef condbb = new_block(g, "for.cond");
    LLVMBasicBlockRef bodybb = new_block(g, "for.body");
    LLVMBasicBlockRef updatebb = new_block(g, "for.update");
    LLVMBasicBlockRef endbb = new_block(g, "for.end");
    LLVMBuildBr(g->builder, condbb);

    place(g, condbb);
    if (s->cond) {
        LLVMValueRef cond = gen_truth(g, s->cond);
        LLVMBuildCondBr(g->builder, cond, bodybb, endbb);
    } else {
        LLVMBuildBr(g->builder, bodybb);
    }

    place(g, bodybb);
    Loop lp = { updatebb, endbb, g->loops };
    g->loops = &lp;
    gen_stmts(g, s->stmts, s->nstmts);
    g->loops = lp.next;
    maybe_br(g, updatebb);

    place(g, updatebb);
    if (s->update) gen_stmt(g, s->update);
    maybe_br(g, condbb);

    place(g, endbb);
}

static void gen_ret(Gen *g, Node *s) {
    ensure_block(g);
    if (s->retkind && strcmp(s->retkind, "res") != 0) {
        set_error(g, "'%s' return is not yet supported by LLVM backend", s->retkind);
        return;
    }
    if (s->nrets > 1) {
        set_error(g, "multi-value return is not yet supported by LLVM backend");
        return;
    }
    if (g->cur_ret == TY_VOID) {
        if (s->expr) {
            set_error(g, "void method cannot return a value");
            return;
        }
        LLVMBuildRetVoid(g->builder);
    } else {
        if (!s->expr) {
            set_error(g, "method must return a value");
            return;
        }
        LV v = cast_lv(g, gen_expr(g, s->expr), g->cur_ret);
        LLVMBuildRet(g->builder, v.v);
    }
    g->terminated = 1;
}

static void gen_stmt(Gen *g, Node *s) {
    ensure_block(g);
    switch (s->kind) {
        case N_ASSIGN: gen_assign(g, s); break;
        case N_INC: gen_inc(g, s); break;
        case N_CALLSTMT: {
            if (!s->l || s->l->kind != N_CALL) {
                set_error(g, "call statement is not yet supported by LLVM backend");
                break;
            }
            Node *c = s->l;
            if (c->qual && strcmp(c->qual, "CIO") == 0 &&
                (strcmp(c->mname, "println") == 0 || strcmp(c->mname, "print") == 0)) {
                gen_print(g, c);
            } else {
                gen_call(g, c);
            }
            break;
        }
        case N_RET: gen_ret(g, s); break;
        case N_IF: gen_if(g, s); break;
        case N_WHILE: gen_while(g, s); break;
        case N_FOR: gen_for(g, s); break;
        case N_BREAK: {
            if (!g->loops) {
                set_error(g, "break outside a loop");
                break;
            }
            LLVMBuildBr(g->builder, g->loops->brk);
            g->terminated = 1;
            break;
        }
        case N_CONTINUE: {
            if (!g->loops) {
                set_error(g, "continue outside a loop");
                break;
            }
            LLVMBuildBr(g->builder, g->loops->cont);
            g->terminated = 1;
            break;
        }
        default:
            set_error(g, "statement node #%d is not yet supported by LLVM backend",
                      (int)s->kind);
            break;
    }
}

static void gen_stmts(Gen *g, Node **stmts, int n) {
    for (int i = 0; i < n; i++) gen_stmt(g, stmts[i]);
}

static int predeclare_methods(Gen *g) {
    Decl *d = g->main_decl;
    for (int i = 0; i < d->nmethods; i++) {
        Method *m = &d->methods[i];
        TyKind rt = type_from_name(g, m->ret_type);
        if (g->err) return 1;
        if (rt == TY_STR || (rt == TY_VOID && strcmp(m->ret_type, "void") != 0)) {
            set_error(g, "method return type '%s' is not yet supported by LLVM backend",
                      m->ret_type);
            return 1;
        }
        LLVMTypeRef ptypes[BIO_ARGS_MAX];
        for (int j = 0; j < m->nparams; j++) {
            TyKind pt = type_from_name(g, m->param_types ? m->param_types[j] : "void");
            if (g->err) return 1;
            if (pt == TY_VOID || pt == TY_STR) {
                set_error(g, "parameter type of '%s' is not yet supported by LLVM backend",
                          m->name);
                return 1;
            }
            ptypes[j] = ltype(g, pt);
        }
        char fname[128];
        snprintf(fname, sizeof fname, "bio_%s", m->name);
        LLVMTypeRef fnty = LLVMFunctionType(ltype(g, rt),
                                            m->nparams ? ptypes : NULL,
                                            (unsigned)m->nparams, 0);
        LLVMValueRef fn = LLVMAddFunction(g->mod, fname, fnty);
        FnInfo *fi = calloc(1, sizeof *fi);
        fi->name = m->name;
        fi->m = m;
        fi->fn = fn;
        fi->fnty = fnty;
        fi->ret = rt;
        fi->next = g->fns;
        g->fns = fi;
    }
    return 0;
}

static void gen_method(Gen *g, FnInfo *fi) {
    Method *m = fi->m;
    g->cur_fn = fi->fn;
    g->cur_ret = fi->ret;
    g->vars = NULL;
    g->loops = NULL;
    g->block_seq = 0;
    g->terminated = 0;
    g->entry = LLVMAppendBasicBlockInContext(g->ctx, fi->fn, "entry");
    place(g, g->entry);

    for (int i = 0; i < m->nparams; i++) {
        TyKind pt = type_from_name(g, m->param_types ? m->param_types[i] : "void");
        add_var(g, m->params[i], pt);
        Var *v = var_find(g, m->params[i]);
        if (v) {
            LLVMValueRef arg = LLVMGetParam(fi->fn, (unsigned)i);
            LLVMBuildStore(g->builder, arg, v->alloca);
        }
    }
    gen_stmts(g, m->stmts, m->nstmts);
    if (!g->terminated) {
        if (g->cur_ret == TY_VOID)
            LLVMBuildRetVoid(g->builder);
        else
            LLVMBuildRet(g->builder, zero_lv(g, g->cur_ret).v);
    }
}

static int build_module(Gen *g, Decl *decls) {
    /* Validate top-level declarations and fold global int consts. */
    for (Decl *d = decls; d; d = d->next) {
        if (d->kind == D_CONST) {
            GConst *c = calloc(1, sizeof *c);
            c->name = d->name;
            c->val = const_eval(g, d->init);
            c->next = g->consts;
            g->consts = c;
        } else if (d->kind == D_MAIN) {
            if (g->main_decl) {
                set_error(g, "multiple Main declarations are not supported");
                return 1;
            }
            g->main_decl = d;
        } else {
            set_error(g, "declaration kind #%d is not yet supported by LLVM backend "
                      "(only const int and Main)", (int)d->kind);
            return 1;
        }
    }
    if (!g->main_decl) {
        set_error(g, "no Main stream declared");
        return 1;
    }

    if (predeclare_methods(g) || g->err) return 1;

    /* Define user methods first so recursion resolves to the same functions. */
    for (FnInfo *fi = g->fns; fi; fi = fi->next)
        gen_method(g, fi);
    if (g->err) return 1;

    /* Emit a C-compatible main() that calls Main::exec(). */
    FnInfo *exec = fn_find(g, "exec");
    if (!exec) {
        set_error(g, "Main has no exec() method");
        return 1;
    }
    if (exec->m->nparams != 0) {
        set_error(g, "Main::exec() must not take parameters");
        return 1;
    }
    LLVMTypeRef mainty = LLVMFunctionType(g->i32ty, NULL, 0, 0);
    LLVMValueRef mainfn = LLVMAddFunction(g->mod, "main", mainty);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g->ctx, mainfn, "entry");
    LLVMPositionBuilderAtEnd(g->builder, entry);
    LLVMBuildCall2(g->builder, exec->fnty, exec->fn, NULL, 0, "");
    LLVMBuildRet(g->builder, LLVMConstInt(g->i32ty, 0, 0));
    return g->err ? 1 : 0;
}

static int emit_object(Gen *g, const char *objpath, const char *triple) {
    LLVMTargetRef target = NULL;
    char *tm_err = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &tm_err)) {
        fprintf(stderr, "LLVM backend: cannot find target for %s: %s\n",
                triple, tm_err ? tm_err : "unknown error");
        if (tm_err) LLVMDisposeMessage(tm_err);
        return 1;
    }

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, "", "", LLVMCodeGenLevelDefault,
        LLVMRelocDefault, LLVMCodeModelDefault);
    if (!tm) {
        fprintf(stderr, "LLVM backend: cannot create target machine\n");
        return 1;
    }

    LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
    char *dl = LLVMCopyStringRepOfTargetData(td);
    LLVMSetDataLayout(g->mod, dl);
    LLVMDisposeMessage(dl);
    LLVMDisposeTargetData(td);
    LLVMSetTarget(g->mod, triple);

    char *verr = NULL;
    if (LLVMVerifyModule(g->mod, LLVMReturnStatusAction, &verr)) {
        fprintf(stderr, "LLVM backend: module verification failed:\n%s\n",
                verr ? verr : "(no message)");
        if (verr) LLVMDisposeMessage(verr);
        LLVMDisposeTargetMachine(tm);
        return 1;
    }

    char *emit_err = NULL;
    if (LLVMTargetMachineEmitToFile(tm, g->mod, objpath, LLVMObjectFile,
                                    &emit_err)) {
        fprintf(stderr, "LLVM backend: cannot emit object file: %s\n",
                emit_err ? emit_err : "unknown error");
        if (emit_err) LLVMDisposeMessage(emit_err);
        LLVMDisposeTargetMachine(tm);
        return 1;
    }
    LLVMDisposeTargetMachine(tm);
    return 0;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static char *default_out(const char *file) {
    const char *dot = strrchr(file, '.');
    if (dot && (strcmp(dot, ".bio") == 0 || strcmp(dot, ".bl") == 0)) {
        char *out = malloc((size_t)(dot - file) + 1);
        if (!out) return NULL;
        memcpy(out, file, (size_t)(dot - file));
        out[dot - file] = 0;
        return out;
    }
    char *out = malloc(strlen(file) + 5);
    if (!out) return NULL;
    sprintf(out, "%s.out", file);
    return out;
}

int bio_llvm_compile(const char *file, const char *out) {
    char *owned_out = NULL;
    if (!out) {
        owned_out = default_out(file);
        if (!owned_out) {
            fprintf(stderr, "LLVM backend: out of memory\n");
            return 1;
        }
        out = owned_out;
    }

    char *src = read_file(file);
    if (!src) {
        fprintf(stderr, "LLVM backend: cannot open file: %s\n", file);
        free(owned_out);
        return 1;
    }
    int ntok = 0;
    Tok *toks = tokenize(src, &ntok);
    int perr = 0;
    Decl *decls = parse_program_tokens(toks, ntok, &perr);
    free(src);
    if (perr) {
        fprintf(stderr, "LLVM backend: parse failed\n");
        free(owned_out);
        return 1;
    }

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    Gen g;
    memset(&g, 0, sizeof g);
    g.ctx = LLVMContextCreate();
    g.mod = LLVMModuleCreateWithNameInContext("biolang", g.ctx);
    g.builder = LLVMCreateBuilderInContext(g.ctx);
    g.i32ty = LLVMInt32TypeInContext(g.ctx);
    g.floatty = LLVMFloatTypeInContext(g.ctx);
    g.doublety = LLVMDoubleTypeInContext(g.ctx);
    g.voidty = LLVMVoidTypeInContext(g.ctx);
    g.boolty = LLVMInt1TypeInContext(g.ctx);
    g.ptrty = LLVMPointerTypeInContext(g.ctx, 0);

    /* Declare C printf (variadic). */
    LLVMTypeRef printf_params[1] = { g.ptrty };
    g.printf_fnty = LLVMFunctionType(g.i32ty, printf_params, 1, 1);
    g.printf_fn = LLVMAddFunction(g.mod, "printf", g.printf_fnty);

    if (build_module(&g, decls)) {
        fprintf(stderr, "LLVM backend: %s\n", g.errbuf);
        LLVMDisposeBuilder(g.builder);
        LLVMDisposeModule(g.mod);
        LLVMContextDispose(g.ctx);
        free(owned_out);
        return 1;
    }

    char objpath[2048];
    snprintf(objpath, sizeof objpath, "%s.o", out);
    char *triple = LLVMGetDefaultTargetTriple();
    int rc = emit_object(&g, objpath, triple);
    LLVMDisposeMessage(triple);

    LLVMDisposeBuilder(g.builder);
    LLVMDisposeModule(g.mod);
    LLVMContextDispose(g.ctx);

    if (rc != 0) {
        remove(objpath);
        free(owned_out);
        return 1;
    }

    char cmd[2300];
    snprintf(cmd, sizeof cmd, "cc '%s' -lm -o '%s'", objpath, out);
    int link_rc = system(cmd);
    remove(objpath);
    if (link_rc != 0) {
        fprintf(stderr, "LLVM backend: system linker failed\n");
        free(owned_out);
        return 1;
    }
    free(owned_out);
    return 0;
}
