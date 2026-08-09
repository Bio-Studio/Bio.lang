/* platform.h — cross-platform helpers (no third-party dependencies).
 *
 *   time     : monotonic seconds + millisecond sleep
 *   process  : spawn a program by argv (no shell → no quoting bugs)
 *   filesys  : recursive mkdir / copy / remove, single-level directory walk
 *   env      : HOME with USERPROFILE fallback
 *   stdin    : binary mode (Windows CRLF safety)
 *
 * POSIX gets native calls; Windows (via _WIN32) gets equivalent Win32 calls.
 */
#ifndef BIO_PLATFORM_H
#define BIO_PLATFORM_H

/* Monotonic wall-clock seconds (unaffected by wall time changes). */
double bio_now_sec(void);

/* Sleep for ms milliseconds. */
void bio_sleep_ms(double ms);

/* Run a program by argv (argv[0] = program, NULL-terminated). No shell is
 * involved. Returns the child's exit code (0 = success, -1 = spawn failure). */
int bio_run(const char **argv);

/* Create a directory and any missing parents (like mkdir -p). Returns 0 on
 * success, including when it already exists. */
int bio_mkdir_p(const char *path);

/* Recursively copy the CONTENTS of src into dst (like `cp -r src/. dst/`). */
int bio_copy_tree(const char *src, const char *dst);

/* Copy one file (binary-safe). */
int bio_copy_file(const char *src, const char *dst);

/* Recursively remove a directory tree (like `rm -rf`); tolerates missing. */
int bio_rm_tree(const char *path);

/* Walk one directory level, invoking fn(path, is_dir, ud) for each entry
 * (dotfiles skipped). Does NOT recurse — the callback decides. */
typedef void (*BioWalkFn)(const char *path, int is_dir, void *ud);
int bio_walk_dir(const char *dir, BioWalkFn fn, void *ud);

/* HOME, or USERPROFILE on Windows. */
const char *bio_home(void);

/* Switch stdin to binary mode on Windows (no-op elsewhere). */
void bio_stdin_binary(void);

/* Dynamic library loading (dlopen/dlsym/dlclose on POSIX; LoadLibrary/
 * GetProcAddress on Windows). Returns NULL on failure; bio_dlerror() gives a
 * human-readable message (NULL when the last call succeeded). */
void *bio_dlopen(const char *path);
void *bio_dlsym(void *handle, const char *name);
void bio_dlclose(void *handle);
const char *bio_dlerror(void);

#endif /* BIO_PLATFORM_H */
