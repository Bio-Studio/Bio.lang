#include "bio.h"
#include "platform.h"

/* ═══════════════ Builtin streams ═══════════════
 * CIO —— Console
 * FIO —— File
 * SIO —— String
 * IO  —— parent stream, aggregating all of the above (old style IO::println still works)
 */


static Value *arg(Value **args, int n, int i) { return i < n ? args[i] : NULL; }
static const char *arg_str(Value **args, int n, int i, const char *dflt) {
    Value *v = arg(args, n, i);
    if (!v) return dflt;
    if (v->kind == V_STR) return v->str;
    if (v->kind == V_NUM) { static char b[32]; snprintf(b, sizeof b, "%g", v->num); return b; }
    return dflt;
}

/* ---------- CIO: console ---------- */
static Result *cio_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "println") == 0) {
        for (int i = 0; i < nargs; i++) {
            if (i) printf(" ");
            /* Successful Result auto-unwraps for display (res(x) → x; ref stays displayed as a ref) */
            if (args[i]->kind == V_RES && args[i]->res && !args[i]->res->ref)
                print_value(args[i]->res->res);
            else print_value(args[i]);
        }
        printf("\n");
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "print") == 0) {
        for (int i = 0; i < nargs; i++) {
            /* Successful Result auto-unwraps for display (res(x) → x), consistent with println */
            if (args[i]->kind == V_RES && args[i]->res && !args[i]->res->ref)
                print_value(args[i]->res->res);
            else print_value(args[i]);
        }
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "write") == 0) {
        /* Byte stream: write raw bytes verbatim (no newline, no formatting) */
        for (int i = 0; i < nargs; i++) fwrite(arg_str(args, nargs, i, ""), 1, strlen(arg_str(args, nargs, i, "")), stdout);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "read") == 0) {
        /* Byte stream: read one raw byte (0-255); EOF returns -1 */
        int c = getchar();
        return mk_res(mk_num(c == EOF ? -1 : (double)c));
    }
    if (strcmp(method, "error") == 0) {
        for (int i = 0; i < nargs; i++) fprintf(stderr, "%s", arg_str(args, nargs, i, ""));
        return mk_res(mk_str(""));
    }
    /* Text stream: getln reads a line of text; get reads one character; readInt/readNumber read numeric values */
    if (strcmp(method, "getln") == 0 || strcmp(method, "readln") == 0 || strcmp(method, "readInt") == 0 ||
        strcmp(method, "readNumber") == 0) {
        char buf[512];
        if (nargs > 0) {
            /* Prompt argument: successful Result auto-unwraps for display (res(x) → x) */
            if (args[0]->kind == V_RES && args[0]->res && !args[0]->res->ref)
                print_value(args[0]->res->res);
            else print_value(args[0]);
        }
        if (!fgets(buf, sizeof buf, stdin)) buf[0] = 0;
        size_t len = strlen(buf);
        while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
        char *end;
        if (strcmp(method, "readInt") == 0) {
            long v = strtol(buf, &end, 10);
            if (end == buf || *end) return mk_ref("CIO refused: readInt input is not an integer");
            return mk_res(mk_num((double)v));
        }
        if (strcmp(method, "readNumber") == 0) {
            double v = strtod(buf, &end);
            if (end == buf || *end) return mk_ref("CIO refused: readNumber input is not a number");
            return mk_res(mk_num(v));
        }
        return mk_res(mk_str(astrdup(buf)));
    }
    if (strcmp(method, "get") == 0) {
        /* Text stream: read one character (a single UTF-8 byte; EOF returns the empty string) */
        int c = getchar();
        if (c == EOF) return mk_res(mk_str(""));
        char b[2] = { (char)c, 0 };
        return mk_res(mk_str(astrdup(b)));
    }
    return mk_ref("CIO refused: no such method (bytes: read/write; text: get/getln/println/print; readInt/readNumber/error)");
}

