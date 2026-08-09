#include "bio.h"

/* Parsing */
/* Out-of-bounds safe: always stops at T_EOF; during error recovery next() never reads past the end of the array */
Tok *peek(Parser *p) {
    if (p->i >= p->n) return &p->t[p->n - 1];
    return &p->t[p->i];
}

Tok *next(Parser *p) {
    Tok *t = peek(p);
    if (p->i < p->n) p->i++;
    return t;
}

int is_op(Parser *p, const char *op) { return peek(p)->kind == T_OP && strcmp(peek(p)->text, op) == 0; }

void expect_op(Parser *p, const char *op) {
    if (!is_op(p, op)) {
        fprintf(stderr, "syntax error: expected '%s', got '%s' (token#%d) | near:", op, peek(p)->text, p->i);
        for (int k = p->i > 3 ? p->i - 3 : 0; k < p->i + 3 && k < p->n; k++)
            fprintf(stderr, " %s", p->t[k].text);
        fprintf(stderr, "\n");
        p->err = 1;
    }
    else next(p);
}

const char *expect_id(Parser *p) {
    if (peek(p)->kind != T_ID) { fprintf(stderr, "syntax error: expected identifier, got %s\n", peek(p)->text); p->err = 1; return ""; }
    return next(p)->text;
}

/* Method name: the keyword new is allowed (e.g. Array::new / Obj::new) */
const char *expect_method_name(Parser *p) {
    if (peek(p)->kind == T_KW && strcmp(peek(p)->text, "new") == 0) return next(p)->text;
    return expect_id(p);
}

Node *parse_expr(Parser *p);

Node *parse_stmt(Parser *p);
static int is_type_name(const char *s);
static int valid_perm(const char *s);
static int valid_follow(const char *s);
static int is_assign_op(const char *s);

Node *parse_if(Parser *p);

Node *parse_while(Parser *p);

Node *parse_for(Parser *p);

Node **parse_stmts_until(Parser *p, const char *end, int *n);
Node *parse_call_plain(Parser *p, const char *fname);

Node *mk_node(NodeKind k) { Node *n = aalloc(sizeof(Node)); n->kind = k; return n; }

Node *parse_primary(Parser *p) {
    Tok *t = next(p);
    Node *e = NULL;
    if (t->kind == T_NUM) { e = mk_node(N_NUM); e->num = t->num; }
    else if (t->kind == T_STR) { e = mk_node(N_STR); e->str = t->text; }
    else if (t->kind == T_KW && strcmp(t->text, "new") == 0) {
        /* new type[12] → array literal (type-agnostic, includes user classes): Obj::new("Array", n) */
        if (peek(p)->kind == T_ID &&
            p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "[") == 0) {
            next(p);                      /* type int / Hero */
            next(p);                      /* [ */
            Node *sz = parse_expr(p);     /* length */
            expect_op(p, "]");
            e = mk_node(N_CALL);
            e->qual = "Obj";
            e->mname = "new";
            e->args = aalloc(sizeof(Node *) * BIO_ARGS_MAX); e->nargs = 0;
            Node *cn = mk_node(N_STR); cn->str = "Array";
            e->args[e->nargs++] = cn;
            e->args[e->nargs++] = sz;
            goto prop_chain;
        }
        /* new Class(args...) → Obj::new("Class", args...): fork a class stream + automatically call __init__ */
        const char *cls = expect_id(p);
        expect_op(p, "(");
        e = mk_node(N_CALL);
        e->qual = "Obj";
        e->mname = "new";
        e->args = aalloc(sizeof(Node *) * BIO_ARGS_MAX); e->nargs = 0;
        Node *cn = mk_node(N_STR); cn->str = cls;
        e->args[e->nargs++] = cn;
        while (!is_op(p, ")") && !p->err) {
            e->args[e->nargs++] = parse_expr(p);
            if (is_op(p, ",")) next(p);
        }
        expect_op(p, ")");
    }
    else if (t->kind == T_KW && strcmp(t->text, "cause") == 0) {
        /* cause X — prefix extraction: the reason a request was refused */
        Node *u = mk_node(N_UNWRAP);
        u->op = t->text;          /* "cause" */
        u->l = parse_primary(p);  /* target: call / variable / literal, etc. */
        e = u;
    }
    else if (t->kind == T_ID && strcmp(t->text, "get") == 0 &&
             p->i + 1 < p->n &&
             !(peek(p)->kind == T_OP &&
               (strcmp(peek(p)->text, "(") == 0 ||
                strcmp(peek(p)->text, "::") == 0 ||
                strcmp(peek(p)->text, ".") == 0 ||
                strcmp(peek(p)->text, "[") == 0))) {
        /* get X — prefix extraction: the actual returned value */
        Node *u = mk_node(N_UNWRAP);
        u->op = "get";
        u->l = parse_primary(p);
        e = u;
    }
    else if (t->kind == T_ID) {
        if (is_op(p, "::")) {
            /* qual::name(...) call, or qual::name property access (this::hp) */
            Node *base = mk_node(N_VAR);
            base->name = t->text;
            next(p); /* :: */
            const char *nm = expect_method_name(p);
            if (is_op(p, "(")) {
                e = mk_node(N_CALL);
                e->qual = t->text;
                e->mname = nm;
                next(p); /* ( */
                e->args = aalloc(sizeof(Node *) * BIO_ARGS_MAX); e->nargs = 0;
                while (!is_op(p, ")") && !p->err) {
                    e->args[e->nargs++] = parse_expr(p);
                    if (is_op(p, ",")) next(p);
                }
                expect_op(p, ")");
            } else {
                e = mk_node(N_PROP);
                e->l = base;
                e->name = nm;
            }
        } else if (is_op(p, "(")) {
            /* bare function call expression: add(a, b) */
            e = parse_call_plain(p, t->text);
        } else if (is_op(p, "[")) {
            /* array index read: a[i] */
            Node *ix = mk_node(N_INDEX);
            Node *base = mk_node(N_VAR); base->name = t->text;
            next(p); /* [ */
            Node *idx = parse_expr(p);
            expect_op(p, "]");
            ix->l = base; ix->r = idx;
            e = ix;
        } else {
            e = mk_node(N_VAR); e->name = t->text;
        }
    }
    else if (t->kind == T_OP && strcmp(t->text, "&") == 0) {
        /* address-of: &<lvalue expression> — creates a reference value;
         * the reference type (&perm follow base) comes from the declaration. */
        Node *rf = mk_node(N_REF);
        rf->l = parse_primary(p);
        e = rf;
    }
    else if (t->kind == T_OP && strcmp(t->text, "(") == 0) {
        e = parse_expr(p);
        expect_op(p, ")");
    }
    else {
        fprintf(stderr, "syntax error: cannot parse expression %s\n", t->text); p->err = 1;
        return mk_node(N_NUM);
    }
    /* property access chain: object fields (request results use get/cause) */
