#include "bio.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ═══════════════ 项目式编译运行 ═══════════════
 * 项目结构：
 *   package.toml        清单（name/version/repo + [dependencies]）
 *   src/                应用源码（main.bio 为入口）
 *   utils/              库/工具箱（.bio）
 *   .biolang/deps/      安装的依赖
 * need 捆绑：按需最小集 —— 从 main 入口开始收集 need，在注册表里找提供者，
 * 递归扩展，直到闭包稳定；任何 need 无提供者 → 报错。
 */

typedef struct ProjFile {
    const char *path;       /* 相对项目根路径（用于信息展示） */
    char *content;          /* 读入源码 */
    Decl *decls;            /* 解析结果 */
    int included;           /* 被捆绑 */
    struct ProjFile *next;
} ProjFile;

static ProjFile *files = NULL;

/* ── 文件系统辅助 ── */
static char *read_whole(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = aalloc((size_t)sz + 1);
    if (fread(b, 1, sz, f) != (size_t)sz) { /* 忽略 */ }
    b[sz] = 0;
    fclose(f);
    return b;
}

static char *join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = aalloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = '/';
    memcpy(r + la + 1, b, lb + 1);
    return r;
}

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/* 递归收集 dir 下所有 .bio/.bl 文件，加入注册表 */
static void scan_dir(const char *dir, const char *relbase) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *p = join(dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            scan_dir(p, relbase);
        } else if (S_ISREG(st.st_mode) && (ends_with(e->d_name, ".bio") || ends_with(e->d_name, ".bl"))) {
            char *content = read_whole(p);
            if (!content) continue;
            int ntok, err = 0;
            Tok *toks = tokenize(content, &ntok);
            Decl *decls = parse_program_tokens(toks, ntok, &err);
            if (err) { fprintf(stderr, "⚠️ parse failed: %s\n", p); continue; }
            ProjFile *f = aalloc(sizeof(ProjFile));
            f->path = relbase ? join(relbase, e->d_name) : e->d_name;
            f->content = content;
            f->decls = decls;
            f->included = 0;
            f->next = files;
            files = f;
        }
    }
    closedir(d);
}

/* ── 提供者 / 需求 识别 ── */
static int decl_is(Decl *d, const char *kind, const char *name) {
    if (d->kind == D_SIG || d->kind == D_CLASS)
        return strcmp(d->name, name) == 0;
    if (d->kind == D_FORK) return strcmp(d->name, name) == 0;   /* 实现流 */
    if (d->kind == D_CONST && strcmp(kind, "value") == 0) return strcmp(d->name, name) == 0;
    return 0;
}

/* 该文件是否提供 kind 名为 name 的东西（kind: stream/Class/value/function） */
static int file_provides(ProjFile *f, const char *kind, const char *name) {
    for (Decl *d = f->decls; d; d = d->next) {
        if (decl_is(d, kind, name)) return 1;
        if (strcmp(kind, "function") == 0) {
            /* 方法提供者：方法名 = name（D_FORK/D_CLASS 里的方法） */
            if (d->kind == D_FORK || d->kind == D_CLASS) {
                for (int i = 0; i < d->nmethods; i++)
                    if (strcmp(d->methods[i].name, name) == 0) return 1;
            }
        }
    }
    return 0;
}

/* 收集该文件所有 need 到数组（kind,name）对 */
static void file_needs(ProjFile *f, const char **kinds, const char **names, int *n) {
    *n = 0;
    for (Decl *d = f->decls; d; d = d->next) {
        if (d->kind == D_NEED && *n < 64) {
            kinds[*n] = d->needkind; names[*n] = d->name; (*n)++;
        }
    }
}

/* 找提供 need 的文件；返回 NULL 表示无提供者 */
static ProjFile *find_provider(const char *kind, const char *name) {
    for (ProjFile *f = files; f; f = f->next)
        if (file_provides(f, kind, name)) return f;
    return NULL;
}

/* ── 按需捆绑最小集 ──
 * 从入口文件开始，BFS 收集 need 提供者，递归扩展。
 * 返回 0 成功；1 有未满足 need。 */
