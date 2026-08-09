/* bio.h — BioLang interpreter public header */
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
typedef struct Value Value;

/* ═══════════════ Central constants (no magic numbers in code) ═══════════════ */
#define BIO_TOKEN_MAX   4096   /* lexer token buffer */
#define BIO_STR_MAX     512    /* string literal / text buffers */
#define BIO_NAME_MAX    64     /* identifiers, type names, target names */
#define BIO_MSG_MAX     256    /* error / refusal message buffers */
#define BIO_ARGS_MAX    64     /* call arguments, multi-return values */
#define BIO_STMTS_MAX   512    /* statement block / work queue */
#define BIO_METHODS_MAX 128    /* methods per stream/class */
#define BIO_FIELDS_MAX  128    /* fields per stream/class */
#define BIO_VAR_MAX     256    /* variables per VarMap */
#define BIO_PATH_MAX    1024   /* filesystem paths */
#define BIO_STACK_SIZE  65536  /* cooperative thread stack */
#define BIO_COPY_BUF    65536  /* file copy buffer */
#define BIO_READ_BUF    4096   /* file read buffer */
#define BIO_SIO_BUF     8192   /* SIO in-memory string buffer */
#define BIO_ARR_CAP     8      /* default array capacity */
#define BIO_NUM_BUF     32     /* number-to-string buffer */
#define BIO_MEM_MAX     256    /* Rem memory entries */
#define BIO_TOML_KEYS   256    /* TOML key/value pairs */
#define BIO_QUEUE_MAX   512    /* project file work queue */
#define BIO_RUNTIME_VERSION "1" /* bump to invalidate cached runtime objects */

/* Smart-reference target: a reference points at an lvalue, and with the
 * `m` (move) permission `p++` advances the pointer (array element refs). */
typedef struct RefTarget {
    int kind;               /* 0 = variable, 1 = array element, 2 = object field */
    const char *name;       /* variable / field name */
    VarMap *map;            /* kind 0: the map that owns the variable */
    Value *arr;             /* kind 1: the array (or Array object) value */
    int index;              /* kind 1: current element index (moves with ++/--) */
    Value *obj;             /* kind 2: the object value */
} RefTarget;

typedef enum { V_NUM, V_STR, V_ARR, V_REF, V_OBJ, V_RES, V_STREAM } VKind;

typedef struct Value {
    VKind kind;
    double num;
    const char *str;
    struct Value **items;  /* kind == V_ARR */
    int len;               /* V_ARR */
    int cap;               /* V_ARR */
    int head;              /* V_ARR: Solid stream head pointer */
    Result *res;          /* kind == V_RES */
    const char *ref_perm;  /* V_REF: r / w / rw */
    const char *ref_follow;/* V_REF: u / m / a */
    const char *ref_type;  /* V_REF: declared base type (int/double/... generic) */
    RefTarget *ref_tgt;    /* V_REF: where the reference points */
    const char *obj_cls;   /* V_OBJ: class name */
    struct VarMap *obj_fields;  /* V_OBJ: object fields (attributes) */
    struct Stream *stream_ref;  /* V_STREAM: stream reference (CIO/FIO/... passed as an argument) */
} Value;

struct Result {
    Value *res;
    const char *ref;
};

struct VarMap {
    const char *names[256];
    Value *vals[256];
    int n;
    struct VarMap *parent;   /* scope chain: method → thread scope (area) → ... */
    int is_area;             /* 1 = thread/main-thread scope layer (the layer 'a' follows) */
};

typedef struct { Result *ret; int brk; int cont; } Flow;

typedef enum {
    N_NUM, N_STR, N_VAR, N_CALL, N_PROP, N_BINOP, N_INDEX,
    N_ASSIGN, N_CALLSTMT, N_RET, N_REF, N_REALME, N_UNWRAP, N_INC,
    N_IF, N_WHILE, N_FOR, N_BREAK, N_CONTINUE
} NodeKind;