prop_chain:
    while (is_op(p, ".")) {
        next(p);
        Node *prop = mk_node(N_PROP);
        prop->l = e;
        Tok *nt = next(p);
        if (nt->kind != T_ID && nt->kind != T_KW) { fprintf(stderr, "syntax error: invalid property name %s\n", nt->text); p->err = 1; }
        prop->name = nt->text;
        e = prop;
    }
    return e;
}

Node *parse_expr(Parser *p) {
    Node *left = parse_primary(p);
    while (peek(p)->kind == T_OP && strchr("+-*/", peek(p)->text[0]) &&
           (strcmp(peek(p)->text, "::") != 0 && strcmp(peek(p)->text, ".") != 0 &&
            strcmp(peek(p)->text, "==") != 0 && strcmp(peek(p)->text, "!=") != 0 &&
            strcmp(peek(p)->text, "<=") != 0 && strcmp(peek(p)->text, ">=") != 0)) {
        Node *b = mk_node(N_BINOP);
        b->op = next(p)->text; b->l = left; b->r = parse_primary(p);
        left = b;
    }
    /* comparison operators (simplified: last precedence level) */
    while (peek(p)->kind == T_OP &&
           (strcmp(peek(p)->text, "==") == 0 || strcmp(peek(p)->text, "!=") == 0 ||
            strcmp(peek(p)->text, "<=") == 0 || strcmp(peek(p)->text, ">=") == 0 ||
            strcmp(peek(p)->text, "<") == 0  || strcmp(peek(p)->text, ">") == 0)) {
        Node *b = mk_node(N_BINOP);
        b->op = next(p)->text; b->l = left; b->r = parse_primary(p);
        left = b;
    }
    return left;
}

/* bare call: ident(args) — qual left empty, method looked up globally at runtime */
Node *parse_call_plain(Parser *p, const char *fname) {
    Node *e = mk_node(N_CALL);
    e->qual = NULL;
    e->mname = fname;
    expect_op(p, "(");
    e->args = aalloc(sizeof(Node *) * BIO_ARGS_MAX); e->nargs = 0;
    while (!is_op(p, ")") && !p->err) {
        e->args[e->nargs++] = parse_expr(p);
        if (is_op(p, ",")) next(p);
    }
    expect_op(p, ")");
    return e;
}

