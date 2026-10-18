/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * An APK is a ZIP file, and this reads only as much of the format as an APK
 * uses: the central directory (ZIP64 included, which large games need),
 * stored entries, and deflated ones.
 *
 * The file is someone else's, so every offset and length is checked against the
 * file's size before it is used, with the arithmetic arranged so that the check
 * itself cannot overflow.
 *
 * How the bytes are reached is a choice made here: a small archive is mapped
 * whole, a big one is read with pread(2) and no address space at all, and a
 * stored entry of one of those gets a mapping of its own when a caller wants a
 * pointer rather than bytes. Nothing above this notices which happened.
 */
#include "husk-tl-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

void tl_log_line(const char *fmt, ...);          /* husk-tl-load.c */

#define SIG_LOCAL       0x04034b50u
#define SIG_CENTRAL     0x02014b50u
#define SIG_END         0x06054b50u
#define SIG_END64       0x06064b50u
#define SIG_END64_LOC   0x07064b50u

/* An APK with more files than this is not an APK. */
#define MAX_ENTRIES     (1u << 20)

/* How much of an unmapped entry's compressed bytes are read from the file at a
 * time. Small enough that a stream's cost in memory does not depend on the
 * entry, large enough that a gigabyte of deflate is a few thousand preads. */
#define TL_ZIP_IN      (64 << 10)

static void fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* how many bytes of the mapping a read has put under the OS's eyes (see tl_zip_trim) */
static void zip_saw(const tl_zip *z, uint64_t n);

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* [off, off+len) lies inside the file. */
static bool in_file(const tl_zip *z, uint64_t off, uint64_t len)
{
    return off <= z->size && len <= z->size - off;
}

/*
 * Read [off, off+n) out of the archive: from the mapping when there is one, and
 * with pread(2) when there is not. Both are the same file, byte for byte, so
 * nothing below has to know which it was.
 */
static bool zip_read(const tl_zip *z, void *dst, size_t n, uint64_t off)
{
    if (!in_file(z, off, n)) {
        return false;
    }
    if (z->map) {
        memcpy(dst, z->map + off, n);
        return true;
    }
    uint8_t *p = dst;
    while (n) {
        ssize_t r = pread(z->fd, p, n, (off_t)off);
        if (r <= 0) {
            return false;
        }
        p += r;
        off += (uint64_t)r;
        n -= (size_t)r;
    }
    return true;
}

/*
 * How big a file may be and still be worth mapping whole. An APK's mapping is
 * address space held for as long as the game runs, and a phone without the
 * extended-address-space entitlement has about four gigabytes of it in total --
 * a game's own APK can be half of that. A big archive is read with pread(2)
 * instead, which costs a system call per read and no address space at all;
 * stored assets, which want a pointer rather than bytes, get a mapping of their
 * own, on their own, once something asks for one.
 *
 * TL_ZIP_MAP_MAX_MB sets the size (0: never map; a Mac can afford to always).
 */
#define TL_ZIP_MAP_MAX ((size_t)64 << 20)

static size_t zip_map_max(void)
{
    static size_t cached = (size_t)-2;
    size_t v = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (v == (size_t)-2) {
        const char *s = getenv("TL_ZIP_MAP_MAX_MB");
        v = s ? (size_t)strtol(s, NULL, 10) << 20 : TL_ZIP_MAP_MAX;
        __atomic_store_n(&cached, v, __ATOMIC_RELAXED);
    }
    return v;
}

/* The end-of-central-directory record: the last thing in the file, followed
 * only by a comment of up to 64 KiB. Searched for from the end. */
static bool find_end(const tl_zip *z, uint64_t *at)
{
    if (z->size < 22) {
        return false;
    }
    uint64_t lowest = z->size > 22 + 0xFFFF ? z->size - 22 - 0xFFFF : 0;
    /* a windowed read when there is no mapping: it is a few pages of file either way */
    uint8_t *window = NULL;
    const uint8_t *base = z->map;
    uint64_t bias = 0;
    if (!base) {
        size_t want = (size_t)(z->size - lowest);
        window = malloc(want);
        if (!window || !zip_read(z, window, want, lowest)) {
            free(window);
            return false;
        }
        base = window;
        bias = lowest;
    }
    bool found = false;
    for (uint64_t i = z->size - 22 + 1; i-- > lowest;) {
        const uint8_t *p = base + (i - bias);
        if (rd32(p) == SIG_END && i + 22 + rd16(p + 20) <= z->size) {
            *at = i;
            found = true;
            break;
        }
    }
    free(window);
    return found;
}

