#include "bio.h"

/* 语法分析 */
Tok *peek(Parser *p) { return &p->t[p->i]; }

Tok *next(Parser *p) { return &p->t[p->i++]; }

int is_op(Parser *p, const char *op) { return peek(p)->kind == T_OP && strcmp(peek(p)->text, op) == 0; }

void expect_op(Parser *p, const char *op) {
    if (!is_op(p, op)) {
        fprintf(stderr, "语法错误: 期望 '%s'，得到 '%s' (token#%d) | 附近:", op, peek(p)->text, p->i);
        for (int k = p->i > 3 ? p->i - 3 : 0; k < p->i + 3 && k < p->n; k++)
            fprintf(stderr, " %s", p->t[k].text);
        fprintf(stderr, "\n");
        p->err = 1;
    }
    else next(p);
}

const char *expect_id(Parser *p) {
    if (peek(p)->kind != T_ID) { fprintf(stderr, "语法错误: 期望标识符，得到 %s\n", peek(p)->text); p->err = 1; return ""; }
    return next(p)->text;
}

/* 方法名：允许关键字 new（如 Array::new / Obj::new） */
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
        /* new int[12] → 数组字面量：Obj::new("Array", n) */
        if (peek(p)->kind == T_ID && is_type_name(peek(p)->text) &&
            p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "[") == 0) {
            next(p);                      /* 类型 int */
            next(p);                      /* [ */
            Node *sz = parse_expr(p);     /* 长度 */
            expect_op(p, "]");
            e = mk_node(N_CALL);
            e->qual = "Obj";
            e->mname = "new";
            e->args = aalloc(sizeof(Node *) * 64); e->nargs = 0;
            Node *cn = mk_node(N_STR); cn->str = "Array";
            e->args[e->nargs++] = cn;
            e->args[e->nargs++] = sz;
            goto prop_chain;
        }
        /* new Class(args...) → Obj::new("Class", args...)：分叉类流 + 自动调 __init__ */
        const char *cls = expect_id(p);
        expect_op(p, "(");
        e = mk_node(N_CALL);
        e->qual = "Obj";
        e->mname = "new";
        e->args = aalloc(sizeof(Node *) * 64); e->nargs = 0;
        Node *cn = mk_node(N_STR); cn->str = cls;
        e->args[e->nargs++] = cn;
        while (!is_op(p, ")")) {
            e->args[e->nargs++] = parse_expr(p);
            if (is_op(p, ",")) next(p);
        }
        expect_op(p, ")");
    }
    else if (t->kind == T_ID) {
        if (is_op(p, "::")) {
            /* qual::name(...) 调用 或 qual::name 属性访问（this::hp） */
            Node *base = mk_node(N_VAR);
            base->name = t->text;
            next(p); /* :: */
            const char *nm = expect_method_name(p);
            if (is_op(p, "(")) {
                e = mk_node(N_CALL);
                e->qual = t->text;
                e->mname = nm;
                next(p); /* ( */
                e->args = aalloc(sizeof(Node *) * 64); e->nargs = 0;
                while (!is_op(p, ")")) {
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
            /* 裸函数调用表达式: add(a, b) */
            e = parse_call_plain(p, t->text);
        } else {
            e = mk_node(N_VAR); e->name = t->text;
        }
    }
    else if (t->kind == T_OP && strcmp(t->text, "&") == 0) {
        /* 智能引用: &<权限> <跟随> <真名>   或   二进制调用: &func(...) */
        if (peek(p)->kind == T_ID && p->i + 2 < p->n && p->t[p->i + 2].kind == T_ID &&
            !(p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "(") == 0)) {
            Node *rf = mk_node(N_REF);
            rf->ref_perm = next(p)->text;      /* r / w / rw / m */
            if (!valid_perm(rf->ref_perm)) {
                fprintf(stderr, "语法错误: 非法引用权限 %s（应为 r/w/rw/m）\n", rf->ref_perm); p->err = 1;
            }
            rf->ref_follow = next(p)->text;    /* u / f / a */
            if (!valid_follow(rf->ref_follow)) {
                fprintf(stderr, "语法错误: 非法引用跟随 %s（应为 u/f/a）\n", rf->ref_follow); p->err = 1;
            }
            rf->ref_name = expect_id(p);       /* 真名 */
            e = rf;
            return e;
        }
        Node *b = mk_node(N_BINCALL);
        b->mname = expect_id(p);
        expect_op(p, "(");
        b->args = aalloc(sizeof(Node *) * 64); b->nargs = 0;
        while (!is_op(p, ")")) {
            b->args[b->nargs++] = parse_expr(p);
            if (is_op(p, ",")) next(p);
        }
        expect_op(p, ")");
        e = b;
    }
    else if (t->kind == T_OP && strcmp(t->text, "(") == 0) {
        e = parse_expr(p);
        expect_op(p, ")");
    }
    else {
        fprintf(stderr, "语法错误: 无法解析表达式 %s\n", t->text); p->err = 1;
        return mk_node(N_NUM);
    }
    /* 属性访问链: .res / .ref / .字段 */
prop_chain:
    while (is_op(p, ".")) {
        next(p);
        Node *prop = mk_node(N_PROP);
        prop->l = e;
        Tok *nt = next(p);
        if (nt->kind != T_ID && nt->kind != T_KW) { fprintf(stderr, "语法错误: 属性名无效 %s\n", nt->text); p->err = 1; }
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
    /* 比较运算符（简化：最后一级） */
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

/* 裸调用：ident(args) —— qual 留空，运行时全局找方法 */
Node *parse_call_plain(Parser *p, const char *fname) {
    Node *e = mk_node(N_CALL);
    e->qual = NULL;
    e->mname = fname;
    expect_op(p, "(");
    e->args = aalloc(sizeof(Node *) * 64); e->nargs = 0;
    while (!is_op(p, ")")) {
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
    e->args = aalloc(sizeof(Node *) * 64); e->nargs = 0;
    while (!is_op(p, ")")) {
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
        if (peek_kw(p, "if")) {                 /* else if → else 分支放一个 N_IF */
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
    if (is_op(p, ";")) next(p);                 /* for(;;) 空 init */
    else n->init = parse_stmt(p);
    if (is_op(p, ";")) next(p);                 /* 空条件 */
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
    if (t->kind == T_KW && (strcmp(t->text, "res") == 0 || strcmp(t->text, "ref") == 0 ||
                            strcmp(t->text, "cause") == 0)) {
        next(p);
        Node *n = mk_node(N_RET);
        n->retkind = strcmp(t->text, "cause") == 0 ? "ref" : t->text;   /* 原稿: cause = 拒绝 */
        n->nrets = 0;
        if (strcmp(t->text, "ref") == 0) {
            if (!(peek(p)->kind == T_KW && strcmp(peek(p)->text, "cause") == 0)) {
                fprintf(stderr, "语法错误: ref 必须带 cause（ref cause <原因>;）\n");
                p->err = 1;
            }
            next(p);               /* cause */
        }
        n->expr = parse_expr(p);
        if (strcmp(t->text, "res") == 0 && is_op(p, ",")) {
            /* res 多值: res a, b, c; → 返回数组 */
            Node **rs = aalloc(sizeof(Node *) * 64);
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
        /* 变量修饰（原稿）：const int x = 10;（→ Constantstream） / thread int x = 10;（→ 线程变量） */
        int is_c = strcmp(t->text, "const") == 0;
        next(p); /* const / thread */
        if (peek(p)->kind != T_ID || !is_type_name(peek(p)->text)) {
            fprintf(stderr, "语法错误: %s 后需要类型（如 %s int x = 10;）\n", t->text, t->text);
            p->err = 1;
            return mk_node(N_BREAK);
        }
        next(p); /* 类型 */
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
        Node *n = mk_node(N_CALLSTMT);
        Node *b = mk_node(N_BINCALL);
        next(p); /* & */
        b->mname = expect_id(p);
        expect_op(p, "(");
        b->args = aalloc(sizeof(Node *) * 64); b->nargs = 0;
        while (!is_op(p, ")")) {
            b->args[b->nargs++] = parse_expr(p);
            if (is_op(p, ",")) next(p);
        }
        expect_op(p, ")");
        n->l = b;
        expect_op(p, ";");
        return n;
    }
    if (t->kind == T_ID) {
        /* 引用变量声明（原稿 realme &权限 跟随 类型）：名字 &权限 跟随 [类型] [= 初值]; */
        if (p->i + 2 < p->n && p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "&") == 0) {
            Node *n = mk_node(N_REALME);
            n->name = next(p)->text;              /* 名字 */
            next(p);                              /* & */
            n->ref_perm = next(p)->text;
            if (!valid_perm(n->ref_perm)) {
                fprintf(stderr, "语法错误: 非法引用权限 %s（应为 r/w/rw/m）\n", n->ref_perm); p->err = 1;
            }
            n->ref_follow = next(p)->text;
            if (!valid_follow(n->ref_follow)) {
                fprintf(stderr, "语法错误: 非法引用跟随 %s（应为 u/f/a）\n", n->ref_follow); p->err = 1;
            }
            if (peek(p)->kind == T_ID && is_type_name(peek(p)->text)) {
                next(p);                          /* 类型（校验用，可带 []） */
                if (is_op(p, "[")) { next(p); expect_op(p, "]"); }
            }
            if (is_op(p, "=")) {                  /* 初值：写入跟随层同名槽位 */
                next(p);
                n->init = parse_expr(p);
            }
            expect_op(p, ";");
            return n;
        }
        /* 数组类型变量声明: int[] a = expr; / int[] a; */
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
        /* 赋值: x = e;  或  类型前缀: int x = e;  或  复合赋值: x += e; / x -= e; 等 */
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
            next(p); /* 类型 */
            Node *n = mk_node(N_ASSIGN);
            n->name = next(p)->text;   /* 变量名 */
            n->op = next(p)->text;     /* = / += / -= 等 */
            n->expr = parse_expr(p);
            expect_op(p, ";");
            return n;
        }
        if (p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "::") == 0) {
            const char *qual = next(p)->text;   /* 流名 / this */
            next(p); /* :: */
            const char *nm = expect_method_name(p);
            if (is_op(p, "=") || is_op(p, "+=") || is_op(p, "-=") || is_op(p, "*=") ||
                is_op(p, "/=") || is_op(p, "%=")) {   /* this::attr = v; / this::attr += v; 等 */
                Node *n = mk_node(N_ASSIGN);
                n->op = next(p)->text;   /* = / += / -= 等 */
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
            c->args = aalloc(sizeof(Node *) * 64); c->nargs = 0;
            while (!is_op(p, ")")) {
                c->args[c->nargs++] = parse_expr(p);
                if (is_op(p, ",")) next(p);
            }
            expect_op(p, ")");
            n->l = c;
            expect_op(p, ";");
            return n;
        }
        if (p->i + 1 < p->n && p->t[p->i + 1].kind == T_OP && strcmp(p->t[p->i + 1].text, "(") == 0) {
            /* 裸函数调用语句: add(a, b); */
            const char *fname = next(p)->text;
            Node *n = mk_node(N_CALLSTMT);
            n->l = parse_call_plain(p, fname);
            expect_op(p, ";");
            return n;
        }
    }
    fprintf(stderr, "语法错误: 无法解析语句 %s\n", t->text);
    p->err = 1;
    return mk_node(N_NUM);
}

Node **parse_stmts_until(Parser *p, const char *end, int *n) {
    Node **stmts = aalloc(sizeof(Node *) * 512);
    int n2 = 0;
    while (!(peek(p)->kind == T_OP && strcmp(peek(p)->text, end) == 0)) {
        stmts[n2++] = parse_stmt(p);
        if (p->err) break;
    }
    if (!p->err) next(p);   /* 消费结束符 */
    *n = n2;
    return stmts;
}

static int is_type_name(const char *s) {
    return strcmp(s, "int") == 0 || strcmp(s, "float") == 0 || strcmp(s, "double") == 0 ||
           strcmp(s, "string") == 0 || strcmp(s, "char") == 0;
}

/* 智能引用权限：r 读 / w 写 / rw 读写 / m 可移动 */
static int valid_perm(const char *s) {
    return strcmp(s, "r") == 0 || strcmp(s, "w") == 0 || strcmp(s, "rw") == 0 || strcmp(s, "m") == 0;
}

/* 智能引用跟随层：u 程序级(Unistream) / f 方法级(Functionstream) / a 作用域级(Areastream) */
static int valid_follow(const char *s) {
    return strcmp(s, "u") == 0 || strcmp(s, "f") == 0 || strcmp(s, "a") == 0;
}

/* 赋值运算符：= / 复合赋值 += -= *= /= %= */
static int is_assign_op(const char *s) {
    return strcmp(s, "=") == 0 || strcmp(s, "+=") == 0 || strcmp(s, "-=") == 0 ||
           strcmp(s, "*=") == 0 || strcmp(s, "/=") == 0 || strcmp(s, "%=") == 0;
}

/* 参数语法（唯一）：参数名 参数类型  如 void add(a int, b int)；数组 a int[]；
 * 类型可以是基本类型，也可以是任意类型名/流名（如 void show(cio CIO)）。
 * 也支持智能引用修饰：void show(&r f io IO) —— &权限 跟随 参数名 类型。 */
void parse_params(Parser *p, const char ***params, int *n) {
    const char **ps = aalloc(sizeof(char *) * 64);
    int n2 = 0;
    while (!is_op(p, ")")) {
        /* &权限 跟随 参数名 类型 */
        if (is_op(p, "&")) {
            next(p);
            next(p);                      /* 权限 r/w/rw/m */
            next(p);                      /* 跟随 u/f/a */
        }
        Tok *t = next(p);
        if (t->kind == T_ID) {
            if (is_type_name(t->text)) {
                fprintf(stderr, "语法错误: 参数写法应为「名称 类型」（如 a int），而不是 %s\n", t->text);
                p->err = 1; break;
            }
            ps[n2++] = t->text;
            if (is_op(p, "[")) { next(p); expect_op(p, "]"); }
            /* 类型：基本类型 或 任意标识符（流名/类名） */
            if (peek(p)->kind == T_ID) {
                if (is_type_name(peek(p)->text)) next(p);
                else { next(p); if (is_op(p, "[")) { next(p); expect_op(p, "]"); } }
            }
        }
        if (is_op(p, ",")) next(p);
    }
    *params = ps; *n = n2;
}

/* 返回类型：void 或类型名（可带 [] 数组后缀，如 int[]）。非方法开头返回 NULL */
static const char *parse_ret_type(Parser *p) {
    Tok *t = peek(p);
    if (t->kind == T_KW && strcmp(t->text, "void") == 0) {
        next(p);
        return "void";
    }
    if (t->kind == T_ID && is_type_name(t->text)) {
        next(p);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s", t->text);
        if (is_op(p, "[")) { next(p); expect_op(p, "]"); strcat(buf, "[]"); }
        return astrdup(buf);
    }
    return NULL;
}

void parse_methods(Parser *p, Method **methods, int *n) {
    Method *ms = aalloc(sizeof(Method) * 64);
    int n2 = 0;
    while (!is_op(p, "}")) {
        const char *rt = parse_ret_type(p);
        if (!rt) {
            fprintf(stderr, "语法错误: 期望返回类型（void/int/float/double/string/char，可带 []），得到 %s\n", peek(p)->text);
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
        if (p->err) break;
    }
    *methods = ms; *n = n2;
}

/* ═══════════════ 成员解析（类/签名流/分叉流通用）═══════════════
 * 方法与字段可任意顺序交错（顺序不严格），字段支持逗号分隔：
 *   void m(params) { ... }     方法（实现）
 *   void m(params);            方法（签名）
 *   int add(a int) { ... }     带返回类型的方法
 *   int x, y;  int[] a;  string s;   字段（类型 名字[, 名字...];）
 *   type T;                    泛型类型声明（原型阶段只登记名字）
 *   T n;  T[] a;  T m() {...}  泛型风格字段/方法
 */
static void parse_members(Parser *p, Method **methods, int *nmethods, Field **fields, int *nfields) {
    Method *ms = aalloc(sizeof(Method) * 128);
    Field *fs = aalloc(sizeof(Field) * 128);
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
                m->stmts = NULL; m->nstmts = 0;   /* 签名 */
            }
        } else if (t->kind == T_ID && is_type_name(t->text)) {
            /* 基本类型：返回类型的方法 或 字段（int x, y; / int[] a;） */
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
            expect_id(p);                  /* 泛型名（原型阶段不实例化） */
            expect_op(p, ";");
        } else if (t->kind == T_ID) {
            /* 泛型风格：T n; / T[] a; / T m() {...} */
            const char *ty = next(p)->text;
            int is_arr = 0;
            if (is_op(p, "[")) { next(p); expect_op(p, "]"); is_arr = 1; }
            const char *name = expect_id(p);
            char buf[64];
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
            } else {
                fs[nf].name = name; fs[nf].type = astrdup(buf); nf++;
                while (is_op(p, ",")) {
                    next(p);
                    fs[nf].name = expect_id(p); fs[nf].type = astrdup(buf); nf++;
                }
                expect_op(p, ";");
            }
        } else {
            fprintf(stderr, "语法错误: 无法解析流/类成员 %s\n", t->text);
            p->err = 1; break;
        }
    }
    if (!p->err) next(p);   /* 消费 } */
    *methods = ms; *nmethods = nm;
    *fields = fs; *nfields = nf;
}

/* 跳过 { ... } 块（need 假设的细节，原型阶段只登记名字） */
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
            continue;   /* 原型里只做声明，不区分 main/utils */
        }
        if (t->kind == T_KW && strcmp(t->text, "const") == 0) {
            /* 顶层 const 声明（原稿：const → Constantstream 公共常量） */
            next(p);
            if (peek(p)->kind != T_ID || !is_type_name(peek(p)->text)) {
                fprintf(stderr, "语法错误: const 后需要类型（如 const int x = 10;）\n"); p->err = 1;
            } else next(p);   /* 类型 */
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
                if (is_op(p, "{")) {           /* 原稿: need Class ... {...}; */
                    skip_block(p);
                    if (is_op(p, ";")) next(p);
                } else expect_op(p, ";");
            } else {
                fprintf(stderr, "语法错误: need 只支持 value/function/stream/Class\n"); p->err = 1;
            }
        }
        else if (t->kind == T_KW && strcmp(t->text, "Stream") == 0) {
            next(p);
            d->name = expect_id(p);
            if (is_op(p, "&")) {                 /* Stream <名> & <二进制库文件> {} */
                next(p);
                d->kind = D_BIN;
                Tok *ft = next(p);
                if (ft->kind != T_STR && ft->kind != T_ID) {
                    fprintf(stderr, "语法错误: 期望二进制库文件名\n"); p->err = 1;
                }
                d->file = ft->text;
                expect_op(p, "{");
                parse_stmts_until(p, "}", &(int){0});   /* 已消费 } */
                goto decl_done;
            }
            d->kind = D_SIG;
            expect_op(p, "{");
            parse_members(p, &d->methods, &d->nmethods, &d->fields, &d->nfields);
        }
        else if (t->kind == T_KW && strcmp(t->text, "Class") == 0) {
            next(p);
            d->kind = D_CLASS;
            d->name = expect_id(p);
            expect_op(p, "{");
            /* 方法与字段统一解析（Objstream 用方法，对象属性用字段） */
            parse_members(p, &d->methods, &d->nmethods, &d->fields, &d->nfields);
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
            /* 分叉: 签名流 实现流 { void m() {...} }  （可自定义字段） */
            d->kind = D_FORK;
            d->sig = expect_id(p);
            d->name = expect_id(p);
            expect_op(p, "{");
            parse_members(p, &d->methods, &d->nmethods, &d->fields, &d->nfields);
        }
        else {
            fprintf(stderr, "语法错误: 无法解析顶层声明 %s\n", t->text); p->err = 1; break;
        }
    decl_done:
        if (tail) { tail->next = d; tail = d; } else { head = tail = d; }
    }
    /* 把 Main 接在 decls 后面（分开存，便于运行） */
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