/* ---------- FIO: file (file implementation of IOStream)----------
 * Besides the readFile/writeFile/appendFile/exists convenience methods, implements the IO core methods:
 * first open the "current file" stream with FIO::open(path, mode), then
 *   text stream: print/println write / get/getln read; byte stream: write writes / read reads
 */
static FILE *fio_cur = NULL;
static int fio_cur_writable = 0;   /* whether the current file allows writing */

static Result *fio_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "open") == 0) {
        const char *path = arg_str(args, nargs, 0, "");
        const char *mode = arg_str(args, nargs, 1, "r");
        if (fio_cur) { fclose(fio_cur); fio_cur = NULL; }
        fio_cur = fopen(path, mode);
        if (!fio_cur) {
            char buf[256];
            snprintf(buf, sizeof buf, "FIO refused: cannot open file %s (mode %s)", path, mode);
            return mk_ref(astrdup(buf));
        }
        fio_cur_writable = mode[0] == 'w' || mode[0] == 'a';
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "close") == 0) {
        if (fio_cur) { fclose(fio_cur); fio_cur = NULL; }
        return mk_res(mk_str(""));
    }
    /* ---- IO core methods (file implementation): write the current file ---- */
    if (strcmp(method, "write") == 0 || strcmp(method, "print") == 0 ||
        strcmp(method, "println") == 0) {
        if (!fio_cur || !fio_cur_writable)
            return mk_ref("FIO refused: no writable file open (use FIO::open(path, \"w\") first)");
        for (int i = 0; i < nargs; i++) {
            if (i) fputc(' ', fio_cur);
            Value *v = args[i];
            if (v->kind == V_RES && v->res && !v->res->ref) v = v->res->res;   /* auto-unwrap */
            if (v->kind == V_STR) fputs(v->str, fio_cur);
            else if (v->kind == V_NUM) { char b[32]; snprintf(b, sizeof b, "%g", v->num); fputs(b, fio_cur); }
        }
        if (strcmp(method, "println") == 0) fputc('\n', fio_cur);
        fflush(fio_cur);
        return mk_res(mk_str(""));
    }
    /* ---- IO core methods (file implementation): read the current file ---- */
    if (strcmp(method, "read") == 0) {
        /* Byte stream: read one raw byte (0-255); EOF returns -1 */
        if (!fio_cur) return mk_ref("FIO refused: no file open (use FIO::open(path) first)");
        int c = fgetc(fio_cur);
        return mk_res(mk_num(c == EOF ? -1 : (double)c));
    }
    if (strcmp(method, "get") == 0) {
        /* Text stream: read one character (a single byte); EOF returns the empty string */
        if (!fio_cur) return mk_ref("FIO refused: no file open (use FIO::open(path) first)");
        int c = fgetc(fio_cur);
        if (c == EOF) return mk_res(mk_str(""));
        char b[2] = { (char)c, 0 };
        return mk_res(mk_str(astrdup(b)));
    }
    if (strcmp(method, "getln") == 0 || strcmp(method, "readln") == 0) {
        /* Text stream: read one line */
        if (!fio_cur) return mk_ref("FIO refused: no file open (use FIO::open(path) first)");
        char buf[1024];
        if (!fgets(buf, sizeof buf, fio_cur)) return mk_res(mk_str(""));
        size_t len = strlen(buf);
        while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
        return mk_res(mk_str(astrdup(buf)));
    }
    if (strcmp(method, "readFile") == 0) {
        const char *path = arg_str(args, nargs, 0, "");
        FILE *f = fopen(path, "r");
        if (!f) return mk_ref("FIO refused: cannot open file (missing or no permission)");
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
        if (!f) return mk_ref("FIO refused: cannot write file");
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
    return mk_ref("FIO refused: no such method");
}

/* ---------- SIO: string ---------- */
static char *s_dup_range(const char *s, int start, int end) {
    int len = (int)strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end || start >= len) return astrdup("");
    char *r = aalloc(end - start + 1);
    memcpy(r, s + start, end - start); r[end - start] = 0;
    return r;
}