Node *parse_call(Parser *p, const char *qual) {
    Node *e = mk_node(N_CALL);
    e->qual = qual;
    expect_op(p, "::");
    e->mname = expect_method_name(p);
    expect_op(p, "(");
    e->args = aalloc(sizeof(Node *) * BIO_ARGS_MAX); e->nargs = 0;
    while (!is_op(p, ")") && !p->err) {
        e->args[e->nargs++] = parse_expr(p);
        if (is_op(p, ",")) next(p);
    }
    expect_op(p, ")");
    return e;
}

int peek_kw(Parser *p, const char *kw) {
    return peek(p)->kind == T_KW && strcmp(peek(p)->text, kw) == 0;
}

Node *parse_if(Parser *p) {
    next(p); /* if */
    Node *n = mk_node(N_IF);
    expect_op(p, "(");
    n->cond = parse_expr(p);
    expect_op(p, ")");
    expect_op(p, "{");
    n->stmts = parse_stmts_until(p, "}", &n->nstmts);
    if (peek_kw(p, "else")) {
        next(p);
        n->has_else = 1;
        if (peek_kw(p, "if")) {                 /* else if → put an N_IF in the else branch */
            n->else_stmts = aalloc(sizeof(Node *));
            n->else_stmts[0] = parse_if(p);
            n->n_else = 1;
        } else {
            expect_op(p, "{");
            n->else_stmts = parse_stmts_until(p, "}", &n->n_else);
        }
    }
    return n;
}

Node *parse_while(Parser *p) {
    next(p); /* while */
    Node *n = mk_node(N_WHILE);
    expect_op(p, "(");
    n->cond = parse_expr(p);
    expect_op(p, ")");
    expect_op(p, "{");
    n->stmts = parse_stmts_until(p, "}", &n->nstmts);
    return n;
}

Node *parse_for(Parser *p) {
    next(p); /* for */
    Node *n = mk_node(N_FOR);
    expect_op(p, "(");
    if (is_op(p, ";")) next(p);                 /* for(;;) empty init */
    else n->init = parse_stmt(p);
    if (is_op(p, ";")) next(p);                 /* empty condition */
    else { n->cond = parse_expr(p); expect_op(p, ";"); }
    if (!is_op(p, ")")) n->update = parse_stmt(p);
    expect_op(p, ")");
    expect_op(p, "{");
    n->stmts = parse_stmts_until(p, "}", &n->nstmts);
    return n;
}