static int bundle_from(ProjFile *entry) {
    entry->included = 1;
    ProjFile *queue[512]; int qn = 0; int qi = 0;
    queue[qn++] = entry;
    int unmet = 0;
    while (qi < qn) {
        ProjFile *cur = queue[qi++];
        const char *kinds[64], *names[64]; int nn = 0;
        file_needs(cur, kinds, names, &nn);
        for (int i = 0; i < nn; i++) {
            /* 已在注册表里找到（当前程序含该名）则跳过 */
            int ok = 0;
            for (ProjFile *f = files; f; f = f->next)
                if (f->included && file_provides(f, kinds[i], names[i])) { ok = 1; break; }
            if (ok) continue;
            ProjFile *p = find_provider(kinds[i], names[i]);
            if (!p) {
                fprintf(stderr, "⛔ need %s %s not provided by any file (src/ utils/ deps)\n", kinds[i], names[i]);
                unmet = 1;
                continue;
            }
            if (!p->included) { p->included = 1; queue[qn++] = p; }
        }
    }
    return unmet;
}

/* 拼接所有 included 文件的源码 */
static char *concat_bundle(void) {
    size_t total = 0;
    for (ProjFile *f = files; f; f = f->next)
        if (f->included) total += strlen(f->content) + 1;
    char *out = aalloc(total + 1);
    out[0] = 0;
    for (ProjFile *f = files; f; f = f->next)
        if (f->included) { strcat(out, f->content); strcat(out, "\n"); }
    return out;
}

/* 找入口文件：src/main.bio 或 src 下第一个 .bio */
static ProjFile *find_entry(const char *dir) {
    (void)dir;
    for (ProjFile *f = files; f; f = f->next) {
        if (strcmp(f->path, "main.bio") == 0) return f;
    }
    /* src/ 下第一个 */
    for (ProjFile *f = files; f; f = f->next)
        if (strstr(f->path, "src/")) return f;
    return files;
}

static int mkdirs(const char *dir) {
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
    return 0;
}

/* ── 项目命令 ── */

/* bio init <name>：创建项目骨架 */
int project_init(const char *name) {
    mkdirs(name);
    mkdirs(join(name, "src"));
    mkdirs(join(name, "utils"));
    mkdirs(join(name, ".biolang/deps"));
    char *toml = join(name, "package.toml");
    FILE *f = fopen(toml, "w");
    if (!f) { fprintf(stderr, "cannot create %s\n", toml); return 1; }
    fprintf(f,
        "name = \"%s\"\n"
        "version = \"0.1.0\"\n"
        "# repo = \"https://...\"   # 可选：本包仓库\n"
        "# 依赖（repo 可选，默认用全局配置 BIOLANG_CONFIG 或 ~/.biolang/config.toml）\n"
        "[dependencies]\n"
        "# libfoo = { version = \"1.0.0\" }\n"
        "# libbar = { version = \"0.2.0\", repo = \"https://...\" }\n",
        name);
    fclose(f);
    char *main = join(join(name, "src"), "main.bio");
    FILE *m = fopen(main, "w");
    if (m) {
        fprintf(m, "program main;\n\nMain {\n    void exec() {\n        IO::println(\"Hello from %s!\");\n    }\n}\n", name);
        fclose(m);
    }
    printf("✔ project created: %s\n", name);
    printf("  package.toml  — manifest (name/version/repo + deps)\n");
    printf("  src/main.bio  — entry\n");
    printf("  utils/        — local libraries (need-satisfying .bio)\n");
    printf("  .biolang/deps — installed dependencies\n");
    return 0;
}