/* ---------- SIO: string (string implementation of IOStream)----------
 * Besides string tool methods, implements the IO core methods (println/print/write/read/readln):
 * writes/reads a single in-memory string buffer (SIO's "file" is this string).
 */
#define SIO_BUF_SIZE 8192
static char sio_buf[SIO_BUF_SIZE];
static int sio_len = 0;
static int sio_head = 0;

/* Convert a value to text and append it to the SIO buffer (number/string/Result unwrapped) */
static void sio_append_arg(Value *v) {
    if (v->kind == V_RES && v->res && !v->res->ref) v = v->res->res;
    if (v->kind == V_STR) {
        int l = (int)strlen(v->str);
        if (sio_len + l < SIO_BUF_SIZE) { memcpy(sio_buf + sio_len, v->str, l); sio_len += l; sio_buf[sio_len] = 0; }
    } else if (v->kind == V_NUM) {
        char b[32]; snprintf(b, sizeof b, "%g", v->num);
        int l = (int)strlen(b);
        if (sio_len + l < SIO_BUF_SIZE) { memcpy(sio_buf + sio_len, b, l); sio_len += l; sio_buf[sio_len] = 0; }
    }
}

static void sio_append_str(const char *s) {
    int l = (int)strlen(s);
    if (sio_len + l < SIO_BUF_SIZE) { memcpy(sio_buf + sio_len, s, l); sio_len += l; sio_buf[sio_len] = 0; }
}

static void sio_append_nl(void) {
    if (sio_len + 1 < SIO_BUF_SIZE) { sio_buf[sio_len++] = '\n'; sio_buf[sio_len] = 0; }
}

