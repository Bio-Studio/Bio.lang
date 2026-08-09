/* pack.c — package formats for compiled products.
 *
 * .img  : custom raw image. No compression: a small directory header followed
 *         by the files' bytes written directly (seekable, extremely fast).
 *         Version 2 stores each file's permission bits (mode) so unpacked
 *         executables keep their executable bit.
 * .zip  : standard zip archive, written/read natively with the STORE method
 *         (no compression, no external zip/unzip tools — works everywhere).
 *         Unpacking falls back to the system `unzip` when present so legacy
 *         deflate archives still open.
 *
 * Layout of a .img file (v2):
 *   magic "BIOIMG1" (8 bytes), u32 version, u32 flags, u32 entry-name len,
 *   entry name bytes, u32 file count, then one directory record per file
 *   (u32 name len, name bytes, u32 mode, u64 offset, u64 size), then raw
 *   payloads. v1 files (no mode field) are still readable.
 */
#include "bio.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define IMG_MAGIC "BIOIMG1"
#define IMG_VERSION 2u

/* ------------------------------------------------------------------ */
/* little-endian helpers                                               */
/* ------------------------------------------------------------------ */

static void wr_u32(FILE *f, unsigned v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void wr_u16(FILE *f, unsigned v) {
    unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    fwrite(b, 1, 2, f);
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

static unsigned rd_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
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

/* Apply a permission mode to a file (best effort on Windows). */
static void apply_mode(const char *path, unsigned mode) {
#if defined(_WIN32)
    int a = _S_IREAD;
    if (mode & 0200) a |= _S_IWRITE;
    _chmod(path, a);
#else
    chmod(path, mode ? mode : 0644);
#endif
}

/* ------------------------------------------------------------------ */
/* .img                                                                 */
/* ------------------------------------------------------------------ */

static unsigned file_mode(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0644;
#if defined(_WIN32)
    return 0644;                       /* no meaningful bits on Windows */
#else
    return (unsigned)(st.st_mode & 07777);
#endif
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
    unsigned *modes = calloc((size_t)nfiles, sizeof *modes);
    size_t *nlen = calloc((size_t)nfiles, sizeof *nlen);
    if (!offs || !sizes || !modes || !nlen) { fclose(f); return -1; }

    for (int i = 0; i < nfiles; i++) {
        const char *leaf = strrchr(files[i], '/');
        leaf = leaf ? leaf + 1 : files[i];
        nlen[i] = strlen(leaf);
        modes[i] = file_mode(files[i]);
        wr_u32(f, (unsigned)nlen[i]);
        fwrite(leaf, 1, nlen[i], f);
        wr_u32(f, modes[i]);
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
        wr_u32(f, modes[i]);
        wr_u64(f, offs[i]);
        wr_u64(f, sizes[i]);
    }

    /* Append payloads directly (zero compression). */
    fseek(f, 0, SEEK_END);
    for (int i = 0; i < nfiles; i++)
        if (copy_payload(f, files[i]) != 0) { fclose(f); return -1; }

    fclose(f);
    free(offs); free(sizes); free(modes); free(nlen);
    return 0;
}

static int img_parse(FILE *f, char **entry, char ***names, unsigned **modes,
                     unsigned long long **offs, unsigned long long **sizes,
                     int *nfiles) {
    unsigned char hdr[32];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, IMG_MAGIC, 8) != 0) return -1;
    unsigned char ver[12];
    if (fread(ver, 1, 12, f) != 12) return -1;
    unsigned version = rd_u32(ver);
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
    *modes = calloc((size_t)n, sizeof(unsigned));
    *offs = calloc((size_t)n, sizeof(unsigned long long));
    *sizes = calloc((size_t)n, sizeof(unsigned long long));
    for (int i = 0; i < n; i++) {
        unsigned char nb[4];
        if (fread(nb, 1, 4, f) != 4) return -1;
        unsigned nl = rd_u32(nb);
        (*names)[i] = malloc(nl + 1);
        if (fread((*names)[i], 1, nl, f) != nl) return -1;
        (*names)[i][nl] = 0;
        unsigned char rec[20];
        unsigned char *recp = rec;
        if (version >= 2) {
            if (fread(rec, 1, 20, f) != 20) return -1;
            (*modes)[i] = rd_u32(rec);
            recp = rec + 4;
        } else {
            if (fread(rec, 1, 16, f) != 16) return -1;
            (*modes)[i] = 0;                       /* v1: unknown */
        }
        (*offs)[i] = rd_u64(recp);
        (*sizes)[i] = rd_u64(recp + 8);
    }
    return 0;
}

