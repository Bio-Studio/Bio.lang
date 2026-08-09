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

/* Phone-booth arena: a small private block list that is reused by one method
 * call after another. reset() clears the previously-used bytes and puts the
 * allocator back at the first block, so a call is zero-allocation and never
 * fragments the global arena. */
typedef struct BoothBlock {
    char *mem;
    size_t used;
    size_t cap;
    struct BoothBlock *next;
} BoothBlock;

static ArenaBlock *arena_head = NULL;
static ArenaBlock *arena_cur = NULL;
static size_t arena_total = 0;
static size_t bio_mem_limit = 256u << 20;
static BoothArena *booth_active = NULL;   /* booth currently used by aalloc */

void bio_set_mem_limit(size_t bytes) {
    bio_mem_limit = bytes;
}

size_t bio_mem_used(void) {
    return arena_total;
}

static BoothBlock *booth_new_block(BoothArena *b, size_t need) {
    size_t cap = b->cur ? b->cur->cap * 2 : (64u << 10);
    if (cap < need) cap = need;
    BoothBlock *blk = calloc(1, sizeof *blk);
    if (!blk) {
        fprintf(stderr, "out of memory (phone booth block header)\n");
        exit(1);
    }
    blk->mem = malloc(cap);
    if (!blk->mem) {
        free(blk);
        fprintf(stderr, "out of memory (phone booth block, %zu bytes)\n", cap);
        exit(1);
    }
    blk->cap = cap;
    blk->used = 0;
    blk->next = NULL;
    if (b->cur) b->cur->next = blk;
    else b->head = blk;
    b->cur = blk;
    return blk;
}

static void *booth_alloc(BoothArena *b, size_t n) {
    n = (n + 7u) & ~7u;
    if (!b->cur || b->cur->used + n > b->cur->cap)
        booth_new_block(b, n);
    void *p = b->cur->mem + b->cur->used;
    b->cur->used += n;
    return p;
}

BoothArena *booth_get(BoothArena **list, const Method *m) {
    for (BoothArena *b = *list; b; b = b->next)
        if (b->method == m) return b;
    BoothArena *b = calloc(1, sizeof *b);
    if (!b) {
        fprintf(stderr, "out of memory (phone booth)\n");
        exit(1);
    }
    b->method = m;
    b->next = *list;
    *list = b;
    return b;
}

void booth_reset(BoothArena *b) {
    for (BoothBlock *blk = b->head; blk; blk = blk->next) {
        if (blk->used) memset(blk->mem, 0, blk->used);
        blk->used = 0;
    }
    b->cur = b->head;
}

BoothArena *booth_current(void) {
    return booth_active;
}

void booth_set_current(BoothArena *b) {
    booth_active = b;
}

void booth_free_list(BoothArena *list) {
    while (list) {
        BoothArena *next = list->next;
        BoothBlock *blk = list->head;
        while (blk) {
            BoothBlock *bn = blk->next;
            free(blk->mem);
            free(blk);
            blk = bn;
        }
        if (booth_active == list) booth_active = NULL;
        free(list);
        list = next;
    }
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
    if (booth_active)
        return booth_alloc(booth_active, n);
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
