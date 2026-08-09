/* pack.c — package formats for compiled products.
 *
 * .img  : custom raw image. No compression: a small directory header followed
 *         by the files' bytes written directly (seekable, extremely fast).
 * .zip  : standard zip archive (uses the system `zip`/`unzip` tools).
 *
 * Layout of a .img file:
 *   magic "BIOIMG1" (8 bytes), u32 version, u32 flags, u32 entry-name len,
 *   entry name bytes, u32 file count, then one directory record per file
 *   (u32 name len, name bytes, u64 offset, u64 size), then raw payloads.
 */
#include "bio.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define IMG_MAGIC "BIOIMG1"
#define IMG_VERSION 1u

static void wr_u32(FILE *f, unsigned v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void wr_u64(FILE *f, unsigned long long v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    fwrite(b, 1, 8, f);
}

static unsigned rd_u32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned long long rd_u64(const unsigned char *p) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static int img_name_match(const char *stored, const char *want) {
    if (strcmp(stored, want) == 0) return 1;
    const char *s = strrchr(stored, '/'); s = s ? s + 1 : stored;
    const char *w = strrchr(want, '/');   w = w ? w + 1 : want;
    return strcmp(s, w) == 0;
}

/* Copy src into out (direct write, no compression). */
static int copy_payload(FILE *out, const char *src) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    char buf[BIO_COPY_BUF];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { fclose(in); return -1; }
    fclose(in);
    return 0;
}

int img_create(const char *out, const char *entry, const char **files, int nfiles) {
    FILE *f = fopen(out, "wb");
    if (!f) return -1;
    size_t elen = entry ? strlen(entry) : 0;
    fwrite(IMG_MAGIC, 1, 8, f);
    wr_u32(f, IMG_VERSION);
    wr_u32(f, entry ? 1u : 0u);
    wr_u32(f, (unsigned)elen);
    if (elen) fwrite(entry, 1, elen, f);
    wr_u32(f, (unsigned)nfiles);

    /* Directory records (offsets filled after we know payload positions). */
    unsigned long long *offs = calloc((size_t)nfiles, sizeof *offs);
    unsigned long long *sizes = calloc((size_t)nfiles, sizeof *sizes);
    size_t *nlen = calloc((size_t)nfiles, sizeof *nlen);
    if (!offs || !sizes || !nlen) { fclose(f); return -1; }

    for (int i = 0; i < nfiles; i++) {
        const char *leaf = strrchr(files[i], '/');
        leaf = leaf ? leaf + 1 : files[i];
        nlen[i] = strlen(leaf);
        wr_u32(f, (unsigned)nlen[i]);
        fwrite(leaf, 1, nlen[i], f);
        wr_u64(f, 0);
        wr_u64(f, 0);
    }

    unsigned long long pos = (unsigned long long)ftell(f);
    for (int i = 0; i < nfiles; i++) {
        FILE *in = fopen(files[i], "rb");
        if (!in) { fclose(f); return -1; }
        fseek(in, 0, SEEK_END);
        sizes[i] = (unsigned long long)ftell(in);
        fclose(in);
        offs[i] = pos;
        pos += sizes[i];
    }

    /* Rewrite directory with real offsets/sizes. */
    fseek(f, 8 + 12 + (long)elen + 4, SEEK_SET);
    for (int i = 0; i < nfiles; i++) {
        const char *leaf = strrchr(files[i], '/');
        leaf = leaf ? leaf + 1 : files[i];
        wr_u32(f, (unsigned)nlen[i]);
        fwrite(leaf, 1, nlen[i], f);
        wr_u64(f, offs[i]);
        wr_u64(f, sizes[i]);
    }

    /* Append payloads directly (zero compression). */
    fseek(f, 0, SEEK_END);
    for (int i = 0; i < nfiles; i++)
        if (copy_payload(f, files[i]) != 0) { fclose(f); return -1; }

    fclose(f);
    free(offs); free(sizes); free(nlen);
    return 0;
}