int img_unpack(const char *path, const char *dir) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *entry = NULL;
    char **names = NULL;
    unsigned *modes = NULL;
    unsigned long long *offs = NULL, *sizes = NULL;
    int n = 0;
    if (img_parse(f, &entry, &names, &modes, &offs, &sizes, &n) != 0) {
        fclose(f);
        return -1;
    }
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
        apply_mode(out, modes[i]);
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
    unsigned *modes = NULL;
    unsigned long long *offs = NULL, *sizes = NULL;
    int n = 0;
    if (img_parse(f, &entry, &names, &modes, &offs, &sizes, &n) != 0) {
        fclose(f);
        return -1;
    }
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
    apply_mode(out_path, modes[idx] ? modes[idx] : 0755);
    return 0;
}

/* ------------------------------------------------------------------ */
/* .zip — native STORE implementation (no external tools)               */
/* ------------------------------------------------------------------ */

#define ZIP_LFH_SIG   0x04034b50u
#define ZIP_CD_SIG    0x02014b50u
#define ZIP_EOCD_SIG  0x06054b50u

static unsigned crc_table[256];
static int crc_table_ready = 0;

static void crc_table_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

static unsigned crc32_buf(const void *data, size_t len, unsigned crc) {
    if (!crc_table_ready) crc_table_init();
    const unsigned char *p = (const unsigned char *)data;
    crc = ~crc;
    while (len--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static unsigned zip_dos_time(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) return 0;
    return (unsigned)(tm->tm_hour << 11) | (unsigned)(tm->tm_min << 5) |
           (unsigned)(tm->tm_sec / 2);
}

static unsigned zip_dos_date(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) return 0;
    return (unsigned)((tm->tm_year - 80) << 9) | (unsigned)((tm->tm_mon + 1) << 5) |
           (unsigned)tm->tm_mday;
}