static Result *sio_request(const char *method, Value **args, int nargs) {
    /* ---- IO core methods (string implementation)----
     * byte stream: write writes raw bytes to the buffer / read reads one raw byte and advances
     * text stream: print/println write text / get/getln read text (getln reads a line) */
    if (strcmp(method, "write") == 0) {
        for (int i = 0; i < nargs; i++) sio_append_arg(args[i]);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "read") == 0) {
        /* Byte stream: read one raw byte (0-255); empty returns -1 */
        if (sio_head >= sio_len) return mk_res(mk_num(-1));
        return mk_res(mk_num((double)(unsigned char)sio_buf[sio_head++]));
    }
    if (strcmp(method, "print") == 0) {
        for (int i = 0; i < nargs; i++) { if (i) sio_append_str(" "); sio_append_arg(args[i]); }
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "println") == 0) {
        for (int i = 0; i < nargs; i++) { if (i) sio_append_str(" "); sio_append_arg(args[i]); }
        sio_append_nl();
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "getln") == 0 || strcmp(method, "readln") == 0) {
        /* Text stream: read one line (up to \n or the end), consuming it */
        if (sio_head >= sio_len) return mk_res(mk_str(""));
        int end = sio_head;
        while (end < sio_len && sio_buf[end] != '\n') end++;
        int n = end - sio_head;
        char *line = aalloc(n + 1);
        memcpy(line, sio_buf + sio_head, n); line[n] = 0;
        sio_head = (end < sio_len) ? end + 1 : end;   /* consume including the newline */
        if (n && line[n-1] == '\r') line[n-1] = 0;
        return mk_res(mk_str(line));
    }
    if (strcmp(method, "get") == 0) {
        /* Text stream: read one character (a single byte); empty returns the empty string */
        if (sio_head >= sio_len) return mk_res(mk_str(""));
        char b[2] = { sio_buf[sio_head++], 0 };
        return mk_res(mk_str(astrdup(b)));
    }
    if (strcmp(method, "content") == 0) {
        if (sio_head >= sio_len) return mk_res(mk_str(""));
        return mk_res(mk_str(s_dup_range(sio_buf, sio_head, sio_len)));
    }
    if (strcmp(method, "clear") == 0) { sio_len = 0; sio_head = 0; return mk_res(mk_str("")); }
    if (strcmp(method, "format") == 0) {   /* hand-written %d %i %s %f substitution */
        const char *fmt = arg_str(args, nargs, 0, "");
        char out[2048]; int oi = 0; int ai = 1;
        for (int i = 0; fmt[i] && oi < 2040; i++) {
            if (fmt[i] == '%' && fmt[i+1] && strchr("disfx", fmt[i+1]) && ai < nargs) {
                char c = fmt[++i];
                Value *v = args[ai++];
                /* Successful Result auto-unwraps (res(x) → x), consistent with println */
                if (v->kind == V_RES && v->res && !v->res->ref) v = v->res->res;
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
    return mk_ref("SIO refused: no such method (IO: println/print/write/read/readln/content/clear; tools: format/length/upper/lower/trim/contains/substring/replace)");
}

/* ---------- Array: array stream ---------- */
/* Unwrap an array argument: a bare V_ARR or a Result wrapper (an array object is a V_ARR) */
/* Unwrap an array object: bare V_ARR / Result wrapper / array-class object (data field = Solid stream) */
static Value *arr_unwrap(Value *v) {
    if (v->kind == V_ARR) return v;
    if (v->kind == V_RES && v->res && !v->res->ref) v = v->res->res;
    if (v && v->kind == V_OBJ && v->obj_fields) {
        Value *d = var_get_layer(v->obj_fields, "data");
        if (d && d->kind == V_ARR) return d;
    }
    return NULL;
}

/* Register an Array/Vector instance into Arrays (called by the __init__ of the Array/Vector class; visible to Bio code) */
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

/* ---------- Solid: Solid stream (contiguous storage + auto-allocation + advancing the head pointer) ---------- */
Result *solid_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "new") == 0) return mk_res(mk_arr(0));
    Value *s = arr_unwrap(nargs > 0 ? args[0] : NULL);
    if (!s) return mk_ref("Solid refused: first argument must be a stream (created by Solid::new)");
    if (strcmp(method, "len") == 0) return mk_res(mk_num((double)(s->len - s->head)));
    if (strcmp(method, "get") == 0) {
        int i = (nargs > 1 && args[1]->kind == V_NUM) ? (int)args[1]->num : -1;
        if (i < 0 || s->head + i >= s->len) return mk_ref("Solid refused: index out of bounds");
        return mk_res(s->items[s->head + i]);
    }
    if (strcmp(method, "set") == 0) {
        int i = (nargs > 1 && args[1]->kind == V_NUM) ? (int)args[1]->num : -1;
        if (i < 0 || s->head + i >= s->len) return mk_ref("Solid refused: index out of bounds");
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
        if (s->len <= s->head) return mk_ref("Solid refused: stream is empty");
        return mk_res(s->items[--s->len]);
    }
    if (strcmp(method, "read") == 0) {          /* advance the head pointer: read one and move on */
        if (s->head >= s->len) return mk_ref("Solid refused: stream is empty");
        return mk_res(s->items[s->head++]);
    }
    if (strcmp(method, "peek") == 0) {
        if (s->head >= s->len) return mk_ref("Solid refused: stream is empty");
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
    return mk_ref("Solid refused: no such method (new/len/get/set/push/pop/read/peek/head/resetHead/clear/join)");
}