Node *parse_stmt(Parser *p) {
    Tok *t = peek(p);
    if (t->kind == T_KW && strcmp(t->text, "if") == 0) return parse_if(p);
    if (t->kind == T_KW && strcmp(t->text, "while") == 0) return parse_while(p);
    if (t->kind == T_KW && strcmp(t->text, "for") == 0) return parse_for(p);
    if (t->kind == T_KW && strcmp(t->text, "break") == 0) {
        next(p); expect_op(p, ";"); return mk_node(N_BREAK);
    }
    if (t->kind == T_KW && strcmp(t->text, "continue") == 0) {
        next(p); expect_op(p, ";"); return mk_node(N_CONTINUE);
    }
    if (t->kind == T_KW && (strcmp(t->text, "res") == 0 || strcmp(t->text, "ref") == 0)) {
        next(p);
        Node *n = mk_node(N_RET);
        n->retkind = t->text;   /* "res" = respond, "ref" = refuse */
        n->nrets = 0;
        n->expr = parse_expr(p);
        if (strcmp(t->text, "res") == 0 && is_op(p, ",")) {
            /* res multi-value: res a, b, c; → returns an array */
            Node **rs = aalloc(sizeof(Node *) * BIO_ARGS_MAX);
            int nr = 0;
            rs[nr++] = n->expr;
            while (is_op(p, ",")) {
                next(p);
                rs[nr++] = parse_expr(p);
            }
            n->rets = rs; n->nrets = nr;
        }
        expect_op(p, ";");
        return n;
    }
    if (t->kind == T_KW && (strcmp(t->text, "const") == 0 || strcmp(t->text, "thread") == 0)) {
        /* variable modifier (original spec): const int x = 10; (→ Constantstream) / thread int x = 10; (→ thread variable) */
        int is_c = strcmp(t->text, "const") == 0;
        next(p); /* const / thread */
        if (peek(p)->kind != T_ID || !is_type_name(peek(p)->text)) {
            fprintf(stderr, "syntax error: %s requires a type (e.g. %s int x = 10;)\n", t->text, t->text);
            p->err = 1;
            return mk_node(N_BREAK);
        }
        next(p); /* type */
        Node *n = mk_node(N_ASSIGN);
        n->is_const = is_c;
        n->is_thread = !is_c;
        n->name = expect_id(p);
        expect_op(p, "=");
        n->expr = parse_expr(p);
        expect_op(p, ";");
        return n;
    }
    if (t->kind == T_KW && strcmp(t->text, "ALL") == 0) {
        next(p);
        Node *n = mk_node(N_ASSIGN);
        n->name = expect_id(p);
        expect_op(p, "=");
        n->expr = parse_expr(p);
        expect_op(p, ";");
        return n;
    }
    if (t->kind == T_OP && strcmp(t->text, "&") == 0) {
        next(p); /* & */
        /* Reference variable declaration — the reference is a typed value:
         * &<perm> <follow> <base type> <name> = &<lvalue expression>; */
        Node *n = mk_node(N_REALME);
        if (peek(p)->kind != T_ID) {
            fprintf(stderr, "syntax error: expected reference permission (r/w/rw/m)\n");
            p->err = 1;
            return mk_node(N_NUM);
        }
        n->ref_perm = next(p)->text;
        if (!valid_perm(n->ref_perm)) {
            fprintf(stderr, "syntax error: invalid reference permission %s (stack of r/w/m: r, w, m, rw, rm, wm, rwm)\n", n->ref_perm); p->err = 1;
        }
        if (peek(p)->kind != T_ID) {
            fprintf(stderr, "syntax error: expected reference follow (u/f/a/t)\n");
            p->err = 1;
            return mk_node(N_NUM);
        }
        n->ref_follow = next(p)->text;
        if (!valid_follow(n->ref_follow)) {
            fprintf(stderr, "syntax error: invalid reference follow %s (should be u/f/a/t)\n", n->ref_follow); p->err = 1;
        }
        if (peek(p)->kind != T_ID) {
            fprintf(stderr, "syntax error: reference declaration requires a base type (e.g. &rw u int p = &a[0];)\n");
            p->err = 1;
        } else {
            n->ref_type = next(p)->text;          /* base type: int / double / string / Hero / ... */
            if (is_op(p, "[")) { next(p); expect_op(p, "]"); }
        }
        n->name = expect_id(p);
        expect_op(p, "=");
        n->init = parse_expr(p);
        if (!n->init || n->init->kind != N_REF) {
            fprintf(stderr, "syntax error: reference declaration requires &<expression> (e.g. &rw u int p = &a[0];)\n");
            p->err = 1;
        } else {
            n->init->ref_perm = n->ref_perm;      /* the address-of value carries the declared permissions */
            n->init->ref_follow = n->ref_follow;
        }
        expect_op(p, ";");
        return n;
    }
    if (t->kind == T_ID) {
        /* array-typed variable declaration: int[] a = expr; / int[] a;  (must be checked before the array-index case) */
        if (p->i + 2 < p->n && p->t[p->i].kind == T_ID && is_type_name(p->t[p->i].text) &&
            p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "[") == 0 &&
            p->t[p->i + 2].kind == T_OP && strcmp(p->t[p->i + 2].text, "]") == 0) {
            next(p); next(p); next(p);   /* int [ ] */
            Node *n = mk_node(N_ASSIGN);
            n->name = expect_id(p);
            if (is_op(p, "=")) {
                next(p);
                n->expr = parse_expr(p);
            }
            expect_op(p, ";");
            return n;
        }
        /* increment/decrement: i++; / i--;  (a reference variable moves its pointer) */
        if (p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP &&
            (strcmp(p->t[p->i + 1].text, "++") == 0 || strcmp(p->t[p->i + 1].text, "--") == 0)) {
            const char *vn = t->text;              /* variable name = current token (i) */
            const char *inc = p->t[p->i + 1].text; /* ++ / -- (next token) */
            next(p); next(p);                      /* consume i and ++ */
            Node *n = mk_node(N_INC);
            n->name = vn;
            n->inc_op = inc;
            if (!is_op(p, ";")) return n;   /* semicolon optional in the for-update clause, e.g. i++) */
            next(p);
            return n;
        }
        /* array index assignment: a[i] = v; / a[i] += v; */
        if (p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "[") == 0) {
            const char *arr = next(p)->text;         /* array name */
            next(p);                                 /* [ */
            Node *idx = parse_expr(p);
            expect_op(p, "]");
            Node *tgt = mk_node(N_INDEX);
            Node *base = mk_node(N_VAR); base->name = arr;
            tgt->l = base; tgt->r = idx;
            if (is_op(p, "=") || is_op(p, "+=") || is_op(p, "-=") ||
                is_op(p, "*=") || is_op(p, "/=") || is_op(p, "%=")) {
                Node *n = mk_node(N_ASSIGN);
                n->op = next(p)->text;
                n->target = tgt;
                n->expr = parse_expr(p);
                expect_op(p, ";");
                return n;
            }
            /* read-only (no side effects) */
            Node *n = mk_node(N_CALLSTMT);
            n->l = tgt;
            expect_op(p, ";");
            return n;
        }
        /* assignment: x = e;   or   type prefix: int x = e;   or   compound assignment: x += e; / x -= e; etc. */
        if (p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP && is_assign_op(p->t[p->i + 1].text)) {
            Node *n = mk_node(N_ASSIGN);
            n->name = next(p)->text;
            n->op = next(p)->text; /* = / += / -= / *= / /=  */
            n->expr = parse_expr(p);
            expect_op(p, ";");
            return n;
        }
        if (p->i + 2 < p->n && p->t[p->i + 1].kind == T_ID && p->t[p->i + 2].kind == T_OP &&
            is_assign_op(p->t[p->i + 2].text)) {
            next(p); /* type */
            Node *n = mk_node(N_ASSIGN);
            n->name = next(p)->text;   /* variable name */
            n->op = next(p)->text;     /* = / += / -= etc. */
            n->expr = parse_expr(p);
            expect_op(p, ";");
            return n;
        }
        if (p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "::") == 0) {
            const char *qual = next(p)->text;   /* stream name / this */
            next(p); /* :: */
            const char *nm = expect_method_name(p);
            if (is_op(p, "=") || is_op(p, "+=") || is_op(p, "-=") || is_op(p, "*=") ||
                is_op(p, "/=") || is_op(p, "%=")) {   /* this::attr = v; / this::attr += v; etc. */
                Node *n = mk_node(N_ASSIGN);
                n->op = next(p)->text;   /* = / += / -= etc. */
                Node *tgt = mk_node(N_PROP);
                Node *base = mk_node(N_VAR);
                base->name = qual;
                tgt->l = base;
                tgt->name = nm;
                n->target = tgt;
                n->expr = parse_expr(p);
                expect_op(p, ";");
                return n;
            }
            Node *n = mk_node(N_CALLSTMT);
            Node *c = mk_node(N_CALL);
            c->qual = qual;
            c->mname = nm;
            expect_op(p, "(");
            c->args = aalloc(sizeof(Node *) * BIO_ARGS_MAX); c->nargs = 0;
            while (!is_op(p, ")") && !p->err) {
                c->args[c->nargs++] = parse_expr(p);
                if (is_op(p, ",")) next(p);
            }
            expect_op(p, ")");
            n->l = c;
            expect_op(p, ";");
            return n;
        }
        if (p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "(") == 0) {
            /* bare function call statement: add(a, b); */
            const char *fname = next(p)->text;
            Node *n = mk_node(N_CALLSTMT);
            n->l = parse_call_plain(p, fname);
            expect_op(p, ";");
            return n;
        }
    }
    fprintf(stderr, "syntax error: cannot parse statement %s\n", t->text);
    p->err = 1;
    return mk_node(N_NUM);
}

