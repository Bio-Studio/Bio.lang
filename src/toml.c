#include "bio.h"

/* ═══════════════ 极简 TOML 解析（package.toml 用）═══════════════
 * 支持：注释 #，顶层 name/version/repo，[dependencies] 节，
 *       依赖项 name = { version="..", repo=".." }（repo 可省）。
 * 足以支撑项目清单标准字段 version/name/repo。
 */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = aalloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { /* 忽略 */ }
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static char *trim(char *s) {
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static char *unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') { s[n-1] = 0; return s + 1; }
    return s;
}

TomlTable *toml_parse_file(const char *path) {
    char *src = read_file(path);
    if (!src) return NULL;

    TomlTable *t = aalloc(sizeof(TomlTable));
    /* 收集键值对到 arena 数组（够用：标准字段少） */
    static const char *keys[256];
    static const char *vals[256];
    static int nkv = 0;
    nkv = 0;

    int in_deps = 0;
    char *line = src;
    for (char *p = src; ; p++) {
        if (*p == '\n' || *p == 0) {
            char c = *p; *p = 0;
            char *l = trim(line);
            if (*l && *l != '#') {
                if (l[0] == '[') {
                    in_deps = strstr(l, "dependencies") != NULL;
                } else {
                    char *eq = strchr(l, '=');
                    if (eq) {
                        *eq = 0;
                        char *k = trim(l);
                        char *v = trim(eq + 1);
                        if (in_deps) {
                            /* 依赖项：name = { version="..", repo=".." } */
                            if (*v == '{') {
                                /* 存 "@dep" 表：把内联表原文存为 val，后续解析 */
                                if (nkv < 256) { keys[nkv] = k; vals[nkv] = v; nkv++; }
                            }
                        } else {
                            v = unquote(v);
                            if (nkv < 256) { keys[nkv] = k; vals[nkv] = v; nkv++; }
                        }
                    }
                }
            }
            (void)c;
            if (c == 0) break;
            line = p + 1;
        }
    }

    t->pairs = aalloc(sizeof(TomlPair) * 256);
    t->n = 0;
    for (int i = 0; i < nkv; i++) {
        const char *k = keys[i];
        const char *v = vals[i];
        /* 依赖项（val 是内联表原文）：解析 version/repo */
        if (v && v[0] == '{') {
            char *ver = NULL, *repo = NULL;
            char *vv = (char*)v;   /* 内联表 { version="..", repo=".." } */
            for (char *q = vv + 1; *q && *q != '}'; q++) {
                /* 找 key = */
                char *ks = q;
                while (*q && *q != '=' && *q != '}' && *q != ',') q++;
                if (*q != '=') continue;
                char *ke = q;
                *ke = 0;
                q++;               /* = */
                while (*q && (*q == ' ' || *q == '\t')) q++;
                char *vs = q;
                if (*q == '"') {
                    q++;
                    while (*q && *q != '"') q++;
                    if (*q == '"') q++;
                } else {
                    while (*q && *q != ',' && *q != '}') q++;
                }
                char *ve = q;
                char old = *ve; *ve = 0;
                char *k = trim(ks);
                char *val = unquote(trim(vs));
                if (strcmp(k, "version") == 0) ver = val;
                else if (strcmp(k, "repo") == 0) repo = val;
                *ve = old;
                if (old == ',' || old == '}') { /* 继续 */ }
            }
            /* 存三键：@dep:name（存在标记）、@dep:name:version、@dep:name:repo */
            char *k1 = aalloc(strlen(k) + 16);
            snprintf(k1, strlen(k) + 16, "@dep:%s", k);
            if (t->n < 256) { t->pairs[t->n].key = k1; t->pairs[t->n].val = "1"; t->n++; }
            if (ver && t->n < 256) {
                char *k2 = aalloc(strlen(k) + 24);
                snprintf(k2, strlen(k) + 24, "@dep:%s:version", k);
                t->pairs[t->n].key = k2; t->pairs[t->n].val = ver; t->n++;
            }
            if (repo && t->n < 256) {
                char *k3 = aalloc(strlen(k) + 20);
                snprintf(k3, strlen(k) + 20, "@dep:%s:repo", k);
                t->pairs[t->n].key = k3; t->pairs[t->n].val = repo; t->n++;
            }
        } else {
            if (t->n < 256) { t->pairs[t->n].key = k; t->pairs[t->n].val = v; t->n++; }
        }
    }
    return t;
}

const char *toml_get(TomlTable *t, const char *key) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->pairs[i].key, key) == 0) return t->pairs[i].val;
    return NULL;
}

/* 遍历依赖：idx 从 0 起；返回依赖名并写入 *name；到末尾返回 NULL */
const char *toml_dep(TomlTable *t, int idx, const char **name) {
    int d = 0;
    for (int i = 0; i < t->n; i++) {
        if (strncmp(t->pairs[i].key, "@dep:", 5) == 0 &&
            !strchr(t->pairs[i].key + 5, ':')) {
            if (d == idx) { if (name) *name = t->pairs[i].key + 5; return t->pairs[i].key + 5; }
            d++;
        }
    }
    return NULL;
}

/* 查某依赖的 version/repo；field = "version"/"repo" */
const char *toml_dep_field(TomlTable *t, const char *dep, const char *field) {
    char key[128];
    snprintf(key, sizeof key, "@dep:%s:%s", dep, field);
    return toml_get(t, key);
}

/* 全局默认 repo：环境变量 BIOLANG_CONFIG → 全局配置文件（TOML）里的 repo。
 * 配置格式示例：
 *   repo = "https://.../biolang-repos"
 *   [repo]
 *   url = "https://..."
 */
const char *global_repo(void) {
    const char *cfgpath = getenv("BIOLANG_CONFIG");
    if (!cfgpath) {
        /* 系统默认路径（跨系统）：~/.biolang/config.toml */
        const char *home = getenv("HOME");
        if (home) {
            static char p[1024];
            snprintf(p, sizeof p, "%s/.biolang/config.toml", home);
            cfgpath = p;
        } else return NULL;
    }
    TomlTable *t = toml_parse_file(cfgpath);
    if (!t) return NULL;
    const char *r = toml_get(t, "repo");
    if (r) return r;
    return toml_get(t, "url");   /* [repo] url = ... */
}