/* ---------- Arrays: Array collection stream (contains Array/Vector instances) ---------- */
Result *arrays_request(const char *method, Value **args, int nargs) {
    Interp *in = g_interp;
    if (strcmp(method, "count") == 0) return mk_res(mk_num((double)in->arrays->len));
    if (strcmp(method, "all") == 0) return mk_res(in->arrays);
    if (strcmp(method, "get") == 0) {
        int i = (nargs > 0 && args[0]->kind == V_NUM) ? (int)args[0]->num : -1;
        if (i < 0 || i >= in->arrays->len) return mk_ref("Arrays refused: index out of bounds");
        return mk_res(in->arrays->items[i]);
    }
    if (strcmp(method, "add") == 0) {           /* register (called by Array/Vector __init__) */
        Value *o = nargs > 0 ? args[0] : NULL;
        if (o && o->kind == V_RES && o->res && !o->res->ref) o = o->res->res;
        if (!o || o->kind != V_OBJ || !var_get_layer(o->obj_fields, "data"))
            return mk_ref("Arrays refused: add requires an array object");
        arrays_register(o);
        return mk_res(o);
    }
    if (strcmp(method, "vector") == 0) {        /* dynamic array: new Vector() (a Bio class) */
        Value *cls = mk_str("Vector");
        return obj_request("new", &cls, 1);
    }
    if (strcmp(method, "forget") == 0) {  /* remove from the registry */
        Value *o = nargs > 0 ? args[0] : NULL;
        if (o && o->kind == V_RES && o->res && !o->res->ref) o = o->res->res;
        if (!o || o->kind != V_OBJ) return mk_ref("Arrays refused: requires an array object");
        for (int i = 0; i < in->arrays->len; i++) {
            if (in->arrays->items[i] == o) {
                in->arrays->items[i] = in->arrays->items[in->arrays->len - 1];
                in->arrays->len--;
                return mk_res(mk_num(1));
            }
        }
        return mk_ref("Arrays refused: array not in registry");
    }
    return mk_ref("Arrays refused: no such method (count/all/get/add/vector/forget)");
}

/* ---------- Obj: Objstream object stream ---------- */
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

/* The result of new is an object wrapped in a V_RES (request model); object APIs unwrap uniformly (arrays are objects too) */
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
        if (nargs < 1 || args[0]->kind != V_STR) return mk_ref("Obj refused: new requires a class name");
        Decl *cls = find_class(in, args[0]->str);
        if (!cls) return mk_ref("Obj refused: no such class (declare Class first)");
        Value *o = mk_obj(args[0]->str);
        o->obj_fields->parent = in->cur_area;
        /* An object is also a stream holding properties: materialize the class-declared fields onto the instance (with defaults) */
        for (int i = 0; i < cls->nfields; i++)
            var_set(o->obj_fields, cls->fields[i].name, field_default(cls->fields[i].type));
        Method *init = class_method(cls, "__init__");
        if (init) {
            Result *r = interp_exec_method(in, init, args + 1, nargs - 1, o->obj_fields, o);
            if (r->ref && strcmp(r->ref, NOTHING) != 0) {   /* ref(nothing) = implicit completion */
                char buf[256];
                snprintf(buf, sizeof buf, "Obj refused: __init__ refused (%s)", r->ref);
                return mk_ref(astrdup(buf));
            }
        }
        return mk_res(o);
    }
    Value *o = obj_unwrap(args[0]);
    if (!o) return mk_ref("Obj refused: requires an object (created by Obj::new / new)");
    if (strcmp(method, "get") == 0) {
        if (nargs < 2 || args[1]->kind != V_STR) return mk_ref("Obj refused: get requires a property name");
        Value *f = var_get_layer(o->obj_fields, args[1]->str);
        if (!f) {
            char buf[256];
            snprintf(buf, sizeof buf, "Obj refused: property %s was washed away", args[1]->str);
            return mk_ref(astrdup(buf));
        }
        return mk_res(f);
    }
    if (strcmp(method, "set") == 0) {
        if (nargs < 2 || args[1]->kind != V_STR) return mk_ref("Obj refused: set requires (property, value)");
        var_set(o->obj_fields, args[1]->str, args[2]);
        return mk_res(o);
    }
    if (strcmp(method, "forget") == 0) {
        if (nargs < 2 || args[1]->kind != V_STR) return mk_ref("Obj refused: forget requires a property name");
        var_del(o->obj_fields, args[1]->str);      /* the property is washed away */
        return mk_res(o);
    }
    if (strcmp(method, "call") == 0) {
        if (nargs < 2 || args[1]->kind != V_STR) return mk_ref("Obj refused: call requires a method name");
        const char *clsname = o->kind == V_ARR ? "Array" : o->obj_cls;
        Decl *cls = find_class(in, clsname);
        Method *m = cls ? class_method(cls, args[1]->str) : NULL;
        if (!m) {
            char buf[256];
            snprintf(buf, sizeof buf, "Obj refused: class %s has no method %s", clsname, args[1]->str);
            return mk_ref(astrdup(buf));
        }
        if (o->kind == V_OBJ) o->obj_fields->parent = in->cur_area;
        Result *r = interp_exec_method(in, m, args + 2, nargs - 2, o->kind == V_OBJ ? o->obj_fields : in->cur_area, o);
        if (r->ref && strcmp(r->ref, NOTHING) != 0) return mk_ref(r->ref);
        if (r->ref) return mk_res(mk_str(""));      /* implicit completion → success with no value */
        return mk_res(r->res);
    }
    if (strcmp(method, "class") == 0) return mk_res(mk_str(o->obj_cls));
    return mk_ref("Obj refused: no such method (new/get/set/forget/call/class)");
}

