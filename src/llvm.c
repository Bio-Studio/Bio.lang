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
    TY_STR,
    TY_RESULT
} TyKind;

typedef struct {
    LLVMValueRef v;
    TyKind ty;
    int is_result;
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
    const char *stream;      /* owning stream name (NULL = Main / bare) */
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
    LLVMTypeRef i64ty;
    LLVMTypeRef floatty;
    LLVMTypeRef doublety;
    LLVMTypeRef voidty;
    LLVMTypeRef boolty;
    LLVMTypeRef ptrty;
    LLVMTypeRef resultty;
    LLVMValueRef printf_fn;
    LLVMTypeRef printf_fnty;

    Decl *main_decl;
    Decl *decls;             /* all top-level declarations */
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
        case TY_INT: return g->doublety;   /* BioLang numbers are doubles; int is a label */
        case TY_FLOAT: return g->floatty;
        case TY_DOUBLE: return g->doublety;
        case TY_BOOL: return g->boolty;
        case TY_STR: return g->ptrty;
        case TY_RESULT: return g->resultty;
    }
    return g->i32ty;
}

static LV zero_lv(Gen *g, TyKind ty) {
    LV z;
    z.ty = ty;
    z.is_result = 0;
    z.v = ty == TY_DOUBLE || ty == TY_INT ? LLVMConstReal(g->doublety, 0.0)
        : ty == TY_FLOAT ? LLVMConstReal(g->floatty, 0.0)
        : LLVMConstInt(ltype(g, ty), 0, 0);
    return z;
}

static LV one_lv(Gen *g, TyKind ty) {
    LV z = zero_lv(g, ty);
    if (ty == TY_DOUBLE || ty == TY_INT) z.v = LLVMConstReal(g->doublety, 1.0);
    else if (ty == TY_FLOAT) z.v = LLVMConstReal(g->floatty, 1.0);
    else z.v = LLVMConstInt(ltype(g, ty), 1, 0);
    return z;
}

static int is_fp(TyKind ty) {
    return ty == TY_FLOAT || ty == TY_DOUBLE || ty == TY_INT;
}

static LV cast_lv(Gen *g, LV x, TyKind want);
static LLVMValueRef string_ptr(Gen *g, const char *s);
static LLVMBasicBlockRef new_block(Gen *g, const char *base);
static void place(Gen *g, LLVMBasicBlockRef bb);

static LLVMValueRef make_result(Gen *g, int status, LLVMValueRef value,
                                LLVMValueRef reason) {
    LLVMValueRef r = LLVMGetUndef(g->resultty);
    r = LLVMBuildInsertValue(g->builder, r,
                             LLVMConstInt(g->i32ty, status, 0), 0,
                             "res.status");
    r = LLVMBuildInsertValue(g->builder, r, value, 1, "res.value");
    r = LLVMBuildInsertValue(g->builder, r, reason, 2, "res.reason");
    return r;
}

static LLVMValueRef result_status(Gen *g, LLVMValueRef r) {
    return LLVMBuildExtractValue(g->builder, r, 0, "res.status");
}

static LLVMValueRef result_reason(Gen *g, LLVMValueRef r) {
    return LLVMBuildExtractValue(g->builder, r, 2, "res.reason");
}

static LV result_value_lv(Gen *g, LV x) {
    if (!x.is_result) return x;
    LLVMValueRef dv = LLVMBuildExtractValue(g->builder, x.v, 1, "res.value");
    LV d;
    d.ty = TY_DOUBLE;
    d.v = dv;
    d.is_result = 0;
    LV r = d;
    r.ty = x.ty;
    if (r.ty == TY_VOID) r.ty = TY_DOUBLE;
    if (r.ty == TY_INT || r.ty == TY_BOOL || r.ty == TY_FLOAT)
        r = cast_lv(g, d, r.ty);
    return r;
}

static void unwrap_result(Gen *g, LV *x) {
    if (!x->is_result) return;
    LLVMValueRef st = result_status(g, x->v);
    LLVMValueRef cond = LLVMBuildICmp(g->builder, LLVMIntEQ, st,
                                      LLVMConstInt(g->i32ty, 1, 0),
                                      "res.refused");
    LLVMBasicBlockRef refbb = new_block(g, "refused");
    LLVMBasicBlockRef okbb = new_block(g, "ok");
    LLVMBuildCondBr(g->builder, cond, refbb, okbb);

    place(g, refbb);
    LLVMBuildRet(g->builder, x->v);
    g->terminated = 1;

    place(g, okbb);
    *x = result_value_lv(g, *x);
}

