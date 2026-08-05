/* bio.h — BioLang 解释器公共头 */
#ifndef BIO_H
#define BIO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct Result Result;

typedef struct Stream Stream;

typedef struct Node Node;

typedef struct VarMap VarMap;

typedef enum { V_NUM, V_STR, V_ARR, V_REF, V_OBJ, V_RES, V_STREAM } VKind;

typedef struct Value {
    VKind kind;
    double num;
    const char *str;
    struct Value **items;  /* kind == V_ARR */
    int len;               /* V_ARR */
    int cap;               /* V_ARR */
    int head;              /* V_ARR: Solid 连续流头指针 */
    Result *res;          /* kind == V_RES */
    const char *ref_perm;  /* V_REF: r / w / rw */
    const char *ref_follow;/* V_REF: u / m / a */
    const char *ref_name;  /* V_REF: 目标真名 */
    const char *obj_cls;   /* V_OBJ: 类名 */
    struct VarMap *obj_fields;  /* V_OBJ: 对象字段（属性） */
    struct Stream *stream_ref;  /* V_STREAM: 流引用（CIO/FIO/... 作为参数传递） */
} Value;

struct Result {
    Value *res;
    const char *ref;
};

struct VarMap {
    const char *names[256];
    Value *vals[256];
    int n;
    struct VarMap *parent;   /* 作用域链：方法 → 线程作用域(area) → ... */
    int is_area;             /* 1 = 线程/主线程作用域层（跟随 a 的解析层） */
};

typedef struct { Result *ret; int brk; int cont; } Flow;

typedef enum {
    N_NUM, N_STR, N_VAR, N_CALL, N_PROP, N_BINOP,
    N_ASSIGN, N_CALLSTMT, N_RET, N_BINCALL, N_REF, N_REALME,
    N_IF, N_WHILE, N_FOR, N_BREAK, N_CONTINUE
} NodeKind;

struct Node {
    NodeKind kind;
    double num;
    const char *str;          /* N_STR */
    const char *name;         /* N_VAR / N_ASSIGN 变量名 / N_PROP 属性名 */
    const char *op;           /* N_BINOP 运算符 / N_ASSIGN 赋值运算符（"=" "+=" "-=" 等） */
    Node *l, *r;              /* N_BINOP / N_PROP(base) */
    const char *qual;         /* N_CALL: 流名 */
    const char *mname;        /* N_CALL: 方法名 */
    Node **args; int nargs;   /* N_CALL */
    Node *expr;               /* N_ASSIGN / N_RET(单值) */
    Node *target;             /* N_ASSIGN: 属性左值（this::base = v 时非 NULL） */
    int is_const;             /* N_ASSIGN: const 修饰（→ Constantstream） */
    int is_thread;            /* N_ASSIGN: thread 修饰（→ 线程变量） */
    Node **rets; int nrets;   /* N_RET: res 多值（res a, b, c → 数组） */
    const char *retkind;      /* N_RET: "res" / "ref" */
    const char *ref_perm;     /* N_REF: r / w / rw */
    const char *ref_follow;   /* N_REF: u / m / a */
    const char *ref_name;     /* N_REF: 目标真名 */
    Node *init;               /* N_FOR: 初始化语句 */
    Node *cond;               /* N_IF / N_WHILE / N_FOR: 条件 */
    Node *update;             /* N_FOR: 更新语句 */
    Node **stmts; int nstmts; /* 语句块 */
    int has_else;             /* N_IF */
    Node **else_stmts; int n_else;
};

typedef struct Method {
    const char *name;
    const char *ret_type;      /* void / int / float / double / string / char / int[] 等 */
    const char **params; int nparams;
    Node **stmts; int nstmts;
    int builtin;               /* 非 0 = 内置实现（如 B_ARS），无 stmts */
} Method;

/* 字段声明（类/流的属性，原稿：每个对象/流储存各种属性） */
typedef struct Field {
    const char *name;
    const char *type;          /* int / string / int[] / T（泛型）等 */
} Field;

typedef enum { D_NEED, D_SIG, D_FORK, D_MAIN, D_BIN, D_CLASS, D_CONST } DeclKind;

typedef struct Decl {
    DeclKind kind;
    const char *name;          /* need 名 / 签名流名 / 实现名 */
    const char *needkind;      /* D_NEED: value/function/stream/Class */
    const char *sig;           /* D_FORK: 签名流名 */
    const char *file;          /* D_BIN: 二进制库文件路径 */
    Method *methods; int nmethods;
    Field *fields; int nfields;/* 声明字段（类/流），对象/流创建时物化 */
    Node *init;               /* D_CONST: 顶层 const 初值表达式 */
    struct Decl *next;
} Decl;