/* ---------- Time: Timestream timing stream ----------
 * Per the spec: a Timestream can hold multiple timers at once; the default first timer belongs to the thread
 * and may not be reset; forked timers (the second, third, ...) may be reset.
 */
static double now_sec(void) {
    return bio_now_sec();
}

typedef struct { int id; double start; int forked; } Timer;
static Timer timers[64];
static int ntimer = 0;
static int next_timer_id = 1000;   /* independent id for forked timers (avoids thread ids) */

static Timer *timer_find(int id) {
    for (int i = 0; i < ntimer; i++)
        if (timers[i].id == id) return &timers[i];
    return NULL;
}

/* Obtain or create a timer: first=1 means the thread's default first timer (may not be reset), otherwise treated as a fork */
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
        bio_sleep_ms(ms);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "start") == 0) {
        /* The default first timer belongs to the thread: use the current thread id when no argument is given */
        int id = (nargs > 0 && args[0]->kind == V_NUM) ? (int)args[0]->num
                 : (int)bts_request("self", NULL, 0)->res->num;
        int first = nargs < 1 || args[0]->kind != V_NUM;
        if (!timer_obtain(id, first)) return mk_ref("Time refused: timer table full");
        return mk_res(mk_num((double)id));
    }
    if (strcmp(method, "fork") == 0) {
        /* Fork a new timer: independent id, may be reset (the first one may not; forked ones may) */
        Timer *t = timer_obtain(next_timer_id++, 0);
        if (!t) return mk_ref("Time refused: timer table full");
        return mk_res(mk_num((double)t->id));
    }
    if (strcmp(method, "elapsed") == 0) {
        /* The default first timer belongs to the thread: use the current thread id when no argument is given */
        int id = (nargs > 0 && args[0]->kind == V_NUM) ? (int)args[0]->num
                 : (int)bts_request("self", NULL, 0)->res->num;
        Timer *t = timer_obtain(id, nargs < 1 || args[0]->kind != V_NUM);
        if (!t) return mk_ref("Time refused: timer table full");
        return mk_res(mk_num(now_sec() - t->start));
    }
    if (strcmp(method, "reset") == 0) {
        /* Per the spec: the default first timer belongs to the thread and may not be reset; forked ones may be reset */
        if (nargs < 1 || args[0]->kind != V_NUM)
            return mk_ref("Time refused: first timer (thread default) cannot be reset; use Time::fork()");
        int id = (int)args[0]->num;
        Timer *t = timer_find(id);
        if (!t) return mk_ref("Time refused: no such timer (create with Time::start / Time::fork)");
        if (!t->forked)
            return mk_ref("Time refused: first timer (thread default) cannot be reset; use Time::fork()");
        t->start = now_sec();
        return mk_res(mk_str(""));
    }
    return mk_ref("Time refused: no such method (now/sleep/start/fork/elapsed/reset)");
}