/* Sizes and offsets too large for 32 bits are 0xFFFFFFFF in the fixed record
 * and the real value is in the ZIP64 extra field, in a fixed order, present
 * only for the fields that overflowed. */
static bool apply_zip64_extra(const uint8_t *extra, uint16_t len, tl_zip_entry *e,
                              bool need_usize, bool need_csize, bool need_offset)
{
    uint16_t pos = 0;
    while ((uint32_t)pos + 4 <= len) {
        uint16_t id = rd16(extra + pos);
        uint16_t size = rd16(extra + pos + 2);
        if ((uint32_t)pos + 4 + size > len) {
            return false;
        }
        if (id == 0x0001) {
            const uint8_t *f = extra + pos + 4;
            uint16_t used = 0;
            if (need_usize) {
                if (used + 8 > size) return false;
                e->usize = rd64(f + used); used += 8;
            }
            if (need_csize) {
                if (used + 8 > size) return false;
                e->csize = rd64(f + used); used += 8;
            }
            if (need_offset) {
                if (used + 8 > size) return false;
                e->local_offset = rd64(f + used);
            }
            return true;
        }
        pos = (uint16_t)(pos + 4 + size);
    }
    return !(need_usize || need_csize || need_offset);
}

bool tl_zip_open(tl_zip *z, const char *path, char *err, size_t errlen)
{
    memset(z, 0, sizeof(*z));
    z->fd = -1;

    z->fd = open(path, O_RDONLY);
    if (z->fd < 0) {
        fail(err, errlen, "cannot open: %s", strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(z->fd, &st) != 0 || st.st_size <= 0) {
        fail(err, errlen, "empty or unreadable file");
        tl_zip_close(z);
        return false;
    }
    z->size = (size_t)st.st_size;
    /* The mapping is a convenience, not a requirement: it is address space held
     * for the whole session, and a phone has only so much of it (see zip_map_max). */
    if (z->size <= zip_map_max()) {
        void *map = mmap(NULL, z->size, PROT_READ, MAP_PRIVATE, z->fd, 0);
        if (map != MAP_FAILED) {
            z->map = map;
        } else {
            /* Refused outright -- which is what happens to a big file once the
             * address space is spoken for. Reading it costs system calls and no
             * address space, so the game opens either way. */
            tl_log_line("zip: %s: %zu MiB would not map (%s); reading it instead",
                        path, z->size >> 20, strerror(errno));
        }
    } else {
        tl_log_line("zip: %s: %zu MiB stays unmapped -- the address space is worth more",
                    path, z->size >> 20);
    }

    uint64_t end;
    if (!find_end(z, &end)) {
        fail(err, errlen, "not a ZIP file (no end-of-directory record)");
        tl_zip_close(z);
        return false;
    }
    uint8_t e[22];
    if (!zip_read(z, e, sizeof(e), end)) {
        fail(err, errlen, "damaged end-of-directory record");
        tl_zip_close(z);
        return false;
    }
    uint64_t count = rd16(e + 10);
    uint64_t cd_size = rd32(e + 12);
    uint64_t cd_off = rd32(e + 16);

    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        uint8_t loc[20], rec64[56];
        if (end < 20 || !zip_read(z, loc, sizeof(loc), end - 20) || rd32(loc) != SIG_END64_LOC) {
            fail(err, errlen, "ZIP64 archive without a ZIP64 locator");
            tl_zip_close(z);
            return false;
        }
        uint64_t rec = rd64(loc + 8);
        if (!in_file(z, rec, sizeof(rec64)) || !zip_read(z, rec64, sizeof(rec64), rec)
            || rd32(rec64) != SIG_END64) {
            fail(err, errlen, "damaged ZIP64 end-of-directory record");
            tl_zip_close(z);
            return false;
        }
        count = rd64(rec64 + 32);
        cd_size = rd64(rec64 + 40);
        cd_off = rd64(rec64 + 48);
    }

    if (!in_file(z, cd_off, cd_size)) {
        fail(err, errlen, "central directory lies outside the file");
        tl_zip_close(z);
        return false;
    }
    if (count > MAX_ENTRIES || count > cd_size / 46) {
        fail(err, errlen, "implausible entry count (%llu)", (unsigned long long)count);
        tl_zip_close(z);
        return false;
    }

    z->entries = calloc(count ? count : 1, sizeof(*z->entries));
    if (!z->entries) {
        fail(err, errlen, "out of memory");
        tl_zip_close(z);
        return false;
    }

    uint64_t pos = cd_off;
    const uint64_t limit = cd_off + cd_size;
    for (uint64_t i = 0; i < count; i++) {
        uint8_t c[46];
        if (pos > limit || limit - pos < sizeof(c) || !zip_read(z, c, sizeof(c), pos)
            || rd32(c) != SIG_CENTRAL) {
            fail(err, errlen, "damaged central directory at entry %llu",
                 (unsigned long long)i);
            tl_zip_close(z);
            return false;
        }
        uint16_t name_len = rd16(c + 28);
        uint16_t extra_len = rd16(c + 30);
        uint16_t comment_len = rd16(c + 32);
        uint64_t record = 46ull + name_len + extra_len + comment_len;
        if (limit - pos < record) {
            fail(err, errlen, "central directory entry runs past its end");
            tl_zip_close(z);
            return false;
        }

        tl_zip_entry *en = &z->entries[z->count];
        en->flags = rd16(c + 8);
        en->method = rd16(c + 10);
        en->csize = rd32(c + 20);
        en->usize = rd32(c + 24);
        en->local_offset = rd32(c + 42);
        /* The extra field is only read when one of the three overflowed its 32 bits. */
        if (en->usize == 0xFFFFFFFFu || en->csize == 0xFFFFFFFFu
            || en->local_offset == 0xFFFFFFFFu) {
            uint8_t *extra = malloc(extra_len ? extra_len : 1);
            bool ok = extra && zip_read(z, extra, extra_len, pos + 46 + name_len)
                && apply_zip64_extra(extra, extra_len, en,
                                     en->usize == 0xFFFFFFFFu, en->csize == 0xFFFFFFFFu,
                                     en->local_offset == 0xFFFFFFFFu);
            free(extra);
            if (!ok) {
                fail(err, errlen, "damaged ZIP64 field in entry %llu",
                     (unsigned long long)i);
                tl_zip_close(z);
                return false;
            }
        }
        en->name = malloc((size_t)name_len + 1);
        if (!en->name) {
            fail(err, errlen, "out of memory");
            tl_zip_close(z);
            return false;
        }
        if (!zip_read(z, en->name, name_len, pos + 46)) {
            fail(err, errlen, "damaged central directory at entry %llu",
                 (unsigned long long)i);
            free(en->name);
            en->name = NULL;
            tl_zip_close(z);
            return false;
        }
        en->name[name_len] = '\0';
        /* A NUL inside a name would make it match something it is not. */
        if (strlen(en->name) != name_len) {
            free(en->name);
            en->name = NULL;
        } else {
            z->count++;
        }
        pos += record;
    }
    return true;
}