static LV cause_lv(Gen *g, LV x) {
    LV r;
    r.is_result = 0;
    r.ty = TY_STR;
    if (x.is_result) {
        LLVMValueRef st = result_status(g, x.v);
        LLVMValueRef refused = LLVMBuildICmp(g->builder, LLVMIntEQ, st,
                                             LLVMConstInt(g->i32ty, 1, 0),
                                             "res.refused");
        r.v = LLVMBuildSelect(g->builder, refused, result_reason(g, x.v),
                              string_ptr(g, "(no cause)"), "res.cause");
    } else {
        r.v = string_ptr(g, "(no cause)");
    }
    return r;
}

static TyKind common_type(TyKind a, TyKind b) {
    if (a == TY_DOUBLE || b == TY_DOUBLE) return TY_DOUBLE;
    if (a == TY_FLOAT || b == TY_FLOAT) return TY_FLOAT;
    return TY_INT;
}

static LV cast_lv(Gen *g, LV x, TyKind want) {
    if (x.ty == want) return x;
    if (x.ty == TY_RESULT || want == TY_RESULT) {
        set_error(g, "result values must be unwrapped before use");
        return zero_lv(g, want == TY_RESULT ? TY_DOUBLE : want);
    }
    if (x.ty == TY_STR || want == TY_STR) {
        set_error(g, "string values are only supported as CIO::print arguments");
        return zero_lv(g, TY_INT);
    }
    if (x.ty == TY_VOID) {
        set_error(g, "void value used as a number");
        return zero_lv(g, want);
    }

    if (x.ty == TY_BOOL && want == TY_INT) {
        x.v = LLVMBuildUIToFP(g->builder, x.v, g->doublety, "bool2fp");
        x.ty = TY_INT;
        return x;
    }
    if (x.ty == TY_INT && want == TY_BOOL) {
        x.v = LLVMBuildFCmp(g->builder, LLVMRealUNE, x.v,
                            LLVMConstReal(g->doublety, 0.0), "fp2bool");
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

    if (x.ty == TY_INT || x.ty == TY_DOUBLE) {
        /* TY_INT and TY_DOUBLE share the same LLVM representation. */
        if (want == TY_INT || want == TY_DOUBLE) {
            x.ty = want;
            return x;
        }
        if (want == TY_FLOAT) {
            x.v = LLVMBuildFPTrunc(g->builder, x.v, g->floatty, "fptrunc");
            x.ty = TY_FLOAT;
            return x;
        }
    }
    if (x.ty == TY_FLOAT && (want == TY_INT || want == TY_DOUBLE)) {
        x.v = LLVMBuildFPExt(g->builder, x.v, g->doublety, "fpext");
        x.ty = want;
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

static FnInfo *fn_find_qual(Gen *g, const char *stream, const char *name) {
    for (FnInfo *f = g->fns; f; f = f->next)
        if (f->stream && strcmp(f->stream, stream) == 0 &&
            strcmp(f->name, name) == 0)
            return f;
    return NULL;
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
    r.is_result = v->ty == TY_RESULT;
    r.ty = r.is_result ? TY_DOUBLE : v->ty;
    r.v = LLVMBuildLoad2(g->builder, ltype(g, v->ty), v->alloca, v->name);
    return r;
}

static void store_var(Gen *g, Var *v, LV val) {
    if (v->ty == TY_RESULT) {
        if (val.ty == TY_STR) {
            set_error(g, "string values are not yet supported by LLVM backend");
            return;
        }
        if (!val.is_result) {
            val = cast_lv(g, val, TY_DOUBLE);
            val.v = make_result(g, 0, val.v, LLVMConstPointerNull(g->ptrty));
        }
        LLVMBuildStore(g->builder, val.v, v->alloca);
        return;
    }
    if (val.is_result) unwrap_result(g, &val);
    val = cast_lv(g, val, v->ty);
    LLVMBuildStore(g->builder, val.v, v->alloca);
}

static LV const_int(Gen *g, long long v) {
    LV r;
    r.ty = TY_INT;
    r.is_result = 0;
    r.v = LLVMConstReal(g->doublety, (double)v);
    return r;
}

static LV literal_lv(Gen *g, double d) {
    long long i = (long long)d;
    if ((double)i == d && i >= -2147483648LL && i <= 2147483647LL)
        return const_int(g, i);
    LV r;
    r.ty = TY_DOUBLE;
    r.is_result = 0;
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
    if (a.is_result) unwrap_result(g, &a);
    if (b.is_result) unwrap_result(g, &b);
    if (a.ty == TY_BOOL) a = cast_lv(g, a, TY_INT);
    if (b.ty == TY_BOOL) b = cast_lv(g, b, TY_INT);

    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
        LV r;
        r.ty = TY_BOOL;
        r.is_result = 0;
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
        /* BioLang numbers are doubles; % is C fmod (LLVM frem matches). */
        TyKind ct = common_type(a.ty, b.ty);
        a = cast_lv(g, a, ct);
        b = cast_lv(g, b, ct);
        LV r = { LLVMBuildFRem(g->builder, a.v, b.v, "rem"), ct, 0 };
        return r;
    }

    TyKind ct = common_type(a.ty, b.ty);
    a = cast_lv(g, a, ct);
    b = cast_lv(g, b, ct);
    LV r;
    r.ty = ct;
    r.is_result = 0;
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
    FnInfo *fi = NULL;
    if (e->qual != NULL) {
        if (strcmp(e->qual, "CIO") == 0 &&
            (strcmp(e->mname, "println") == 0 || strcmp(e->mname, "print") == 0)) {
            set_error(g, "CIO print outside a statement is not supported");
            return zero_lv(g, TY_INT);
        }
        /* Stream method call: resolve by stream + name (fork implementation). */
        fi = fn_find_qual(g, e->qual, e->mname);
        if (!fi) {
            set_error(g, "unknown method '%s::%s'", e->qual, e->mname);
            return zero_lv(g, TY_INT);
        }
    } else {
        fi = fn_find(g, e->mname);
        if (!fi) {
            set_error(g, "unknown method '%s'", e->mname);
            return zero_lv(g, TY_INT);
        }
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
        if (a.is_result) unwrap_result(g, &a);
        if (a.ty == TY_STR) {
            set_error(g, "string arguments are not yet supported by LLVM backend");
            return zero_lv(g, fi->ret);
        }
        args[i] = cast_lv(g, a, pt).v;
    }
    LV r;
    r.ty = fi->ret;
    r.is_result = 1;
    r.v = LLVMBuildCall2(g->builder, fi->fnty, fi->fn, args,
                         (unsigned)e->nargs,
                         e->mname);
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
            r.is_result = 0;
            r.v = string_ptr(g, e->str);
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
        case N_BINOP: {
            LV a = gen_expr(g, e->l);
            if (a.is_result) unwrap_result(g, &a);
            LV b = gen_expr(g, e->r);
            if (b.is_result) unwrap_result(g, &b);
            return gen_binop(g, e->op, a, b);
        }
        case N_UNWRAP: {
            LV v = gen_expr(g, e->l);
            if (strcmp(e->op, "cause") == 0) return cause_lv(g, v);
            if (v.is_result) unwrap_result(g, &v);
            return v;
        }
        default:
            set_error(g, "expression node #%d is not yet supported by LLVM backend",
                      (int)e->kind);
            return zero_lv(g, TY_INT);
    }
}

static LLVMValueRef gen_truth(Gen *g, Node *cond) {
    LV v = gen_expr(g, cond);
    if (v.is_result) unwrap_result(g, &v);
    if (v.ty == TY_BOOL) return v.v;
    if (is_fp(v.ty))
        return LLVMBuildFCmp(g->builder, LLVMRealUNE, v.v,
                             zero_lv(g, v.ty).v, "tobool");
    set_error(g, "condition must be numeric");
    return LLVMConstInt(g->boolty, 0, 0);
}

static void printf_lit(Gen *g, const char *s) {
    LLVMValueRef args[1] = { string_ptr(g, s) };
    LLVMBuildCall2(g->builder, g->printf_fnty, g->printf_fn, args, 1,
                   "printf");
}

static void printf_val(Gen *g, const char *fmt, LLVMValueRef v) {
    LLVMValueRef args[2] = { string_ptr(g, fmt), v };
    LLVMBuildCall2(g->builder, g->printf_fnty, g->printf_fn, args, 2,
                   "printf");
}

static void print_lv(Gen *g, LV v) {
    if (v.ty == TY_STR) {
        printf_val(g, "%s", v.v);
    } else if (v.ty == TY_BOOL) {
        printf_val(g, "%d", cast_lv(g, v, TY_INT).v);
    } else if (is_fp(v.ty)) {
        /* Integer-valued doubles print as integers (%ld), like the
         * interpreter's print_value; everything else uses %g. */
        LLVMValueRef dv = cast_lv(g, v, TY_DOUBLE).v;
        LLVMValueRef vi = LLVMBuildFPToSI(g->builder, dv, g->i64ty, "num2i64");
        LLVMValueRef back = LLVMBuildSIToFP(g->builder, vi, g->doublety, "i642fp");
        LLVMValueRef isint = LLVMBuildFCmp(g->builder, LLVMRealOEQ, dv, back,
                                           "isint");
        LLVMBasicBlockRef intbb = new_block(g, "print.int");
        LLVMBasicBlockRef fpbb = new_block(g, "print.fp");
        LLVMBasicBlockRef contbb = new_block(g, "print.cont");
        LLVMBuildCondBr(g->builder, isint, intbb, fpbb);
        place(g, intbb);
        printf_val(g, "%ld", vi);
        maybe_br(g, contbb);
        place(g, fpbb);
        printf_val(g, "%g", dv);
        maybe_br(g, contbb);
        place(g, contbb);
    } else {
        set_error(g, "cannot print this value");
    }
}

static void print_result(Gen *g, LV res) {
    LLVMValueRef st = result_status(g, res.v);
    LLVMValueRef cond = LLVMBuildICmp(g->builder, LLVMIntEQ, st,
                                      LLVMConstInt(g->i32ty, 1, 0),
                                      "res.refused");
    LLVMBasicBlockRef refbb = new_block(g, "print.refused");
    LLVMBasicBlockRef okbb = new_block(g, "print.ok");
    LLVMBasicBlockRef contbb = new_block(g, "print.cont");
    LLVMBuildCondBr(g->builder, cond, refbb, okbb);

    place(g, refbb);
    printf_val(g, "refused: %s", result_reason(g, res.v));
    maybe_br(g, contbb);

    place(g, okbb);
    print_lv(g, result_value_lv(g, res));
    maybe_br(g, contbb);

    place(g, contbb);
}

static void gen_print(Gen *g, Node *c) {
    for (int i = 0; i < c->nargs; i++) {
        if (i > 0) printf_lit(g, " ");
        Node *arg = c->args[i];
        if (arg->kind == N_STR) {
            printf_val(g, "%s", string_ptr(g, arg->str));
            continue;
        }

        int is_get = arg->kind == N_UNWRAP && strcmp(arg->op, "get") == 0;
        LV v = gen_expr(g, is_get ? arg->l : arg);
        if (v.is_result)
            print_result(g, v);
        else
            print_lv(g, v);
    }
    if (strcmp(c->mname, "println") == 0) printf_lit(g, "\n");
}

static void gen_assign(Gen *g, Node *s) {
    if (s->target) {
        set_error(g, "property/array assignment is not yet supported by LLVM backend");
        return;
    }
    if (s->vtype) {
        TyKind ty = strcmp(s->vtype, "ALL") == 0
            ? TY_RESULT : type_from_name(g, s->vtype);
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
        if (s->expr)
            store_var(g, var_find(g, s->name), gen_expr(g, s->expr));
        else if (ty == TY_RESULT)
            store_var(g, var_find(g, s->name),
                      zero_lv(g, TY_DOUBLE));
        return;
    }
    Var *v = var_find(g, s->name);
    if (!v) {
        set_error(g, "assignment to undeclared variable '%s'", s->name);
        return;
    }
    LV rhs = gen_expr(g, s->expr);
    if (rhs.is_result) unwrap_result(g, &rhs);
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
    r.is_result = 0;
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
    if (s->nrets > 1) {
        set_error(g, "multi-value return is not yet supported by LLVM backend");
        return;
    }

    if (s->retkind && strcmp(s->retkind, "ref") == 0) {
        if (!s->expr) {
            set_error(g, "'ref' requires a refusal reason");
            return;
        }
        LV v = gen_expr(g, s->expr);
        if (v.is_result) {
            LLVMValueRef st = result_status(g, v.v);
            LLVMValueRef cond = LLVMBuildICmp(g->builder, LLVMIntEQ, st,
                                              LLVMConstInt(g->i32ty, 1, 0),
                                              "res.refused");
            LLVMBasicBlockRef refbb = new_block(g, "ref.refused");
            LLVMBasicBlockRef okbb = new_block(g, "ref.ok");
            LLVMBuildCondBr(g->builder, cond, refbb, okbb);

            place(g, refbb);
            LLVMBuildRet(g->builder, v.v);
            g->terminated = 1;

            place(g, okbb);
            LLVMBuildRet(g->builder,
                         make_result(g, 1, LLVMConstReal(g->doublety, 0.0),
                                     string_ptr(g, "(no cause)")));
            g->terminated = 1;
            return;
        }
        if (v.ty != TY_STR) {
            set_error(g, "'ref' requires a string literal or a refused Result");
            return;
        }
        LLVMBuildRet(g->builder,
                     make_result(g, 1, LLVMConstReal(g->doublety, 0.0), v.v));
        g->terminated = 1;
        return;
    }

    if (!s->expr) {
        LLVMBuildRet(g->builder,
                     make_result(g, 0, LLVMConstReal(g->doublety, 0.0),
                                 LLVMConstPointerNull(g->ptrty)));
        g->terminated = 1;
        return;
    }

    LV v = gen_expr(g, s->expr);
    if (v.is_result) unwrap_result(g, &v);
    if (v.ty == TY_STR) {
        set_error(g, "res of a string value is not yet supported by LLVM backend");
        return;
    }
    v = cast_lv(g, v, TY_DOUBLE);
    LLVMBuildRet(g->builder,
                 make_result(g, 0, v.v, LLVMConstPointerNull(g->ptrty)));
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
    /* Main stream methods (bare calls). */
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
        LLVMTypeRef fnty = LLVMFunctionType(g->resultty,
                                            m->nparams ? ptypes : NULL,
                                            (unsigned)m->nparams, 0);
        LLVMValueRef fn = LLVMAddFunction(g->mod, fname, fnty);
        FnInfo *fi = calloc(1, sizeof *fi);
        fi->name = m->name;
        fi->stream = NULL;
        fi->m = m;
        fi->fn = fn;
        fi->fnty = fnty;
        fi->ret = rt;
        fi->next = g->fns;
        g->fns = fi;
    }
    /* Fork implementation streams: methods callable as Qual::name(...).
     * Signatures (D_SIG) carry no body; calls resolve by method name to the
     * fork that implements them. */
    for (Decl *fd = g->decls; fd; fd = fd->next) {
        if (fd->kind != D_FORK) continue;
        for (int i = 0; i < fd->nmethods; i++) {
            Method *m = &fd->methods[i];
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
            char fname[160];
            snprintf(fname, sizeof fname, "bio_%s_%s", fd->name, m->name);
            LLVMTypeRef fnty = LLVMFunctionType(g->resultty,
                                                m->nparams ? ptypes : NULL,
                                                (unsigned)m->nparams, 0);
            LLVMValueRef fn = LLVMAddFunction(g->mod, fname, fnty);
            FnInfo *fi = calloc(1, sizeof *fi);
            fi->name = m->name;
            fi->stream = fd->name;
            fi->m = m;
            fi->fn = fn;
            fi->fnty = fnty;
            fi->ret = rt;
            fi->next = g->fns;
            g->fns = fi;
        }
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
        LLVMBuildRet(g->builder,
                     make_result(g, 0, LLVMConstReal(g->doublety, 0.0),
                                 LLVMConstPointerNull(g->ptrty)));
    }
}

static int build_module(Gen *g, Decl *decls) {
    g->decls = decls;
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
        } else if (d->kind == D_FORK || d->kind == D_SIG) {
            /* Fork streams provide callable methods; signatures carry the
             * contract (arity checked at the call site). Both are handled
             * in predeclare_methods/gen_method. */
            continue;
        } else {
            set_error(g, "declaration kind #%d is not yet supported by LLVM backend "
                      "(const, Stream/Class forks and Main)", (int)d->kind);
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
    g.i64ty = LLVMInt64TypeInContext(g.ctx);
    g.floatty = LLVMFloatTypeInContext(g.ctx);
    g.doublety = LLVMDoubleTypeInContext(g.ctx);
    g.voidty = LLVMVoidTypeInContext(g.ctx);
    g.boolty = LLVMInt1TypeInContext(g.ctx);
    g.ptrty = LLVMPointerTypeInContext(g.ctx, 0);
    g.resultty = LLVMStructCreateNamed(g.ctx, "Result");
    LLVMTypeRef result_elems[3] = { g.i32ty, g.doublety, g.ptrty };
    LLVMStructSetBody(g.resultty, result_elems, 3, 0);

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