static int img_parse(FILE *f, char **entry, char ***names, unsigned long long **offs,
                     unsigned long long **sizes, int *nfiles) {
    unsigned char hdr[32];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, IMG_MAGIC, 8) != 0) return -1;
    unsigned char ver[12];
    if (fread(ver, 1, 12, f) != 12) return -1;
    (void)ver;
    unsigned elen = rd_u32(ver + 8);
    if (elen) {
        *entry = malloc(elen + 1);
        if (fread(*entry, 1, elen, f) != elen) return -1;
        (*entry)[elen] = 0;
    }
    unsigned char cntb[4];
    if (fread(cntb, 1, 4, f) != 4) return -1;
    int n = (int)rd_u32(cntb);
    *nfiles = n;
    *names = calloc((size_t)n, sizeof(char *));
    *offs = calloc((size_t)n, sizeof(unsigned long long));
    *sizes = calloc((size_t)n, sizeof(unsigned long long));
    for (int i = 0; i < n; i++) {
        unsigned char nb[4];
        if (fread(nb, 1, 4, f) != 4) return -1;
        unsigned nl = rd_u32(nb);
        (*names)[i] = malloc(nl + 1);
        if (fread((*names)[i], 1, nl, f) != nl) return -1;
        (*names)[i][nl] = 0;
        unsigned char rec[16];
        if (fread(rec, 1, 16, f) != 16) return -1;
        (*offs)[i] = rd_u64(rec);
        (*sizes)[i] = rd_u64(rec + 8);
    }
    return 0;
}

int img_unpack(const char *path, const char *dir) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *entry = NULL;
    char **names = NULL;
    unsigned long long *offs = NULL, *sizes = NULL;
    int n = 0;
    if (img_parse(f, &entry, &names, &offs, &sizes, &n) != 0) { fclose(f); return -1; }
    (void)entry;
    bio_mkdir_p(dir);
    char out[BIO_PATH_MAX];
    char copy[BIO_COPY_BUF];
    for (int i = 0; i < n; i++) {
        const char *base = strrchr(names[i], '/');
        const char *leaf = base ? base + 1 : names[i];
        snprintf(out, sizeof out, "%s/%s", dir, leaf);
        FILE *o = fopen(out, "wb");
        if (!o) { fclose(f); return -1; }
        if (fseek(f, (long)offs[i], SEEK_SET) != 0) { fclose(o); fclose(f); return -1; }
        unsigned long long left = sizes[i];
        while (left > 0) {
            size_t take = left > sizeof copy ? sizeof copy : (size_t)left;
            if (fread(copy, 1, take, f) != take) { fclose(o); fclose(f); return -1; }
            fwrite(copy, 1, take, o);
            left -= take;
        }
        fclose(o);
    }
    fclose(f);
    return 0;
}

/* Extract the entry payload to out_path (used by `bio run pkg.img`). */
int img_entry(const char *path, char *out_path, size_t out_cap) {
    (void)out_cap;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *entry = NULL;
    char **names = NULL;
    unsigned long long *offs = NULL, *sizes = NULL;
    int n = 0;
    if (img_parse(f, &entry, &names, &offs, &sizes, &n) != 0) { fclose(f); return -1; }
    if (!entry) { fclose(f); return -1; }
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (img_name_match(names[i], entry)) { idx = i; break; }
    if (idx < 0) { fclose(f); return -1; }
    FILE *o = fopen(out_path, "wb");
    if (!o) { fclose(f); return -1; }
    fseek(f, (long)offs[idx], SEEK_SET);
    char copy[BIO_COPY_BUF];
    unsigned long long left = sizes[idx];
    while (left > 0) {
        size_t take = left > sizeof copy ? sizeof copy : (size_t)left;
        if (fread(copy, 1, take, f) != take) { fclose(o); fclose(f); return -1; }
        fwrite(copy, 1, take, o);
        left -= take;
    }
    fclose(o);
    fclose(f);
#if defined(_WIN32)
    _chmod(out_path, _S_IREAD | _S_IWRITE);
#else
    chmod(out_path, 0755);
#endif
    return 0;
}

int zip_create(const char *out, const char **files, int nfiles) {
    /* Flat zip: `zip -j -q out file...` (junk paths, no compression tuning). */
    const char **av = aalloc(sizeof(char *) * (size_t)(4 + nfiles + 1));
    int n = 0;
    av[n++] = "zip";
    av[n++] = "-j";
    av[n++] = "-q";
    av[n++] = out;
    for (int i = 0; i < nfiles; i++) av[n++] = files[i];
    av[n] = NULL;
    return bio_run(av);
}

int zip_unpack(const char *path, const char *dir) {
    bio_mkdir_p(dir);
    const char *av[] = { "unzip", "-q", "-o", path, "-d", dir, NULL };
    return bio_run(av);
}