Node **parse_stmts_until(Parser *p, const char *end, int *n) {
    Node **stmts = aalloc(sizeof(Node *) * BIO_STMTS_MAX);
    int n2 = 0;
    while (!(peek(p)->kind == T_OP && strcmp(peek(p)->text, end) == 0)) {
        stmts[n2++] = parse_stmt(p);
        if (p->err) break;
    }
    if (!p->err) next(p);   /* consume the terminator */
    *n = n2;
    return stmts;
}

static int is_type_name(const char *s) {
    return strcmp(s, "int") == 0 || strcmp(s, "float") == 0 || strcmp(s, "double") == 0 ||
           strcmp(s, "string") == 0 || strcmp(s, "char") == 0;
}

/* smart reference permissions: any stack of r (read) / w (write) / m (move):
 * r, w, m, rw, rm, wm, rwm — 7 combinations */
static int valid_perm(const char *s) {
    return strcmp(s, "r") == 0 || strcmp(s, "w") == 0 || strcmp(s, "m") == 0 ||
           strcmp(s, "rw") == 0 || strcmp(s, "rm") == 0 || strcmp(s, "wm") == 0 ||
           strcmp(s, "rwm") == 0;
}

/* smart reference follow layer: u program / f method / a area / t thread */
static int valid_follow(const char *s) {
    return strcmp(s, "u") == 0 || strcmp(s, "f") == 0 || strcmp(s, "a") == 0 ||
           strcmp(s, "t") == 0;
}

/* assignment operators: = / compound assignments += -= *= /= %= */
static int is_assign_op(const char *s) {
    return strcmp(s, "=") == 0 || strcmp(s, "+=") == 0 || strcmp(s, "-=") == 0 ||
           strcmp(s, "*=") == 0 || strcmp(s, "/=") == 0 || strcmp(s, "%=") == 0;
}

