#include "bio.h"
#include <stdlib.h>

/* Block-based arena: allocations never move (realloc would invalidate every
 * outstanding pointer). New blocks are appended when the current one fills.
 *
 * Interpreted mode has a configurable upper limit (default 256 MiB, 0 =
 * unlimited); `-e` / BIO_MEM_LIMIT override it. Compiled products run with
 * no limit unless BIO_MEM_LIMIT is set. */

typedef struct ArenaBlock {
    char *mem;
    size_t used;
    size_t cap;
    struct ArenaBlock *next;
} ArenaBlock;

static ArenaBlock *arena_head = NULL;
static ArenaBlock *arena_cur = NULL;
static size_t arena_total = 0;
static size_t bio_mem_limit = 256u << 20;

void bio_set_mem_limit(size_t bytes) {
    bio_mem_limit = bytes;
}

size_t bio_mem_used(void) {
    return arena_total;
}

static ArenaBlock *arena_new_block(size_t need) {
    size_t cap = arena_cur ? arena_cur->cap * 2 : (64u << 10);
    if (cap < need) cap = need;
    if (bio_mem_limit) {
        if (arena_total >= bio_mem_limit) {
            fprintf(stderr, "memory limit exceeded (limit %zu bytes)\n", bio_mem_limit);
            exit(1);
        }
        size_t room = bio_mem_limit - arena_total;
        if (cap > room) cap = room;
        if (cap < need) {
            fprintf(stderr, "memory limit exceeded (%zu bytes requested, limit %zu)\n",
                    need, bio_mem_limit);
            exit(1);
        }
    }
    ArenaBlock *b = calloc(1, sizeof(ArenaBlock));
    b->mem = malloc(cap);
    if (!b || !b->mem) {
        fprintf(stderr, "out of memory (%zu bytes)\n", cap);
        exit(1);
    }
    b->cap = cap;
    b->used = 0;
    b->next = NULL;
    if (arena_cur) arena_cur->next = b;
    else arena_head = b;
    arena_cur = b;
    return b;
}

void *aalloc(size_t n) {
    n = (n + 7u) & ~7u;
    if (!arena_cur || arena_cur->used + n > arena_cur->cap)
        arena_new_block(n);
    void *p = arena_cur->mem + arena_cur->used;
    arena_cur->used += n;
    arena_total += n;
    return p;
}

char *astrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = aalloc(n);
    memcpy(p, s, n);
    return p;
}