/* ---------- Rem: Remstream memory stream (persists to memory by default; can save/load) ---------- */
typedef struct { const char *key; Value *val; } Mem;
static Mem mems[256];
static int nmem = 0;

Result *rem_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "save") == 0) {
        if (nargs < 2 || args[0]->kind != V_STR) return mk_ref("Rem refused: save requires (name, value)");
        for (int i = 0; i < nmem; i++)
            if (strcmp(mems[i].key, args[0]->str) == 0) { mems[i].val = args[1]; return mk_res(mk_str("")); }
        if (nmem < 256) { mems[nmem].key = args[0]->str; mems[nmem].val = args[1]; nmem++; return mk_res(mk_str("")); }
        return mk_ref("Rem refused: memory full");
    }
    if (strcmp(method, "load") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR) return mk_ref("Rem refused: load requires a name");
        for (int i = 0; i < nmem; i++)
            if (strcmp(mems[i].key, args[0]->str) == 0) return mk_res(mems[i].val);
        return mk_ref("Rem refused: no such memory");
    }
    if (strcmp(method, "forget") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR) return mk_ref("Rem refused: forget requires a name");
        for (int i = 0; i < nmem; i++)
            if (strcmp(mems[i].key, args[0]->str) == 0) {
                mems[i] = mems[--nmem]; return mk_res(mk_str(""));
            }
        return mk_ref("Rem refused: no such memory");
    }
    return mk_ref("Rem refused: no such method (save/load/forget)");
}

/* ---------- Const: Constantstream constant stream (public constants, immutable once set) ---------- */
static Mem consts[128];
static int nconst = 0;

Result *const_request(const char *method, Value **args, int nargs) {
    if (strcmp(method, "set") == 0) {
        if (nargs < 2 || args[0]->kind != V_STR) return mk_ref("Const refused: set requires (name, value)");
        for (int i = 0; i < nconst; i++)
            if (strcmp(consts[i].key, args[0]->str) == 0)
                return mk_ref("Const refused: constant already exists, cannot overwrite");
        if (nconst < 128) { consts[nconst].key = args[0]->str; consts[nconst].val = args[1]; nconst++; return mk_res(mk_str("")); }
        return mk_ref("Const refused: constant table full");
    }
    if (strcmp(method, "get") == 0) {
        if (nargs < 1 || args[0]->kind != V_STR) return mk_ref("Const refused: get requires a name");
        for (int i = 0; i < nconst; i++)
            if (strcmp(consts[i].key, args[0]->str) == 0) return mk_res(consts[i].val);
        return mk_ref("Const refused: no such constant");
    }
    return mk_ref("Const refused: no such method (set/get)");
}

/* ---------- Ref: smart reference (&permission follow target name) ---------- */
/* Follow layers (per the spec): u program level (Unistream) / f method level (Functionstream) / a scope level (Areastream) */
VarMap *ref_layer_get(Interp *in, const char *follow) {
    if (strcmp(follow, "u") == 0) return &in->globals;
    if (strcmp(follow, "a") == 0) return in->cur_area;
    return in->cur_scope;                                    /* f = current method */
}