void tl_zip_close(tl_zip *z)
{
    if (z->entries) {
        for (size_t i = 0; i < z->count; i++) {
            free(z->entries[i].name);
        }
        free(z->entries);
    }
    if (z->map) {
        munmap((void *)z->map, z->size);
    }
    if (z->fd >= 0) {
        close(z->fd);
    }
    memset(z, 0, sizeof(*z));
    z->fd = -1;
}

const tl_zip_entry *tl_zip_find(const tl_zip *z, const char *name)
{
    for (size_t i = 0; i < z->count; i++) {
        if (strcmp(z->entries[i].name, name) == 0) {
            return &z->entries[i];
        }
    }
    return NULL;
}

bool tl_zip_mapped(const tl_zip *z)
{
    return z && z->map != NULL;
}

bool tl_zip_data_offset(const tl_zip *z, const tl_zip_entry *e, uint64_t *off,
                        char *err, size_t errlen)
{
    if (!z || !e) {
        fail(err, errlen, "no entry");
        return false;
    }
    /* The local header repeats the name and carries its own extra field, whose
     * length need not match the central directory's. */
    uint8_t l[30];
    if (!zip_read(z, l, sizeof(l), e->local_offset) || rd32(l) != SIG_LOCAL) {
        fail(err, errlen, "%s: damaged local header", e->name);
        return false;
    }
    *off = e->local_offset + 30ull + rd16(l + 26) + rd16(l + 28);
    return true;
}