struct Node {
    NodeKind kind;
    double num;
    const char *str;          /* N_STR */
    const char *name;         /* N_VAR / N_ASSIGN var name / N_PROP property name */
    const char *op;           /* N_BINOP operator / N_ASSIGN assignment operator / N_UNWRAP extractor ("res"/"cause") */
    Node *l, *r;              /* N_BINOP / N_PROP(base) / N_INDEX(base,idx) / N_UNWRAP(target) */
    const char *qual;         /* N_CALL: stream name */
    const char *mname;        /* N_CALL: method name */
    Node **args; int nargs;   /* N_CALL */
    Node *expr;               /* N_ASSIGN / N_RET (single value) */
    Node *target;             /* N_ASSIGN: property lvalue (non-NULL for this::base = v) */
    int is_const;             /* N_ASSIGN: const modifier (→ Constantstream) */
    int is_thread;            /* N_ASSIGN: thread modifier (→ thread variable) */
    Node **rets; int nrets;   /* N_RET: res multi-values (res a, b, c → array) */
    const char *retkind;      /* N_RET: "res" / "ref" */
    const char *ref_perm;     /* N_REF: r / w / rw */
    const char *ref_follow;   /* N_REF: u / f / a */
    const char *ref_type;     /* N_REALME: declared reference base type */
    const char *inc_op;       /* N_INC: "++" / "--" */
    Node *init;               /* N_FOR: init statement */
    Node *cond;               /* N_IF / N_WHILE / N_FOR: condition */
    Node *update;             /* N_FOR: update statement */
    Node **stmts; int nstmts; /* statement block */
    int has_else;             /* N_IF */
    Node **else_stmts; int n_else;
};

typedef struct Method {
    const char *name;
    const char *ret_type;      /* void / int / float / double / string / char / int[] etc. */
    const char **params; int nparams;
    Node **stmts; int nstmts;
    int builtin;               /* non-zero = builtin implementation (e.g. B_ARS), no stmts */
    int write;                 /* @write annotation: method writes */
    int read;                  /* @read annotation: method is read-only */
} Method;

/* Field declaration (class/stream attributes; the original spec: every object/stream stores its own attributes) */
typedef struct Field {
    const char *name;
    const char *type;          /* int / string / int[] / T (generic) etc. */
} Field;

typedef enum { D_NEED, D_SIG, D_FORK, D_MAIN, D_BIN, D_CLASS, D_CONST } DeclKind;

typedef struct Decl {
    DeclKind kind;
    const char *name;          /* need name / signature stream name / implementation name */
    const char *needkind;      /* D_NEED: value/function/stream/Class */
    const char *sig;           /* D_FORK: signature stream name */
    const char *file;          /* D_BIN: binary library file path */
    Method *methods; int nmethods;
    Field *fields; int nfields;/* declared fields (class/stream), materialized when object/stream is created */
    Node *init;               /* D_CONST: top-level const initializer expression */
    int onlyread;             /* @onlyread annotation */
    int unfork;               /* @unfork annotation */
    struct Decl *next;
} Decl;

typedef struct MethodEntry { Method *m; struct MethodEntry *next; } MethodEntry;

/* Builtin stream kinds */
typedef enum { B_NONE = 0, B_CIO, B_FIO, B_SIO, B_SOLID, B_ARRAYS, B_BIN, B_BTS, B_TASK, B_REF, B_TIME, B_REM, B_CONST, B_OBJ, B_COM, B_IO } BuiltinKind;

struct Stream {
    const char *name;
    MethodEntry *methods;      /* user stream methods */
    VarMap *fields;            /* stream fields/attributes (storage for this:: attributes) */
    int builtin;               /* B_NONE = user stream, otherwise builtin */
    void *dl;                  /* B_BIN: dlopen handle */
    void *fio_fp;              /* B_FIO: per-stream open file handle */
    int fio_writable;          /* B_FIO: current handle opened for writing */
    BuiltinKind sig_builtin;   /* user fork of a builtin signature (e.g. FIO r {}) */
    int onlyread;              /* @onlyread: users cannot call write methods */
    int unfork;                /* @unfork: the stream cannot be forked */
    Method *method_cache;      /* hot-path: last method found by name */
    const char *method_cache_name;
    struct Stream *next;
};

typedef enum { T_NUM, T_STR, T_KW, T_ID, T_OP, T_EOF } TokKind;