/* Parameter syntax (only form): parameter name parameter type, e.g. void add(a int, b int); array a int[];
 * The type can be a primitive type, or any type name / stream name (e.g. void show(cio CIO)).
 * Smart reference modifiers are also supported: void show(&r f io IO) — &permission follow parameter name type. */
void parse_params(Parser *p, const char ***params, int *n) {
    const char **ps = aalloc(sizeof(char *) * 64);
    int n2 = 0;
    while (!is_op(p, ")") && !p->err) {
        /* &permission follow parameter name type */
        if (is_op(p, "&")) {
            next(p);
            next(p);                      /* permission r/w/rw/m */
            next(p);                      /* follow u/f/a */
        }
        Tok *t = next(p);
        if (t->kind == T_ID) {
            if (is_type_name(t->text)) {
                fprintf(stderr, "syntax error: parameter syntax is 'name type' (e.g. a int), not %s\n", t->text);
                p->err = 1; break;
            }
            ps[n2++] = t->text;
            if (is_op(p, "[")) { next(p); expect_op(p, "]"); }
            /* type: primitive type or any identifier (stream name / class name) */
            if (peek(p)->kind == T_ID) {
                if (is_type_name(peek(p)->text)) next(p);
                else { next(p); if (is_op(p, "[")) { next(p); expect_op(p, "]"); } }
            }
        }
        if (is_op(p, ",")) next(p);
    }
    *params = ps; *n = n2;
}

/* return type: void or a type name (may carry a [] array suffix, e.g. int[]). Returns NULL when not at the start of a method */
static const char *parse_ret_type(Parser *p) {
    Tok *t = peek(p);
    if (t->kind == T_KW && strcmp(t->text, "void") == 0) {
        next(p);
        return "void";
    }
    if (t->kind == T_ID && is_type_name(t->text)) {
        next(p);
        char buf[BIO_NAME_MAX];
        snprintf(buf, sizeof(buf), "%s", t->text);
        if (is_op(p, "[")) { next(p); expect_op(p, "]"); strcat(buf, "[]"); }
        return astrdup(buf);
    }
    return NULL;
}

/* Annotations written after a method:  void m() {...} @read/@write */
static void parse_method_annotations(Parser *p, Method *m) {
    while (is_op(p, "@")) {
        next(p);
        const char *a = expect_id(p);
        if (strcmp(a, "write") == 0) m->write = 1;
        else if (strcmp(a, "read") == 0) m->read = 1;
        else if (strcmp(a, "call") == 0) m->call = 1;
        else if (strcmp(a, "ucall") == 0) m->ucall = 1;
        else {
            fprintf(stderr, "syntax error: unknown method annotation @%s (only @read/@write/@call/@ucall)\n", a);
            p->err = 1;
        }
    }
}

/* Annotations written after a stream/class/fork declaration:  FIO r {} @onlyread @unfork */
static void parse_decl_annotations(Parser *p, Decl *d) {
    while (is_op(p, "@")) {
        next(p);
        const char *a = expect_id(p);
        if (strcmp(a, "onlyread") == 0) d->onlyread = 1;
        else if (strcmp(a, "unfork") == 0) d->unfork = 1;
        else {
            fprintf(stderr, "syntax error: unknown stream annotation @%s (only @onlyread/@unfork)\n", a);
            p->err = 1;
        }
    }
}

void parse_methods(Parser *p, Method **methods, int *n) {
    Method *ms = aalloc(sizeof(Method) * BIO_METHODS_MAX);
    int n2 = 0;
    while (!is_op(p, "}")) {
        const char *rt = parse_ret_type(p);
        if (!rt) {
            fprintf(stderr, "syntax error: expected return type (void/int/float/double/string/char, with optional []), got %s\n", peek(p)->text);
            p->err = 1; break;
        }
        Method *m = &ms[n2++];
        m->ret_type = rt;
        m->name = expect_id(p);
        expect_op(p, "(");
        parse_params(p, &m->params, &m->nparams);
        expect_op(p, ")");
        expect_op(p, "{");
        m->stmts = parse_stmts_until(p, "}", &m->nstmts);
        parse_method_annotations(p, m);
        if (p->err) break;
    }
    *methods = ms; *n = n2;
}

/* ═══════════════ Member parsing (common to classes / signature streams / forked streams) ═══════════════
 * Methods and fields may interleave in any order (the order is not strict); fields support comma separation:
 *   void m(params) { ... }     method (implementation)
 *   void m(params);            method (signature)
 *   int add(a int) { ... }     method with a return type
 *   int x, y;  int[] a;  string s;   field (type name[, name...];)
 *   type T;                    generic type declaration (only the name is registered at the prototype stage)
 *   T n;  T[] a;  T m() {...}  generic-style field/method
 */