typedef struct MethodEntry { Method *m; struct MethodEntry *next; } MethodEntry;

/* 内置流类型 */
typedef enum { B_NONE = 0, B_CIO, B_FIO, B_SIO, B_SOLID, B_ARRAYS, B_BIN, B_BTS, B_TASK, B_REF, B_TIME, B_REM, B_CONST, B_OBJ, B_COM, B_IO } BuiltinKind;

struct Stream {
    const char *name;
    MethodEntry *methods;      /* 用户流的方法 */
    VarMap *fields;            /* 流字段/属性（this:: 的属性存储） */
    int builtin;               /* B_NONE=用户流，其余=内置流 */
    void *dl;                  /* B_BIN: dlopen 句柄 */
    struct Stream *next;
};

typedef enum { T_NUM, T_STR, T_KW, T_ID, T_OP, T_EOF } TokKind;

typedef struct Tok { TokKind kind; const char *text; double num; } Tok;

typedef struct {
    Tok *t; int i; int n;
    int err;
} Parser;

typedef struct {
    Decl *decls;         /* 全部声明（含 Main） */
    Stream *streams;     /* 流注册表 */
    VarMap globals;      /* Unistream 程序级（跟随 u） */
    VarMap consts;       /* Constantstream 公共常量层（const 修饰声明，只读） */
    VarMap main_area;    /* 主线程作用域（跟随 a） */
    VarMap *cur_area;    /* 当前作用域（线程切换时更新） */
    VarMap *cur_scope;   /* 当前方法作用域（引用解析用） */
    Stream *cur_stream;  /* 当前执行方法的流（裸调用/内部属性优先） */
    Value *arrays;       /* Arrays 注册表：所有 Array/Vector 实例 */
} Interp;

void *aalloc(size_t n);
char *astrdup(const char *s);
/* 通用"无"：无显式返回时默认 ref(NOTHING)，if 视为假 */
#define NOTHING "无"

Result *builtin_request(int kind, const char *method, Value **args, int nargs);
Value *mk_arr(int cap);
void binlib_register(Stream *s);
Result *bin_call_global(const char *method, Value **args, int nargs);
Result *bin_request(Stream *s, const char *method, Value **args, int nargs);
Result *bts_request(const char *method, Value **args, int nargs);
Result *taskm_request(const char *method, Value **args, int nargs);
Result *ref_request(const char *method, Value **args, int nargs);
Result *time_request(const char *method, Value **args, int nargs);
Result *rem_request(const char *method, Value **args, int nargs);
Result *const_request(const char *method, Value **args, int nargs);
Result *obj_request(const char *method, Value **args, int nargs);
Result *arrays_request(const char *method, Value **args, int nargs);
Result *solid_request(const char *method, Value **args, int nargs);
VarMap *ref_layer_get(Interp *in, const char *follow);   /* 智能引用跟随层：u/f/a */
Value *mk_obj(const char *cls);
Decl *find_class(Interp *in, const char *name);
Method *class_method(Decl *cls, const char *mname);
Result *interp_exec_method(Interp *in, Method *m, Value **args, int nargs, VarMap *parent, Value *self);
Value *mk_refobj(const char *perm, const char *follow, const char *name);
Result *interp_call_global(Interp *in, const char *mname, Value **args, int nargs);
extern Interp *g_interp;
int is_rejected(Value *v);
Value *field_default(const char *type);
Value *mk_streamref(Stream *s);
Value *mk_num(double d);
Result *mk_ref(const char *reason);
Value *mk_refval(const char *reason);
Result *mk_res(Value *v);
Value *mk_str(const char *s);
Decl *parse_program_tokens(Tok *toks, int n, int *err);
void print_value(Value *v);
const char *reject_reason(Value *v);
void run_source(const char *src);
Tok *tokenize(const char *src, int *ntok);
int truthy(Value *v);
Value *var_get(VarMap *m, const char *name);
Value *var_get_layer(VarMap *m, const char *name);   /* 单层查找（引用用） */
void var_del(VarMap *m, const char *name);               /* 删除（属性冲走） */
void var_set(VarMap *m, const char *name, Value *v);

#endif /* BIO_H */