/* bio build [dir]：按需捆绑 + 编译 */
int project_build(const char *dir, const char *out) {
    char *src_dir = join(dir, "src");
    char *utils_dir = join(dir, "utils");
    char *deps_dir = join(dir, ".biolang/deps");
    files = NULL;
    scan_dir(src_dir, "src");
    scan_dir(utils_dir, "utils");
    scan_dir(deps_dir, ".biolang/deps");
    if (!files) { fprintf(stderr, "no .bio files in %s\n", src_dir); return 1; }
    ProjFile *entry = find_entry(dir);
    if (!entry) { fprintf(stderr, "no entry file\n"); return 1; }
    if (bundle_from(entry)) {
        fprintf(stderr, "⛔ build failed: unmet needs\n");
        return 1;
    }
    char *src = concat_bundle();
    if (!out) out = "app";
    printf("building %d bundled file(s) → %s\n", 0, out);
    int n = 0;
    for (ProjFile *f = files; f; f = f->next) if (f->included) n++;
    printf("bundled %d file(s)\n", n);
    if (compile_program(src, out) != 0) return 1;
    printf("✔ build ok: %s\n", out);
    return 0;
}

/* bio run [dir]：解释运行项目 */
int project_run(const char *dir) {
    char *src_dir = join(dir, "src");
    char *utils_dir = join(dir, "utils");
    char *deps_dir = join(dir, ".biolang/deps");
    files = NULL;
    scan_dir(src_dir, "src");
    scan_dir(utils_dir, "utils");
    scan_dir(deps_dir, ".biolang/deps");
    if (!files) { fprintf(stderr, "no .bio files in %s\n", src_dir); return 1; }
    ProjFile *entry = find_entry(dir);
    if (bundle_from(entry)) {
        fprintf(stderr, "⛔ run failed: unmet needs\n");
        return 1;
    }
    char *src = concat_bundle();
    run_source(src);
    return 0;
}

/* bio install [dir]：读 package.toml 依赖，按 repo 拉取到 .biolang/deps/<name>/ */
int project_install(const char *dir) {
    char *toml = join(dir, "package.toml");
    TomlTable *t = toml_parse_file(toml);
    if (!t) { fprintf(stderr, "no package.toml in %s\n", dir); return 1; }
    const char *name;
    int idx = 0, any = 0;
    while ((name = toml_dep(t, idx, NULL))) {
        any = 1;
        const char *repo = toml_dep_field(t, name, "repo");
        const char *ver = toml_dep_field(t, name, "version");
        if (!repo) repo = global_repo();   /* 依赖自身 → 全局配置 */
        if (!repo) {
            fprintf(stderr, "⛔ dep %s: no repo (set repo= or BIOLANG_CONFIG global repo)\n", name);
            idx++; continue;
        }
        char *depsroot = join(dir, ".biolang/deps");
        mkdirs(depsroot);
        char *dest = join(depsroot, name);
        char cmd[4096];
        if (strncmp(repo, "http", 4) == 0 && strstr(repo, ".git")) {
            /* git 仓库 */
            snprintf(cmd, sizeof cmd, "git clone --depth 1 %s %s 2>/dev/null", repo, dest);
        } else if (strncmp(repo, "http", 4) == 0) {
            /* http 下载（假设 repo 指向 package.toml，下载到临时再解？v1 简化为 curl 到 dest/package.toml） */
            mkdirs(dest);
            snprintf(cmd, sizeof cmd, "curl -fsSL %s -o %s/package.toml 2>/dev/null", repo, dest);
        } else {
            /* 本地路径 */
            mkdirs(dest);
            snprintf(cmd, sizeof cmd, "cp -r %s/. %s/ 2>/dev/null", repo, dest);
        }
        if (system(cmd) != 0) {
            fprintf(stderr, "⛔ dep %s: fetch failed from %s\n", name, repo);
        } else {
            printf("✔ installed %s%s%s\n", name, ver ? " v" : "", ver ? ver : "");
        }
        idx++;
    }
    if (!any) printf("ℹ️ no dependencies in %s\n", toml);
    return 0;
}

/* bio destroy [dir]：删除构建产物（.biolang/ 和 app 可执行文件，保留源码） */
int project_destroy(const char *dir) {
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "rm -rf %s/.biolang %s/app %s/build", dir, dir, dir);
    system(cmd);
    printf("✔ destroyed build artifacts: %s/.biolang, %s/app\n", dir, dir);
    return 0;
}