static void parse_members(Parser *p, Method **methods, int *nmethods, Field **fields, int *nfields) {
    Method *ms = aalloc(sizeof(Method) * BIO_METHODS_MAX);
    Field *fs = aalloc(sizeof(Field) * BIO_FIELDS_MAX);
    int nm = 0, nf = 0;
    while (!is_op(p, "}") && !p->err) {
        Tok *t = peek(p);
        if (t->kind == T_KW && strcmp(t->text, "void") == 0) {
            next(p);
            Method *m = &ms[nm++];
            m->ret_type = "void";
            m->name = expect_id(p);
            expect_op(p, "(");
            parse_params(p, &m->params, &m->nparams);
            expect_op(p, ")");
            if (is_op(p, "{")) {
                next(p);
                m->stmts = parse_stmts_until(p, "}", &m->nstmts);
            } else {
                expect_op(p, ";");
                m->stmts = NULL; m->nstmts = 0;   /* signature */
            }
            parse_method_annotations(p, m);
        } else if (t->kind == T_ID && is_type_name(t->text)) {
            /* primitive type: a method with a return type, or a field (int x, y; / int[] a;) */
            const char *rt = parse_ret_type(p);
            const char *name = expect_id(p);
            if (is_op(p, "(")) {
                next(p);
                Method *m = &ms[nm++];
                m->ret_type = rt;
                m->name = name;
                parse_params(p, &m->params, &m->nparams);
                expect_op(p, ")");
                if (is_op(p, "{")) { next(p); m->stmts = parse_stmts_until(p, "}", &m->nstmts); }
                else { expect_op(p, ";"); m->stmts = NULL; m->nstmts = 0; }
                parse_method_annotations(p, m);
            } else {
                fs[nf].name = name; fs[nf].type = rt; nf++;
                while (is_op(p, ",")) {
                    next(p);
                    fs[nf].name = expect_id(p); fs[nf].type = rt; nf++;
                }
                expect_op(p, ";");
            }
        } else if (t->kind == T_KW && strcmp(t->text, "type") == 0) {
            next(p);                       /* type */
            expect_id(p);                  /* generic name (not instantiated at the prototype stage) */
            expect_op(p, ";");
        } else if (t->kind == T_ID) {
            /* generic style: T n; / T[] a; / T m() {...} */
            const char *ty = next(p)->text;
            int is_arr = 0;
            if (is_op(p, "[")) { next(p); expect_op(p, "]"); is_arr = 1; }
            const char *name = expect_id(p);
            char buf[BIO_NAME_MAX];
            snprintf(buf, sizeof buf, "%s%s", ty, is_arr ? "[]" : "");
            if (is_op(p, "(")) {
                next(p);
                Method *m = &ms[nm++];
                m->ret_type = astrdup(buf);
                m->name = name;
                parse_params(p, &m->params, &m->nparams);
                expect_op(p, ")");
                if (is_op(p, "{")) { next(p); m->stmts = parse_stmts_until(p, "}", &m->nstmts); }
                else { expect_op(p, ";"); m->stmts = NULL; m->nstmts = 0; }
                parse_method_annotations(p, m);
            } else {
                fs[nf].name = name; fs[nf].type = astrdup(buf); nf++;
                while (is_op(p, ",")) {
                    next(p);
                    fs[nf].name = expect_id(p); fs[nf].type = astrdup(buf); nf++;
                }
                expect_op(p, ";");
            }
        } else {
            fprintf(stderr, "syntax error: cannot parse stream/class member %s\n", t->text);
            p->err = 1; break;
        }
    }
    if (!p->err) next(p);   /* consume } */
    *methods = ms; *nmethods = nm;
    *fields = fs; *nfields = nf;
}

/* skip { ... } block (details assumed by need; only names are registered at the prototype stage) */
static void skip_block(Parser *p) {
    if (!is_op(p, "{")) return;
    int depth = 0;
    do {
        Tok *t = next(p);
        if (t->kind == T_EOF) break;
        if (t->kind == T_OP && strcmp(t->text, "{") == 0) depth++;
        else if (t->kind == T_OP && strcmp(t->text, "}") == 0) depth--;
    } while (depth > 0);
}