bool tl_zip_data(const tl_zip *z, const tl_zip_entry *e, size_t limit,
                 const uint8_t **out, size_t *out_len, bool *owned,
                 char *err, size_t errlen)
{
    *out = NULL;
    *out_len = 0;
    *owned = false;

    if (e->flags & 1) {
        fail(err, errlen, "%s is encrypted", e->name);
        return false;
    }
    if (e->usize > limit) {
        fail(err, errlen, "%s is too large (%llu bytes)", e->name,
             (unsigned long long)e->usize);
        return false;
    }
    /* where the entry's bytes are (the local header carries its own extra field) */
    uint64_t data_off;
    if (!tl_zip_data_offset(z, e, &data_off, err, errlen)) {
        return false;
    }
    if (!in_file(z, data_off, e->csize)) {
        fail(err, errlen, "%s runs past the end of the file", e->name);
        return false;
    }
    const uint8_t *data = z->map ? z->map + data_off : NULL;

    if (e->method == 0) {
        if (e->csize != e->usize) {
            fail(err, errlen, "%s: stored entry with mismatched sizes", e->name);
            return false;
        }
        zip_saw(z, e->csize);          /* the caller is about to touch the mapping's pages */
        if (data) {
            *out = data;
            *out_len = (size_t)e->usize;
            return true;
        }
        /* Not mapped whole: the caller has asked for the bytes, so here they are.
         * `limit` bounds the size, and every caller passes one. */
        uint8_t *copy = malloc(e->usize ? (size_t)e->usize : 1);
        if (!copy) {
            fail(err, errlen, "out of memory reading %s", e->name);
            return false;
        }
        if (!zip_read(z, copy, (size_t)e->usize, data_off)) {
            free(copy);
            fail(err, errlen, "%s: cannot read its bytes", e->name);
            return false;
        }
        *out = copy;
        *out_len = (size_t)e->usize;
        *owned = true;
        return true;
    }
    if (e->method != 8) {
        fail(err, errlen, "%s uses compression method %u", e->name, e->method);
        return false;
    }

    uint8_t *buf = malloc(e->usize ? (size_t)e->usize : 1);
    if (!buf) {
        fail(err, errlen, "out of memory inflating %s", e->name);
        return false;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        free(buf);
        fail(err, errlen, "inflate failed to start");
        return false;
    }
    /* zlib counts in uInt; feed and drain in chunks so entries over 4 GiB
     * would work too, not that an APK should have one. A mapped file is fed
     * from the mapping; an unmapped one through a window read from the file. */
    uint64_t in_left = e->csize, out_left = e->usize, in_at = data_off;
    const uint8_t *in = data;
    uint8_t *feed = NULL;
    if (!in) {
        feed = malloc(TL_ZIP_IN);
        if (!feed) {
            inflateEnd(&zs);
            free(buf);
            fail(err, errlen, "out of memory inflating %s", e->name);
            return false;
        }
    }
    uint8_t *dst = buf;
    int rc = Z_OK;
    while (rc == Z_OK) {
        if (zs.avail_in == 0 && in_left > 0) {
            if (in) {
                uInt n = in_left > 0x40000000u ? 0x40000000u : (uInt)in_left;
                zs.next_in = (Bytef *)in;
                zs.avail_in = n;
                in += n;
                in_left -= n;
            } else {
                size_t want = in_left > TL_ZIP_IN ? TL_ZIP_IN : (size_t)in_left;
                size_t got = 0;
                while (got < want) {
                    ssize_t r = pread(z->fd, feed + got, want - got, (off_t)(in_at + got));
                    if (r <= 0) {
                        break;
                    }
                    got += (size_t)r;
                }
                if (!got) {
                    break;
                }
                zs.next_in = feed;
                zs.avail_in = (uInt)got;
                in_at += got;
                in_left -= got;
            }
        }
        if (zs.avail_out == 0 && out_left > 0) {
            uInt n = out_left > 0x40000000u ? 0x40000000u : (uInt)out_left;
            zs.next_out = dst;
            zs.avail_out = n;
            dst += n;
            out_left -= n;
        }
        if (zs.avail_in == 0 && zs.avail_out == 0) {
            break;
        }
        /* Z_BUF_ERROR ends the loop too: out of input before the end marker,
         * or more output than the directory promised. Both are damage. */
        rc = inflate(&zs, Z_NO_FLUSH);
    }
    uint64_t produced = zs.total_out;
    inflateEnd(&zs);
    free(feed);
    if (rc != Z_STREAM_END || produced != e->usize) {
        free(buf);
        fail(err, errlen, "%s did not inflate cleanly", e->name);
        return false;
    }
    zip_saw(z, e->csize);              /* the bytes came out of the mapping */
    *out = buf;
    *out_len = (size_t)e->usize;
    *owned = true;
    return true;
}