typedef struct Tok { TokKind kind; const char *text; double num; } Tok;

typedef struct {
    Tok *t; int i; int n;
    int err;
} Parser;

typedef struct {
    Decl *decls;         /* all declarations (incl. Main) */
    Stream *streams;     /* stream registry */
    VarMap globals;      /* Unistream program level (followed by u) */
    VarMap consts;       /* Constantstream public constants layer (const declarations, read-only) */
    VarMap main_area;    /* main-thread scope (followed by a) */
    VarMap *cur_area;    /* current area (updated on thread switch) */
    VarMap *cur_scope;   /* current method scope (for reference resolution) */
    Stream *cur_stream;  /* stream currently executing a method (bare calls / internal attrs take priority) */
    Value *arrays;       /* Arrays registry: all Array/Vector instances */
    Stream *stream_cache;        /* hot-path: last stream found by name */
    const char *stream_cache_name;
} Interp;

void *aalloc(size_t n);
char *astrdup(const char *s);
void bio_set_mem_limit(size_t bytes);
size_t bio_mem_used(void);
/* Generic "nothing": default ref(NOTHING) when no explicit return; if treats it as false */
#define NOTHING "nothing"

Result *builtin_request(Stream *s, int kind, const char *method, Value **args, int nargs);
Value *mk_arr(int cap);
Value *mk_refobj(const char *perm, const char *follow, RefTarget *tgt);
RefTarget *ref_target_var(VarMap *map, const char *name);
RefTarget *ref_target_elem(Value *arr, int index);
RefTarget *ref_target_field(Value *obj, const char *name);
Value *ref_read(RefTarget *t, const char **err);
int ref_write(RefTarget *t, Value *v, const char **err);
int ref_move(RefTarget *t, int delta, const char **err);
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
VarMap *ref_layer_get(Interp *in, const char *follow);   /* smart-ref follow layer: u/f/a */
VarMap *var_find_map(VarMap *scope, const char *name);   /* find the map that owns a variable */
Value *mk_obj(const char *cls);
Decl *find_class(Interp *in, const char *name);
Method *class_method(Decl *cls, const char *mname);
Result *interp_exec_method(Interp *in, Method *m, Value **args, int nargs, VarMap *parent, Value *self);
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
int compile_program(const char *src, const char *outpath);   /* bio shell build: source-embedded compilation */
Tok *tokenize(const char *src, int *ntok);

/* ── Package formats (src/pack.c): raw .img and .zip ── */
int img_create(const char *out, const char *entry, const char **files, int nfiles);
int img_unpack(const char *path, const char *dir);
int img_entry(const char *path, char *out_path, size_t out_cap);
int zip_create(const char *out, const char **files, int nfiles);
int zip_unpack(const char *path, const char *dir);

/* ── Project-based build & run (src/project.c) ── */
/* TOML key-value table (minimal, src/toml.c) */
typedef struct TomlPair { const char *key; const char *val; } TomlPair;
typedef struct TomlTable { TomlPair *pairs; int n; } TomlTable;
TomlTable *toml_parse_file(const char *path);         /* top-level + [dependencies] */
const char *toml_get(TomlTable *t, const char *key);  /* look up a top-level key */
const char *toml_dep(TomlTable *t, int idx, const char **name); /* iterate dependencies */
const char *toml_dep_field(TomlTable *t, const char *dep, const char *field); /* a dep's version/repo */
/* Global config: env var BIOLANG_CONFIG points to a global config file (TOML, may contain repo / system path) */
const char *global_repo(void);                        /* resolve the default repo from the global config */

/* Project commands */
int project_init(const char *name);                   /* bio init */
int project_build(const char *dir, const char *out);  /* bio build */
int project_run(const char *dir);                     /* bio run */
int project_install(const char *dir);                 /* bio install */
int project_destroy(const char *dir);                 /* bio destroy */
int truthy(Value *v);
Value *var_get(VarMap *m, const char *name);
Value *var_get_layer(VarMap *m, const char *name);   /* single-layer lookup (for references) */
void var_del(VarMap *m, const char *name);               /* delete (attribute washed away) */
void var_set(VarMap *m, const char *name, Value *v);

#endif /* BIO_H */