int zip_create(const char *out, const char **files, int nfiles) {
    FILE *f = fopen(out, "wb");
    if (!f) return -1;
    crc_table_init();
    unsigned dtime = zip_dos_time(), ddate = zip_dos_date();

    unsigned long long *lhoff = calloc((size_t)nfiles, sizeof *lhoff);
    unsigned long long *sizes = calloc((size_t)nfiles, sizeof *sizes);
    unsigned *crcs = calloc((size_t)nfiles, sizeof *crcs);
    unsigned *modes = calloc((size_t)nfiles, sizeof *modes);
    char **leaves = calloc((size_t)nfiles, sizeof(char *));
    if (!lhoff || !sizes || !crcs || !modes || !leaves) { fclose(f); return -1; }

    /* Local file headers + raw payloads. */
    for (int i = 0; i < nfiles; i++) {
        const char *leaf = strrchr(files[i], '/');
        leaf = leaf ? leaf + 1 : files[i];
        leaves[i] = (char *)leaf;
        modes[i] = file_mode(files[i]);
        FILE *in = fopen(files[i], "rb");
        if (!in) { fclose(f); return -1; }
        fseek(in, 0, SEEK_END);
        long size = ftell(in);
        fseek(in, 0, SEEK_SET);
        lhoff[i] = (unsigned long long)ftell(f);
        sizes[i] = (unsigned long long)size;

        unsigned crc = 0;
        char buf[BIO_COPY_BUF];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) crc = crc32_buf(buf, n, crc);
        fclose(in);
        crcs[i] = crc;

        unsigned namelen = (unsigned)strlen(leaf);
        wr_u32(f, ZIP_LFH_SIG);
        wr_u16(f, 20u);                    /* version needed */
        wr_u16(f, 0u);                     /* flags */
        wr_u16(f, 0u);                     /* method: STORE */
        wr_u16(f, dtime);
        wr_u16(f, ddate);
        wr_u32(f, crcs[i]);
        wr_u32(f, (unsigned)sizes[i]);     /* compressed size = size */
        wr_u32(f, (unsigned)sizes[i]);     /* uncompressed size */
        wr_u16(f, namelen);
        wr_u16(f, 0u);                     /* extra len */
        fwrite(leaf, 1, namelen, f);

        in = fopen(files[i], "rb");
        if (!in) { fclose(f); return -1; }
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, n, f) != n) { fclose(in); fclose(f); return -1; }
        fclose(in);
    }

    /* Central directory. */
    unsigned long long cd_start = (unsigned long long)ftell(f);
    for (int i = 0; i < nfiles; i++) {
        unsigned namelen = (unsigned)strlen(leaves[i]);
        wr_u32(f, ZIP_CD_SIG);
        wr_u16(f, 20u);                    /* version made by (unix-ish) */
        wr_u16(f, 20u);                    /* version needed */
        wr_u16(f, 0u);                     /* flags */
        wr_u16(f, 0u);                     /* method: STORE */
        wr_u16(f, dtime);
        wr_u16(f, ddate);
        wr_u32(f, crcs[i]);
        wr_u32(f, (unsigned)sizes[i]);
        wr_u32(f, (unsigned)sizes[i]);
        wr_u16(f, namelen);
        wr_u16(f, 0u);                     /* extra len */
        wr_u16(f, 0u);                     /* comment len */
        wr_u16(f, 0u);                     /* disk number */
        wr_u16(f, 0u);                     /* internal attrs */
        wr_u32(f, modes[i] << 16);         /* external attrs (unix mode) */
        wr_u32(f, (unsigned)lhoff[i]);     /* local header offset */
        fwrite(leaves[i], 1, namelen, f);
    }
    unsigned long long cd_size = (unsigned long long)ftell(f) - cd_start;

    /* End of central directory. */
    wr_u32(f, ZIP_EOCD_SIG);
    wr_u16(f, 0u);                         /* disk number */
    wr_u16(f, 0u);                         /* cd start disk */
    wr_u16(f, (unsigned)nfiles);
    wr_u16(f, (unsigned)nfiles);
    wr_u32(f, (unsigned)cd_size);
    wr_u32(f, (unsigned)cd_start);
    wr_u16(f, 0u);                         /* comment len */

    fclose(f);
    free(lhoff); free(sizes); free(crcs); free(modes); free(leaves);
    return 0;
}

/* Read 4 bytes at offset off (little-endian); returns 0 on EOF. */
static unsigned read_u32_at(FILE *f, unsigned long long off) {
    unsigned char b[4];
    if (fseek(f, (long)off, SEEK_SET) != 0) return 0;
    if (fread(b, 1, 4, f) != 4) return 0;
    return rd_u32(b);
}