/* ------------------------------------------------------------------ resident pages and streams */

/*
 * The bytes an APK read put under iOS's eyes: clean pages of a file-backed
 * mapping, but resident all the same, and not ours to keep once they have
 * been served. iOS kills a machine on its footprint, so the mappings go back
 * when they have held too much; the entries do not move, and a later read
 * only pays a page-in. (MADV_DONTNEED on a PROT_READ private mapping just
 * drops the addresses: the file stays, and the pages come back untouched.)
 *
 * TL_ZIP_TRIM_MB sets how much a mapping may hold (0: never trim), which is
 * how the trimming is A/B'd on a phone.
 */
#define TL_ZIP_RESIDENT_MAX ((uint64_t)128 << 20)   /* per APK */

static uint64_t zip_resident_max(void)
{
    static uint64_t cached;                             /* 0: not read yet */
    uint64_t v = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (!v) {
        const char *s = getenv("TL_ZIP_TRIM_MB");
        long mb = s ? strtol(s, NULL, 10) : 128;
        v = mb > 0 ? (uint64_t)mb << 20 : UINT64_MAX;    /* 0 or less: never */
        __atomic_store_n(&cached, v, __ATOMIC_RELAXED);
    }
    return v;
}

void tl_zip_trim(tl_zip *z) /* used by the loader too, right after it has carved a library out */
{
    if (!z || !z->map || !z->size) return;
    uint64_t t = __atomic_load_n(&z->touched, __ATOMIC_RELAXED);
    if (t < zip_resident_max()) return;
    if (!__atomic_compare_exchange_n(&z->touched, &t, 0, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    madvise((void *)(uintptr_t)z->map, z->size, MADV_DONTNEED);
}

/* Read a stored entry's span, which the caller is about to touch. */
static void zip_saw(const tl_zip *z, uint64_t n)
{
    __atomic_add_fetch(&((tl_zip *)z)->touched, n ? n : 1, __ATOMIC_RELAXED);
    tl_zip_trim((tl_zip *)z);
}

struct tl_zip_stream {
    const tl_zip    *z;
    const tl_zip_entry *e;
    uint64_t  size;                    /* the entry's uncompressed size */
    uint64_t  pos;                     /* the read position */
    bool      stored;
    const uint8_t *direct;             /* a stored entry: where its bytes are (NULL: not mapped) */
    void      *whole_map;              /* a stored entry's own mapping, when the file has none */
    size_t     whole_map_len;
    /* a deflated one: the inflater and the little window ahead of `pos` */
    z_stream  zs;
    bool      zs_on, done;
    uint64_t  data_off;                /* where the entry's compressed bytes are */
    uint64_t  in_left;                 /* compressed bytes left */
    uint64_t  in_at;                   /* where the next of them lies, in the file */
    const uint8_t *in;                 /* where they are, when the file is mapped */
    uint8_t  *feed;                    /* the window they are read through when it is not */
    uint64_t  out_pos;                 /* win[0] is the offset `out_pos` of the output */
    size_t    wlen;                    /* valid window bytes */
    uint8_t  *win;
    uint8_t  *whole;                   /* tl_zip_stream_whole()'s answer, once asked for */
};

#define TL_ZIP_WIN (512 << 10)

tl_zip_stream *tl_zip_stream_open(const tl_zip *z, const tl_zip_entry *e,
                                  size_t limit, char *err, size_t errlen)
{
    if (!z || !e) { fail(err, errlen, "no entry to stream"); return NULL; }
    if (e->flags & 1) { fail(err, errlen, "%s is encrypted", e->name); return NULL; }
    if (e->usize > limit) {
        fail(err, errlen, "%s is too large (%llu bytes)", e->name, (unsigned long long)e->usize);
        return NULL;
    }

    /* where the entry's bytes are (the local header carries its own extra field) */
    uint64_t data_off;
    if (!tl_zip_data_offset(z, e, &data_off, err, errlen)) {
        return NULL;
    }
    if (!in_file(z, data_off, e->csize)) {
        fail(err, errlen, "%s runs past the end of the file", e->name);
        return NULL;
    }

    tl_zip_stream *s = calloc(1, sizeof(*s));
    if (!s) { fail(err, errlen, "out of memory"); return NULL; }
    s->z = z; s->e = e;
    s->size = e->usize;
    s->data_off = data_off;
    s->in_at = data_off;
    s->stored = e->method == 0;
    if (s->stored) {
        if (e->csize != e->usize) { free(s); fail(err, errlen, "%s: stored entry with mismatched sizes", e->name); return NULL; }
        s->direct = z->map ? z->map + data_off : NULL;      /* unmapped: tl_zip_stream_whole maps it */
        return s;
    }
    if (e->method != 8) { free(s); fail(err, errlen, "%s uses compression method %u", e->name, e->method); return NULL; }
    s->win = malloc(TL_ZIP_WIN);
    if (!s->win) { free(s); fail(err, errlen, "out of memory"); return NULL; }
    s->zs_on = inflateInit2(&s->zs, -MAX_WBITS) == Z_OK;
    if (!s->zs_on) { free(s->win); free(s); fail(err, errlen, "inflate failed to start"); return NULL; }
    s->in = z->map ? z->map + data_off : NULL;
    s->in_left = e->csize;
    if (!s->in) {                                   /* an unmapped file: read the entry through a window */
        s->feed = malloc(TL_ZIP_IN);
        if (!s->feed) { inflateEnd(&s->zs); free(s->win); free(s); fail(err, errlen, "out of memory"); return NULL; }
    }
    return s;
}

void tl_zip_stream_close(tl_zip_stream *s)
{
    if (!s) return;
    if (s->zs_on) inflateEnd(&s->zs);
    if (s->whole_map) munmap(s->whole_map, s->whole_map_len);   /* s->whole points into it */
    else free(s->whole);
    free(s->feed);
    free(s->win);
    free(s);
}

uint64_t tl_zip_stream_pos(const tl_zip_stream *s) { return s ? s->pos : 0; }
uint64_t tl_zip_stream_len(const tl_zip_stream *s) { return s ? s->size : 0; }

/* Drop the window's front, up to `keep_from` (an output offset). Bytes that
 * have not been inflated yet cannot be dropped, so `keep_from` is clamped to
 * what the window holds: a caller that asks past it simply gets an empty
 * window lying at the end of the output. */
static void zs_shift(tl_zip_stream *s, uint64_t keep_from)
{
    uint64_t produced = s->out_pos + s->wlen;
    if (keep_from > produced) keep_from = produced;
    if (keep_from <= s->out_pos) return;
    uint64_t drop = keep_from - s->out_pos;
    if (drop >= s->wlen) {
        s->out_pos = produced;
        s->wlen = 0;
        return;
    }
    memmove(s->win, s->win + drop, s->wlen - drop);
    s->out_pos += drop;
    s->wlen -= (size_t)drop;
}

/* Inflate until the window reaches `upto` (an output offset), or the stream
 * ends. Bytes below `floor` are not kept: a seek to a far offset walks the
 * window forward, dropping what it passes. `out_pos` is never left below
 * `floor`, and `floor + TL_ZIP_WIN` bounds `upto`, so the window is enough. */
static bool zs_produce(tl_zip_stream *s, uint64_t floor, uint64_t upto)
{
    if (upto > floor + TL_ZIP_WIN) upto = floor + TL_ZIP_WIN;
    if (s->done || s->out_pos + s->wlen >= upto) return true;
    int still = 0;                             /* rounds in which zlib neither read nor wrote: a hang, not a stall */
    while (!s->done && s->out_pos + s->wlen < upto) {
        if (s->wlen == TL_ZIP_WIN) {              /* full, and none of it is wanted: slide forward */
            uint64_t keep = s->out_pos + s->wlen - TL_ZIP_WIN;
            zs_shift(s, keep < floor ? floor : keep);
        }
        if (s->zs.avail_in == 0 && s->in_left > 0) {
            if (s->in) {
                uInt n = s->in_left > 0x40000000u ? 0x40000000u : (uInt)s->in_left;
                s->zs.next_in = (Bytef *)s->in;
                s->zs.avail_in = n;
                s->in += n;
                s->in_left -= n;
            } else {
                /* an unmapped file: fill the window from it */
                size_t want = s->in_left > TL_ZIP_IN ? TL_ZIP_IN : (size_t)s->in_left;
                size_t got = 0;
                while (got < want) {
                    ssize_t r = pread(s->z->fd, s->feed + got, want - got, (off_t)(s->in_at + got));
                    if (r <= 0) {
                        break;
                    }
                    got += (size_t)r;
                }
                if (!got) {
                    return false;                     /* the file ended before the entry did */
                }
                s->zs.next_in = s->feed;
                s->zs.avail_in = (uInt)got;
                s->in_at += got;
                s->in_left -= got;
            }
        }
        s->zs.next_out = s->win + s->wlen;
        s->zs.avail_out = (uInt)(TL_ZIP_WIN - s->wlen);
        uint64_t before = s->zs.total_out, read_before = s->zs.total_in;
        int rc = inflate(&s->zs, Z_NO_FLUSH);
        size_t made = (size_t)(s->zs.total_out - before);
        s->wlen += made;
        zip_saw(s->z, s->zs.total_in - read_before);
        if (!made && s->zs.total_in == read_before && ++still > 2) return false;
        if (made || s->zs.total_in != read_before) still = 0;
        if (rc == Z_STREAM_END) {
            s->done = true;
            /* an entry that ends before the size its directory promised is damage, not an early end */
            if (s->out_pos + s->wlen != s->size) return false;
        } else if (rc == Z_BUF_ERROR) {
            /* zlib makes no progress: the input is exhausted without its end marker, or
             * the bytes are damage. Both are the same answer for a caller. */
            if (!made) return false;
        } else if (rc != Z_OK) {
            return false;
        }
    }
    return true;
}

/* Put the read (and the window) at `pos`, inflating afresh whatever lies
 * behind it; the stream's `pos` is left where it was on failure. */
static bool zs_seek(tl_zip_stream *s, uint64_t pos)
{
    if (s->stored) { if (pos > s->size) return false; s->pos = pos; return true; }
    if (pos > s->size) return false;
    if (pos < s->out_pos || pos > s->out_pos + s->wlen) {
        if (pos < s->out_pos) {                    /* raw deflate cannot go back: start again */
            if (inflateReset(&s->zs) != Z_OK) return false;
            s->zs.avail_in = 0;
            s->out_pos = 0; s->wlen = 0;
            s->in = s->z->map ? s->z->map + s->data_off : NULL;
            s->in_at = s->data_off;
            s->in_left = s->e->csize;
            s->done = false;
        }
        /* slide the window up to pos, keeping nothing of what it passes */
        while (!s->done && s->out_pos + s->wlen < pos) {
            uint64_t floor = s->out_pos + s->wlen;
            if (!zs_produce(s, floor, pos)) return false;
        }
        if (s->out_pos + s->wlen < pos) {
            if (pos < s->size) return false;       /* the entry ran out before `pos`: damage */
            s->pos = pos;
            return true;
        }
    }
    zs_shift(s, pos);
    s->pos = pos;
    return true;
}

uint64_t tl_zip_stream_seek(tl_zip_stream *s, uint64_t at, char *err, size_t errlen)
{
    if (!s || !zs_seek(s, at)) {
        fail(err, errlen, "cannot seek an asset");
        return (uint64_t)-1;
    }
    return s->pos;
}

size_t tl_zip_stream_read(tl_zip_stream *s, void *buf, size_t n, char *err, size_t errlen)
{
    if (!s) { fail(err, errlen, "no stream"); return (size_t)-1; }
    if ((uint64_t)n > s->size - s->pos) n = (size_t)(s->size - s->pos);
    if (!n) return 0;

    if (s->stored) {
        zip_saw(s->z, n);
        if (s->direct) {
            memcpy(buf, s->direct + s->pos, (size_t)n);
        } else if (!zip_read(s->z, buf, (size_t)n, s->data_off + s->pos)) {
            fail(err, errlen, "%s: cannot read its bytes", s->e->name);
            return (size_t)-1;
        }
        s->pos += (uint64_t)n;
        return n;
    }
    if (!zs_seek(s, s->pos)) { fail(err, errlen, "%s did not inflate cleanly", s->e->name); return (size_t)-1; }
    size_t got = 0;
    while (got < n) {
        if (s->pos >= s->out_pos + s->wlen) {
            if (s->done) break;
            zs_shift(s, s->pos);
            if (!zs_produce(s, s->pos, s->pos + (n - got))) { fail(err, errlen, "%s did not inflate cleanly", s->e->name); return (size_t)-1; }
            if (s->pos >= s->out_pos + s->wlen) break;
        }
        uint64_t off = s->pos - s->out_pos;
        size_t have = s->wlen - (size_t)off;
        size_t take = have < n - got ? have : n - got;
        memcpy((uint8_t *)buf + got, s->win + off, take);
        s->pos += take;
        got += take;
    }
    if (got < n && !s->done) { fail(err, errlen, "%s did not inflate cleanly", s->e->name); return (size_t)-1; }
    return got;
}

/*
 * A stored entry of an archive that is not mapped whole: map just this entry's
 * bytes, from its own offset in the file. Android hands a stored asset over as a
 * pointer into the APK's mapping; this is the same pointer, over a mapping of the
 * entry instead of the file, so the address space a game holds is the assets it
 * actually opens rather than its whole APK. The stream owns the mapping and gives
 * it back when it is closed.
 */
static bool stream_map_entry(tl_zip_stream *s)
{
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    if (!ps) {
        return false;
    }
    uint64_t base = s->data_off & ~(uint64_t)(ps - 1);
    uint64_t skip = s->data_off - base;
    size_t len = (size_t)(skip + s->size);
    if (!len) {
        len = 1;
    }
    void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, s->z->fd, (off_t)base);
    if (m == MAP_FAILED) {
        return false;
    }
    s->whole_map = m;
    s->whole_map_len = len;
    s->whole = (uint8_t *)m + skip;
    return true;
}

const uint8_t *tl_zip_stream_whole(tl_zip_stream *s, char *err, size_t errlen)
{
    if (!s) { fail(err, errlen, "no stream"); return NULL; }
    if (s->stored) {
        /* a stored entry's answer is a pointer, so an unmapped archive needs a
         * mapping of this entry (and only this entry) to give one */
        if (s->direct) {
            return s->direct;
        }
        if (!s->whole && !stream_map_entry(s)) {
            fail(err, errlen, "%s: cannot map its bytes", s->e->name);
            return NULL;
        }
        return s->whole;
    }
    if (!s->whole) {
        uint8_t *buf = malloc(s->size ? (size_t)s->size : 1);
        if (!buf) { fail(err, errlen, "out of memory inflating %s", s->e->name); return NULL; }
        uint64_t got = 0;
        while (got < s->size) {
            if (!zs_seek(s, got) || !zs_produce(s, got, got + TL_ZIP_WIN)) {
                free(buf);
                fail(err, errlen, "%s did not inflate cleanly", s->e->name);
                return NULL;
            }
            uint64_t off = got - s->out_pos;                    /* the window starts at, or before, `got` */
            size_t have = s->wlen - (size_t)off;
            size_t take = (size_t)(s->size - got) < have ? (size_t)(s->size - got) : have;
            if (!take) { free(buf); fail(err, errlen, "%s did not inflate cleanly", s->e->name); return NULL; }
            memcpy(buf + got, s->win + off, take);
            got += take;
        }
        s->whole = buf;
    }
    return s->whole;
}

/* ------------------------------------------------------------------ extraction (web apps) */

static void mkdirs_for(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
    }
}

