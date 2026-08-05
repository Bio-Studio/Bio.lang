/* platform.c — cross-platform helpers. See platform.h. */
#include "bio.h"
#include "platform.h"
#include <errno.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#endif

/* ---------- time ---------- */

double bio_now_sec(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

void bio_sleep_ms(double ms) {
    if (ms < 0) ms = 0;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - ts.tv_sec * 1000.0) * 1e6);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
#endif
}

/* ---------- process (no shell) ---------- */

int bio_run(const char **argv) {
#if defined(_WIN32)
    /* Build a quoted command line (CreateProcessA does not take argv). */
    size_t len = 0;
    for (int i = 0; argv[i]; i++) len += strlen(argv[i]) + 3;
    char *cmdline = aalloc(len + 1);
    cmdline[0] = 0;
    for (int i = 0; argv[i]; i++) {
        if (i) strcat(cmdline, " ");
        strcat(cmdline, "\"");
        strcat(cmdline, argv[i]);
        strcat(cmdline, "\"");
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);   /* replaces the child */
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

/* ---------- filesystem ---------- */

static int mkdir_one(const char *p) {
#if defined(_WIN32)
    return _mkdir(p);
#else
    return mkdir(p, 0755);
#endif
}

static int rmdir_one(const char *p) {
#if defined(_WIN32)
    return _rmdir(p);
#else
    return rmdir(p);
#endif
}

int bio_mkdir_p(const char *path) {
    char *p = astrdup(path);
    for (char *q = p + 1; *q; q++) {
        if (*q == '/' || *q == '\\') {
            char save = *q; *q = 0;
            if (mkdir_one(p) != 0 && errno != EEXIST) return -1;
            *q = save;
        }
    }
    if (mkdir_one(p) != 0 && errno != EEXIST) return -1;
    return 0;
}

int bio_walk_dir(const char *dir, BioWalkFn fn, void *ud) {
#if defined(_WIN32)
    char pat[1024];
    snprintf(pat, sizeof pat, "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, fd.cFileName);
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        fn(p, is_dir, ud);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        fn(p, S_ISDIR(st.st_mode), ud);
    }
    closedir(d);
    return 0;
#endif
}

/* Copy one file with stdio. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

typedef struct { const char *src; const char *dst; } CopyCtx;

static void copy_cb(const char *path, int is_dir, void *ud) {
    CopyCtx *c = ud;
    const char *rel = path + strlen(c->src);   /* e.g. "/sub/file" */
    char out[1024];
    snprintf(out, sizeof out, "%s%s", c->dst, rel);
    if (is_dir) {
        mkdir_one(out);
        bio_walk_dir(path, copy_cb, ud);
    } else {
        copy_file(path, out);
    }
}

int bio_copy_tree(const char *src, const char *dst) {
    if (mkdir_one(dst) != 0 && errno != EEXIST) return -1;
    CopyCtx c = { src, dst };
    bio_walk_dir(src, copy_cb, &c);
    return 0;
}

static void rm_cb(const char *path, int is_dir, void *ud) {
    (void)ud;
    if (is_dir) {
        bio_walk_dir(path, rm_cb, ud);
        rmdir_one(path);   /* now empty */
    } else {
        remove(path);
    }
}

int bio_rm_tree(const char *path) {
    bio_walk_dir(path, rm_cb, NULL);
    remove(path);   /* the top directory itself */
    return 0;
}

/* ---------- env ---------- */

const char *bio_home(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    return getenv("USERPROFILE");
}

/* ---------- stdin ---------- */

void bio_stdin_binary(void) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#else
    /* no-op */
#endif
}
