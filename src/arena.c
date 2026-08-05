#include "bio.h"

/* 简易内存池 */
#define ARENA_SIZE (8u << 20)
static char arena[ARENA_SIZE];
static size_t arena_used = 0;

void *aalloc(size_t n) {
    n = (n + 7u) & ~7u;
    if (arena_used + n > ARENA_SIZE) {
        fprintf(stderr, "arena exhausted\n");
        exit(1);
    }
    void *p = arena + arena_used;
    arena_used += n;
    return p;
}

char *astrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = aalloc(n);
    memcpy(p, s, n);
    return p;
}