int husk_tl_extract(const char *apk, const char *prefix, const char *out_dir)
{
    tl_zip z;
    char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) return -1;
    size_t plen = strlen(prefix);
    int written = 0;
    for (size_t i = 0; i < z.count; i++) {
        const tl_zip_entry *e = &z.entries[i];
        if (strncmp(e->name, prefix, plen) || !e->name[plen]) continue;
        const char *rel = e->name + plen;
        if (strstr(rel, "..") || rel[strlen(rel) - 1] == '/') continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", out_dir, rel);
        mkdirs_for(path);
        /* streamed: nothing bigger than a slice of the entry is ever in memory */
        tl_zip_stream *st = tl_zip_stream_open(&z, e, (size_t)512 << 20, err, sizeof(err));
        if (!st) continue;
        size_t want = (size_t)tl_zip_stream_len(st), got = 0;
        FILE *f = fopen(path, "wb");
        if (f) {
            uint8_t buf[64 << 10];             /* small: the thread this runs on may have a 512 KiB stack */
            size_t r;
            bool good = true;
            while ((r = tl_zip_stream_read(st, buf, sizeof(buf), err, sizeof(err))) > 0 && r != (size_t)-1) {
                if (fwrite(buf, 1, r, f) != r) { good = false; break; }
                got += r;
            }
            fclose(f);
            if (good && got == want) written++;
            else unlink(path);                 /* a half-written entry is not an extracted one */
        }
        tl_zip_stream_close(st);
    }
    tl_zip_close(&z);
    return written;
}

int husk_tl_has_entry(const char *apk, const char *name)
{
    tl_zip z;
    char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) return 0;
    int found = tl_zip_find(&z, name) != NULL;
    tl_zip_close(&z);
    return found;
}