Decl *parse_program(Parser *p) {
    Decl *head = NULL, *tail = NULL;
    Decl *mains = NULL, *mtail = NULL;
    while (peek(p)->kind != T_EOF && !p->err) {
        Tok *t = peek(p);
        Decl *d = aalloc(sizeof(Decl));
        d->next = NULL;
        if (t->kind == T_KW && strcmp(t->text, "program") == 0) {
            next(p); expect_id(p); expect_op(p, ";");
            continue;   /* the prototype only declares it; main/utils are not distinguished */
        }
        if (t->kind == T_KW && strcmp(t->text, "const") == 0) {
            /* top-level const declaration (original spec: const → Constantstream public constant) */
            next(p);
            if (peek(p)->kind != T_ID || !is_type_name(peek(p)->text)) {
                fprintf(stderr, "syntax error: const requires a type (e.g. const int x = 10;)\n"); p->err = 1;
            } else next(p);   /* type */
            d->kind = D_CONST;
            d->name = expect_id(p);
            expect_op(p, "=");
            d->init = parse_expr(p);
            expect_op(p, ";");
            goto decl_done;
        }
        if (t->kind == T_KW && strcmp(t->text, "need") == 0) {
            next(p);
            Tok *k = next(p);
            if (strcmp(k->text, "stream") == 0 || strcmp(k->text, "Stream") == 0) {
                d->kind = D_NEED; d->needkind = "stream";
                d->name = expect_id(p);
                skip_block(p);
                if (is_op(p, ";")) next(p);
            } else if (strcmp(k->text, "value") == 0 || strcmp(k->text, "function") == 0 ||
                       strcmp(k->text, "Class") == 0) {
                d->kind = D_NEED; d->needkind = k->text;
                d->name = expect_id(p);
                if (is_op(p, "{")) {           /* original spec: need Class ... {...}; */
                    skip_block(p);
                    if (is_op(p, ";")) next(p);
                } else expect_op(p, ";");
            } else {
                fprintf(stderr, "syntax error: need only supports value/function/stream/Class\n"); p->err = 1;
            }
        }
        else if (t->kind == T_KW && strcmp(t->text, "Stream") == 0) {
            next(p);
            d->name = expect_id(p);
            if (is_op(p, "&")) {                 /* Stream <name> & <binary library file> { normal stream body } */
                next(p);
                d->kind = D_BIN;
                Tok *ft = next(p);
                if (ft->kind != T_STR && ft->kind != T_ID) {
                    fprintf(stderr, "syntax error: expected binary library file name\n"); p->err = 1;
                }
                d->file = ft->text;
                expect_op(p, "{");
                /* De-special-cased: the body is a normal stream body (methods + fields),
                 * exactly like any other Stream declaration. */
                parse_members(p, &d->methods, &d->nmethods, &d->fields, &d->nfields);
                parse_decl_annotations(p, d);
                goto decl_done;
            }
            d->kind = D_SIG;
            expect_op(p, "{");
            parse_members(p, &d->methods, &d->nmethods, &d->fields, &d->nfields);
            parse_decl_annotations(p, d);
        }
        else if (t->kind == T_KW && strcmp(t->text, "Class") == 0) {
            next(p);
            d->kind = D_CLASS;
            d->name = expect_id(p);
            expect_op(p, "{");
            /* methods and fields parsed uniformly (Objstream uses methods; object properties use fields) */
            parse_members(p, &d->methods, &d->nmethods, &d->fields, &d->nfields);
            parse_decl_annotations(p, d);
        }
        else if (t->kind == T_KW && strcmp(t->text, "Main") == 0) {
            next(p);
            d->kind = D_MAIN;
            expect_op(p, "{");
            parse_methods(p, &d->methods, &d->nmethods);
            expect_op(p, "}");
            if (mtail) { mtail->next = d; mtail = d; } else { mains = mtail = d; }
            continue;
        }
        else if (t->kind == T_ID) {
            /* fork: signature stream implementation stream { void m() {...} }  (custom fields allowed) */
            d->kind = D_FORK;
            d->sig = expect_id(p);
            d->name = expect_id(p);
            expect_op(p, "{");
            parse_members(p, &d->methods, &d->nmethods, &d->fields, &d->nfields);
            parse_decl_annotations(p, d);
        }
        else {
            fprintf(stderr, "syntax error: cannot parse top-level declaration %s\n", t->text); p->err = 1; break;
        }
    decl_done:
        if (tail) { tail->next = d; tail = d; } else { head = tail = d; }
    }
    /* append Main after the decls (stored separately, for ease of execution) */
    if (mains) {
        if (tail) tail->next = mains; else head = mains;
    }
    return head;
}

Decl *parse_program_tokens(Tok *toks, int n, int *err) {
    Parser p = { toks, 0, n, 0 };
    Decl *d = parse_program(&p);
    if (err) *err = p.err;
    return d;
}