Result *ref_request(const char *method, Value **args, int nargs) {
    if (nargs < 1 || args[0]->kind != V_REF)
        return mk_ref("Ref refused: requires a reference object (&perm follow name)");
    Value *ref = args[0];
    Interp *in = g_interp;
    const char *perm = ref->ref_perm;
    const char *follow = ref->ref_follow;
    if (!(strcmp(perm, "r") == 0 || strcmp(perm, "w") == 0 ||
          strcmp(perm, "rw") == 0 || strcmp(perm, "m") == 0))
        return mk_ref("Ref refused: invalid reference permission (should be r/w/rw/m)");
    if (!(strcmp(follow, "u") == 0 || strcmp(follow, "f") == 0 || strcmp(follow, "a") == 0))
        return mk_ref("Ref refused: invalid reference follow (should be u/f/a)");
    VarMap *layer = ref_layer_get(in, follow);

    if (strcmp(method, "read") == 0) {
        if (strcmp(perm, "w") == 0)
            return mk_ref("Ref refused: reference is write-only, cannot read");
        Value *v = var_get_layer(layer, ref->ref_name);
        if (!v) {
            char buf[256];
            snprintf(buf, sizeof buf, "Ref refused: target %s does not exist (%s layer)", ref->ref_name, follow);
            return mk_ref(astrdup(buf));
        }
        return mk_res(v);
    }
    if (strcmp(method, "write") == 0) {
        if (strcmp(perm, "r") == 0)
            return mk_ref("Ref refused: reference is read-only, cannot write");
        if (nargs < 2) return mk_ref("Ref refused: write requires a value argument");
        var_set(layer, ref->ref_name, args[1]);
        return mk_res(mk_str(""));
    }
    if (strcmp(method, "move") == 0) {
        /* Moveable (m permission): take the target from its layer and return its value */
        if (strcmp(perm, "m") != 0)
            return mk_ref("Ref refused: move requires movable permission (m)");
        Value *v = var_get_layer(layer, ref->ref_name);
        if (!v) {
            char buf[256];
            snprintf(buf, sizeof buf, "Ref refused: target %s does not exist (%s layer)", ref->ref_name, follow);
            return mk_ref(astrdup(buf));
        }
        var_del(layer, ref->ref_name);           /* take it away (washed away) */
        return mk_res(v);
    }
    if (strcmp(method, "target") == 0)
        return mk_res(mk_str(ref->ref_name));
    if (strcmp(method, "perm") == 0)
        return mk_res(mk_str(perm));
    return mk_ref("Ref refused: no such method (read/write/move/target/perm)");
}

/* ---------- Binary library stream (B_BIN): dlsym calls ---------- */
#include <dlfcn.h>

/* Binary functions are all called as double(*)(double,...) (x86-64 SysV: double args go in xmm0-7, return value in xmm0) */
typedef double (*bin_fn)(double, double, double, double, double, double);

Result *bin_request(Stream *s, const char *method, Value **args, int nargs) {
    if (!s->dl) return mk_ref("binary stream refused: library not loaded");
    bin_fn fn = (bin_fn)dlsym(s->dl, method);
    if (!fn) return mk_ref("binary stream refused: no such symbol in library (or not a function)");
    if (nargs > 6) return mk_ref("binary stream refused: at most 6 arguments supported");
    double a[6] = {0};
    for (int i = 0; i < nargs; i++) {
        if (args[i]->kind == V_NUM) a[i] = args[i]->num;
        else return mk_ref("binary stream refused: binary functions support numeric arguments only");
    }
    double r = fn(a[0], a[1], a[2], a[3], a[4], a[5]);
    return mk_res(mk_num(r));
}

/* ---------- Com: Comstream computation stream (a branch of instantaneous streams, handling various instantaneous computations) ---------- */
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
        if (x < 0) return mk_ref("Com refused: sqrt argument cannot be negative");
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
        if (x <= 0) return mk_ref("Com refused: log argument must be positive");
        return mk_res(mk_num(log(x)));
    }
    if (strcmp(method, "exp") == 0) return mk_res(mk_num(exp(x)));
    return mk_ref("Com refused: no such method (abs/min/max/pow/sqrt/floor/ceil/round/sign/sin/cos/tan/log/exp)");
}

/* ---------- IO: IOStream (present by default; can read and output) ----------
 * parent stream, aggregating CIO (console) / FIO (file) / SIO (string), dispatching to each in order */
static Result *io_request(const char *method, Value **args, int nargs) {
    Result *r = cio_request(method, args, nargs);
    if (!r->ref) return r;
    r = fio_request(method, args, nargs);
    if (!r->ref) return r;
    r = sio_request(method, args, nargs);
    if (!r->ref) return r;
    return mk_ref("IO refused: no such method (aggregates CIO/FIO/SIO)");
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
            return mk_ref("unknown builtin stream");
    }
}
