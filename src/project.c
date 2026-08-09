#include "bio.h"
#include "platform.h"

#ifndef BIO_HOME
#define BIO_HOME "."
#endif
#ifndef BIO_CC
#define BIO_CC "gcc"
#endif
#ifndef BIO_LDFLAGS
#define BIO_LDFLAGS "-lm"
#endif

/* ═══════════════ project-based build & run ═══════════════
 * Project structure:
 *   package.toml        manifest (name/version/repo + [dependencies])
 *   src/                application source (main.bio is the entry)
 *   utils/              library/toolbox (.bio)
 *   .biolang/deps/      installed dependencies
 * need bundling: on-demand minimal set — collect needs starting from the main entry,
 * find providers in the registry, expand recursively until the closure is stable;
 * any need without a provider → error.
 */

typedef struct ProjFile {
    const char *path;       /* path relative to project root (for display) */
    char *content;          /* source code read in */
    Decl *decls;            /* parse result */
    int included;           /* bundled */
    struct ProjFile *next;
} ProjFile;

static ProjFile *files = NULL;

/* ── filesystem helpers ── */
static char *read_whole(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = aalloc((size_t)sz + 1);
    if (fread(b, 1, sz, f) != (size_t)sz) { /* ignored */ }
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

/* recursively collect all .bio/.bl files under dir and add them to the registry */
typedef struct { const char *relbase; } ScanCtx;

static void scan_dir(const char *dir, const char *relbase);   /* forward (scan_cb recurses) */

static void scan_cb(const char *path, int is_dir, void *ud) {
    ScanCtx *c = ud;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (is_dir) {
        scan_dir(path, c->relbase);   /* recurse (same relbase, as before) */
        return;
    }
    if (!(ends_with(name, ".bio") || ends_with(name, ".bl"))) return;
    char *content = read_whole(path);
    if (!content) return;
    int ntok, err = 0;
    Tok *toks = tokenize(content, &ntok);
    Decl *decls = parse_program_tokens(toks, ntok, &err);
    if (err) { fprintf(stderr, "⚠️ parse failed: %s\n", path); return; }
    ProjFile *f = aalloc(sizeof(ProjFile));
    f->path = c->relbase ? join(c->relbase, name) : name;
    f->content = content;
    f->decls = decls;
    f->included = 0;
    f->next = files;
    files = f;
}

static void scan_dir(const char *dir, const char *relbase) {
    ScanCtx c = { relbase };
    bio_walk_dir(dir, scan_cb, &c);
}

/* ── provider / requirement recognition ── */
static int decl_is(Decl *d, const char *kind, const char *name) {
    if (d->kind == D_SIG || d->kind == D_CLASS)
        return strcmp(d->name, name) == 0;
    if (d->kind == D_FORK) return strcmp(d->name, name) == 0;   /* implementation stream */
    if (d->kind == D_CONST && strcmp(kind, "value") == 0) return strcmp(d->name, name) == 0;
    return 0;
}

/* whether the file provides something of the given kind named name (kind: stream/Class/value/function) */
static int file_provides(ProjFile *f, const char *kind, const char *name) {
    for (Decl *d = f->decls; d; d = d->next) {
        if (decl_is(d, kind, name)) return 1;
        if (strcmp(kind, "function") == 0) {
            /* method provider: method name = name (methods inside D_FORK/D_CLASS) */
            if (d->kind == D_FORK || d->kind == D_CLASS) {
                for (int i = 0; i < d->nmethods; i++)
                    if (strcmp(d->methods[i].name, name) == 0) return 1;
            }
        }
    }
    return 0;
}

/* collect all needs of the file into an array of (kind, name) pairs */
static void file_needs(ProjFile *f, const char **kinds, const char **names, int *n) {
    *n = 0;
    for (Decl *d = f->decls; d; d = d->next) {
        if (d->kind == D_NEED && *n < 64) {
            kinds[*n] = d->needkind; names[*n] = d->name; (*n)++;
        }
    }
}

/* find the file providing the need; returns NULL if there is no provider */
static ProjFile *find_provider(const char *kind, const char *name) {
    for (ProjFile *f = files; f; f = f->next)
        if (file_provides(f, kind, name)) return f;
    return NULL;
}

/* ── on-demand bundling of the minimal set ──
 * Starting from the entry file, BFS-collect need providers and expand recursively.
 * Returns 0 on success; 1 if some needs are unmet. */
static int bundle_from(ProjFile *entry) {
    entry->included = 1;
    ProjFile *queue[BIO_QUEUE_MAX]; int qn = 0; int qi = 0;
    queue[qn++] = entry;
    int unmet = 0;
    while (qi < qn) {
        ProjFile *cur = queue[qi++];
        const char *kinds[64], *names[64]; int nn = 0;
        file_needs(cur, kinds, names, &nn);
        for (int i = 0; i < nn; i++) {
            /* skip if already found in the registry (current program contains that name) */
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

/* concatenate the source of all included files */
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

/* ═══════════════ Incremental multi-file compilation ═══════════════
 * Every bundled .bio file becomes its own C module (embedded source string);
 * each module is compiled to a cached .o keyed by content hash, so only
 * changed files are recompiled. The C runtime is cached once per
 * BIO_RUNTIME_VERSION. Finally all objects are linked into one executable
 * (or a .img/.zip package). */

static unsigned long long fnv1a(const char *s) {
    unsigned long long h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h;
}

static char *escape_cstr(const char *s) {
    size_t n = strlen(s);
    char *out = aalloc(n * 2 + 2);
    char *p = out;
    for (const char *q = s; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if (c == '"') { *p++ = '\\'; *p++ = '"'; }
        else if (c == '\\') { *p++ = '\\'; *p++ = '\\'; }
        else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
        else if (c == '\t') { *p++ = '\\'; *p++ = 't'; }
        else if (c == '\r') { *p++ = '\\'; *p++ = 'r'; }
        else *p++ = (char)c;
    }
    *p = 0;
    return out;
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static const char *RUNTIME_SRC[] = {
    "arena.c", "lexer.c", "value.c", "builtin.c", "parser.c",
    "interp.c", "bts.c", "compile.c", "toml.c", "project.c",
    "platform.c", "pack.c"
};
#define RUNTIME_N ((int)(sizeof RUNTIME_SRC / sizeof *RUNTIME_SRC))

static int ensure_runtime_objs(const char *rtdir, const char ***objs, int *n) {
    bio_mkdir_p(rtdir);
    *objs = aalloc(sizeof(char *) * RUNTIME_N);
    *n = 0;
    for (int i = 0; i < RUNTIME_N; i++) {
        char obj[BIO_PATH_MAX + 32], srcpath[BIO_PATH_MAX + 32];
        snprintf(obj, sizeof obj, "%s/%s.o", rtdir, RUNTIME_SRC[i]);
        if (!file_exists(obj)) {
            snprintf(srcpath, sizeof srcpath, "%s/src/%s", BIO_HOME, RUNTIME_SRC[i]);
            char hf[BIO_PATH_MAX + 64], cf[BIO_PATH_MAX + 64], lf[BIO_PATH_MAX + 64];
            snprintf(hf, sizeof hf, "-DBIO_HOME=\"%s\"", BIO_HOME);
            snprintf(cf, sizeof cf, "-DBIO_CC=\"%s\"", BIO_CC);
            snprintf(lf, sizeof lf, "-DBIO_LDFLAGS=\"%s\"", BIO_LDFLAGS);
            const char *av[] = { BIO_CC, "-O2", "-Isrc", hf, cf, lf,
                                 "-c", srcpath, "-o", obj, NULL };
            if (bio_run(av) != 0) return -1;
        }
        (*objs)[(*n)++] = astrdup(obj);
    }
    return 0;
}

static int compile_bio_module(const char *objdir, int idx, const char *content,
                              const char *hash) {
    char cpath[BIO_PATH_MAX + 32], opath[BIO_PATH_MAX + 32], hpath[BIO_PATH_MAX + 32];
    snprintf(cpath, sizeof cpath, "%s/%d.c", objdir, idx);
    snprintf(opath, sizeof opath, "%s/%d.o", objdir, idx);
    snprintf(hpath, sizeof hpath, "%s/%d.hash", objdir, idx);
    char old[BIO_STR_MAX] = "";
    FILE *hf = fopen(hpath, "r");
    if (hf) {
        if (fgets(old, sizeof old, hf)) old[strcspn(old, "\n")] = 0;
        fclose(hf);
    }
    if (file_exists(opath) && strcmp(old, hash) == 0) return 0;   /* cached */
    char *esc = escape_cstr(content);
    char *driver = aalloc(strlen(esc) + 64);
    sprintf(driver, "const char *bio_src_%d = \"%s\";\n", idx, esc);
    if (write_text(cpath, driver) != 0) return -1;
    const char *av[] = { BIO_CC, "-O2", "-Isrc", "-c", cpath, "-o", opath, NULL };
    if (bio_run(av) != 0) return -1;
    FILE *w = fopen(hpath, "w");
    if (w) { fprintf(w, "%s\n", hash); fclose(w); }
    return 1;   /* recompiled */
}

static int compile_project_main(const char *objdir, int nfiles) {
    char mainc[BIO_PATH_MAX + 32], maino[BIO_PATH_MAX + 32];
    snprintf(mainc, sizeof mainc, "%s/main.c", objdir);
    snprintf(maino, sizeof maino, "%s/main.o", objdir);
    FILE *f = fopen(mainc, "w");
    if (!f) return -1;
    fprintf(f,
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include \"bio.h\"\n");
    for (int i = 0; i < nfiles; i++) fprintf(f, "extern const char *bio_src_%d;\n", i);
    fprintf(f, "int main(void) {\n"
               "    const char *ml = getenv(\"BIO_MEM_LIMIT\");\n"
               "    bio_set_mem_limit(ml && *ml ? (size_t)strtoull(ml, 0, 0) : 0);\n"
               "    const char *parts[%d];\n", nfiles + 1);
    for (int i = 0; i < nfiles; i++) fprintf(f, "    parts[%d] = bio_src_%d;\n", i, i);
    fprintf(f,
        "    parts[%d] = NULL;\n"
        "    size_t total = 1;\n"
        "    for (int i = 0; parts[i]; i++) total += strlen(parts[i]) + 1;\n"
        "    char *buf = aalloc(total); buf[0] = 0;\n"
        "    for (int i = 0; parts[i]; i++) { strcat(buf, parts[i]); strcat(buf, \"\\n\"); }\n"
        "    run_source(buf);\n"
        "    return 0;\n"
        "}\n", nfiles);
    fclose(f);
    const char *av[] = { BIO_CC, "-O2", "-Isrc", "-c", mainc, "-o", maino, NULL };
    return bio_run(av);
}

/* Split a whitespace-separated string into argv entries (points into s). */
static int split_args(const char *s, const char **argv, int cap) {
    int n = 0;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (n >= cap) break;
        argv[n++] = s;
        while (*s && !isspace((unsigned char)*s)) s++;
    }
    argv[n] = NULL;
    return n;
}

static int link_project(const char *out, const char *objdir, int nfiles,
                        const char **rtobjs, int nrt) {
    const char *ld[8];
    int nld = split_args(BIO_LDFLAGS, ld, 8);
    const char **av = aalloc(sizeof(char *) * (size_t)(12 + nfiles + nrt + nld));
    int n = 0;
    av[n++] = BIO_CC;
    av[n++] = "-O2";
    av[n++] = "-Isrc";
    av[n++] = "-o";
    av[n++] = out;
    char maino[BIO_PATH_MAX + 32];
    snprintf(maino, sizeof maino, "%s/main.o", objdir);
    av[n++] = astrdup(maino);
    for (int i = 0; i < nfiles; i++) {
        char fo[BIO_PATH_MAX + 32];
        snprintf(fo, sizeof fo, "%s/%d.o", objdir, i);
        av[n++] = astrdup(fo);
    }
    for (int i = 0; i < nrt; i++) av[n++] = rtobjs[i];
    for (int i = 0; i < nld; i++) av[n++] = ld[i];
    av[n] = NULL;
    return bio_run(av);
}

const char *project_name(const char *dir) {
    static char name[BIO_NAME_MAX] = "app";
    char *toml = join(dir, "package.toml");
    FILE *f = fopen(toml, "r");
    if (f) {
        char line[BIO_MSG_MAX];
        while (fgets(line, sizeof line, f))
            if (sscanf(line, " name = \"%63[^\"]\"", name) == 1) break;
        fclose(f);
    }
    return name;
}

/* Current platform's release directory name under bin/ (linux-x86_64, win64, ...). */
static const char *current_platform_dir(void) {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "win-arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return "win64";
#else
    return "win32";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "macos-arm64";
#else
    return "macos-x86_64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x86_64";
#endif
#else
    return NULL;
#endif
}

typedef struct { const char **paths; int n; int cap; } FileList;

static void pkg_cb(const char *path, int is_dir, void *ud) {
    FileList *fl = ud;
    if (is_dir || fl->n >= fl->cap) return;
    fl->paths[fl->n++] = astrdup(path);
}

/* find the entry file: src/main.bio, or the first .bio under src */
static ProjFile *find_entry(const char *dir) {
    (void)dir;
    for (ProjFile *f = files; f; f = f->next) {
        if (strcmp(f->path, "main.bio") == 0) return f;
    }
    /* first one under src/ */
    for (ProjFile *f = files; f; f = f->next)
        if (strstr(f->path, "src/")) return f;
    return files;
}

/* ── project commands ── */

/* bio init <name>: create a project skeleton */
int project_init(const char *name) {
    bio_mkdir_p(name);
    bio_mkdir_p(join(name, "src"));
    bio_mkdir_p(join(name, "utils"));
    bio_mkdir_p(join(name, ".biolang/deps"));
    char *toml = join(name, "package.toml");
    FILE *f = fopen(toml, "w");
    if (!f) { fprintf(stderr, "cannot create %s\n", toml); return 1; }
    fprintf(f,
        "name = \"%s\"\n"
        "version = \"0.1.0\"\n"
        "# repo = \"https://...\"   # optional: this package's repository\n"
        "# dependencies (repo optional; defaults to the global config BIOLANG_CONFIG or ~/.biolang/config.toml)\n"
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

/* bio build [dir]: on-demand bundling + compile */
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
    int n = 0;
    for (ProjFile *f = files; f; f = f->next) if (f->included) n++;
    printf("bundled %d file(s)\n", n);

    const char *pname = project_name(dir);
    char objdir[BIO_PATH_MAX], rtdir[BIO_PATH_MAX];
    snprintf(objdir, sizeof objdir, "bin/.cache/project/%s", pname);
    snprintf(rtdir, sizeof rtdir, "bin/.cache/runtime/%s", BIO_RUNTIME_VERSION);
    bio_mkdir_p(objdir);

    const char **rtobjs = NULL;
    int nrt = 0;
    if (ensure_runtime_objs(rtdir, &rtobjs, &nrt) != 0) {
        fprintf(stderr, "⛔ runtime cache build failed\n");
        return 1;
    }

    int idx = 0, recompiled = 0;
    for (ProjFile *f = files; f; f = f->next) {
        if (!f->included) continue;
        char hash[BIO_NAME_MAX];
        snprintf(hash, sizeof hash, "%016llx",
                 (unsigned long long)fnv1a(f->content));
        int rc = compile_bio_module(objdir, idx, f->content, hash);
        if (rc < 0) {
            fprintf(stderr, "⛔ compile failed: %s\n", f->path);
            return 1;
        }
        recompiled += rc;
        idx++;
    }
    if (compile_project_main(objdir, n) != 0) {
        fprintf(stderr, "⛔ project driver compile failed\n");
        return 1;
    }
    printf("incremental: %d module(s) cached, %d recompiled\n", n - recompiled, recompiled);

    char default_out[BIO_PATH_MAX];
    if (!out) {
        snprintf(default_out, sizeof default_out, "bin/%s", pname);
        out = default_out;
    }
    bio_mkdir_p("bin");
    const char *final = out;
    char appbin[BIO_PATH_MAX + 32];
    if (ends_with(out, ".img") || ends_with(out, ".zip")) {
        snprintf(appbin, sizeof appbin, "%s/app", objdir);
        final = appbin;
    }
    if (link_project(final, objdir, n, rtobjs, nrt) != 0) {
        fprintf(stderr, "⛔ link failed\n");
        return 1;
    }
    if (final != out) {
        /* Package the app binary together with the current platform's bio CLI
         * and its lib/ dependencies. */
        const char **pkg = aalloc(sizeof(char *) * 64);
        int np = 0;
        pkg[np++] = appbin;
        const char *pf = current_platform_dir();
        if (pf) {
#if defined(_WIN32)
            const char *exe = ".exe";
#else
            const char *exe = "";
#endif
            char cli[BIO_PATH_MAX + 32], libdir[BIO_PATH_MAX + 32];
            snprintf(cli, sizeof cli, "bin/%s/bin/bio%s", pf, exe);
            if (file_exists(cli)) pkg[np++] = astrdup(cli);
            snprintf(libdir, sizeof libdir, "bin/%s/lib", pf);
            FileList fl = { pkg + np, 0, 64 - np };
            bio_walk_dir(libdir, pkg_cb, &fl);
            np += fl.n;
        }
        int rc = ends_with(out, ".img")
            ? img_create(out, "app", pkg, np)
            : zip_create(out, pkg, np);
        if (rc != 0) {
            fprintf(stderr, "⛔ package failed: %s\n", out);
            return 1;
        }
    }
    printf("✔ build ok: %s\n", out);
    return 0;
}

/* bio run [dir]: interpret and run the project */
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

/* bio install [dir]: read dependencies from package.toml and fetch them by repo into .biolang/deps/<name>/ */
/* git/curl run as direct processes (no shell); local paths copy with stdio. */
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
        if (!repo) repo = global_repo();   /* dependency itself → global config */
        if (!repo) {
            fprintf(stderr, "⛔ dep %s: no repo (set repo= or BIOLANG_CONFIG global repo)\n", name);
            idx++; continue;
        }
        char *depsroot = join(dir, ".biolang/deps");
        bio_mkdir_p(depsroot);
        char *dest = join(depsroot, name);
        int rc;
        if (strncmp(repo, "http", 4) == 0 && strstr(repo, ".git")) {
            /* git repository */
            const char *argv[] = { "git", "clone", "--depth", "1", repo, dest, NULL };
            rc = bio_run(argv);
        } else if (strncmp(repo, "http", 4) == 0) {
            /* http download (assumes repo points to package.toml; v1 simplifies to curl into dest/package.toml) */
            bio_mkdir_p(dest);
            char *outfile = join(dest, "package.toml");
            const char *argv[] = { "curl", "-fsSL", repo, "-o", outfile, NULL };
            rc = bio_run(argv);
        } else {
            /* local path */
            rc = bio_copy_tree(repo, dest);
        }
        if (rc != 0) {
            fprintf(stderr, "⛔ dep %s: fetch failed from %s\n", name, repo);
        } else {
            printf("✔ installed %s%s%s\n", name, ver ? " v" : "", ver ? ver : "");
        }
        idx++;
    }
    if (!any) printf("ℹ️ no dependencies in %s\n", toml);
    return 0;
}

/* bio destroy [dir]: delete build artifacts (.biolang/ and the app executable, keep source) */
int project_destroy(const char *dir) {
    bio_rm_tree(join(dir, ".biolang"));
    bio_rm_tree(join(dir, "app"));
    bio_rm_tree(join(dir, "build"));
    printf("✔ destroyed build artifacts: %s/.biolang, %s/app\n", dir, dir);
    return 0;
}