static int zip_extract_store(FILE *f, const char *dir, const char *name,
                             unsigned long long lhoff, unsigned long long csize,
                             unsigned ext_attrs) {
    /* Skip local header: 30 bytes + name len + extra len. */
    unsigned char lh[32];
    if (fseek(f, (long)lhoff, SEEK_SET) != 0) return -1;
    if (fread(lh, 1, 30, f) != 30) return -1;
    unsigned namelen = rd_u16(lh + 26);
    unsigned extralen = rd_u16(lh + 28);
    if (fseek(f, (long)(lhoff + 30 + namelen + extralen), SEEK_SET) != 0) return -1;

    /* Entry name may contain '/'; create subdirectories. */
    char out[BIO_PATH_MAX];
    snprintf(out, sizeof out, "%s/%s", dir, name);
    for (char *p = out; *p; p++)
        if (*p == '\\') *p = '/';
    char *slash = strrchr(out, '/');
    if (slash && slash != out) {
        *slash = 0;
        bio_mkdir_p(out);
        *slash = '/';
    }

    FILE *o = fopen(out, "wb");
    if (!o) return -1;
    char buf[BIO_COPY_BUF];
    unsigned long long left = csize;
    while (left > 0) {
        size_t take = left > sizeof buf ? sizeof buf : (size_t)left;
        if (fread(buf, 1, take, f) != take) { fclose(o); return -1; }
        if (fwrite(buf, 1, take, o) != take) { fclose(o); return -1; }
        left -= take;
    }
    fclose(o);
    apply_mode(out, (ext_attrs >> 16) & 07777);
    return 0;
}

int zip_unpack(const char *path, const char *dir) {
    /* Prefer the system unzip when present (handles deflate archives). */
    bio_mkdir_p(dir);
    const char *av[] = { "unzip", "-q", "-o", path, "-d", dir, NULL };
    if (bio_run(av) == 0) return 0;

    /* Native fallback: STORE-only reader. */
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long fsize = ftell(f);
    if (fsize < 22) { fclose(f); return -1; }

    /* Locate EOCD (scan backwards up to 64 KiB). */
    long scan = fsize - 22;
    long eocd = -1;
    for (long off = scan; off >= 0 && off > scan - 65536; off--) {
        if (read_u32_at(f, (unsigned long long)off) == ZIP_EOCD_SIG) { eocd = off; break; }
    }
    if (eocd < 0) { fclose(f); return -1; }

    unsigned char e[22];
    if (fseek(f, eocd, SEEK_SET) != 0 || fread(e, 1, 22, f) != 22) { fclose(f); return -1; }
    unsigned nentries = rd_u16(e + 10);
    unsigned long long cd_off = rd_u32(e + 16);

    for (unsigned i = 0; i < nentries; i++) {
        unsigned char cd[46];
        (void)i;
        if (fseek(f, (long)cd_off, SEEK_SET) != 0) { fclose(f); return -1; }
        /* Central entries are variable length; walk sequentially. */
        if (fread(cd, 1, 46, f) != 46) { fclose(f); return -1; }
        if (rd_u32(cd) != ZIP_CD_SIG) { fclose(f); return -1; }
        unsigned method = rd_u16(cd + 10);
        unsigned namelen = rd_u16(cd + 28);
        unsigned extralen = rd_u16(cd + 30);
        unsigned commentlen = rd_u16(cd + 32);
        unsigned ext_attrs = rd_u32(cd + 38);
        unsigned long long lhoff = rd_u32(cd + 42);
        unsigned long long csize = rd_u32(cd + 20);

        char *name = malloc((size_t)namelen + 1);
        if (!name) { fclose(f); return -1; }
        if (fread(name, 1, namelen, f) != namelen) { free(name); fclose(f); return -1; }
        name[namelen] = 0;
        /* Skip this entry's extra + comment to reach the next CD record. */
        if (extralen || commentlen) {
            if (fseek(f, (long)(extralen + commentlen), SEEK_CUR) != 0) {
                free(name); fclose(f); return -1;
            }
        }

        if (method != 0) {
            fprintf(stderr, "zip: entry %s uses compression method %u (needs unzip)\n",
                    name, method);
            free(name);
            fclose(f);
            return -1;
        }
        if (zip_extract_store(f, dir, name, lhoff, csize, ext_attrs) != 0) {
            free(name);
            fclose(f);
            return -1;
        }
        free(name);
        cd_off += 46 + namelen + extralen + commentlen;
    }
    fclose(f);
    return 0;
}
