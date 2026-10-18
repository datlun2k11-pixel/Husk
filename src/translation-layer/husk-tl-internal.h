/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared between the translation layer's own files. Not part of the bridge:
 * Swift sees husk-tl.h only.
 */
#ifndef HUSK_TL_INTERNAL_H
#define HUSK_TL_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------ the window ---
 *
 * The software ANativeWindow the guest draws RGBX into, shared by the
 * loader (which publishes frames) and the shim (which implements the
 * ANativeWindow calls), so both see one layout.
 */
typedef struct tl_window tl_window;
struct tl_window {
    int32_t width, height, format;   /* 1 == WINDOW_FORMAT_RGBA_8888 */
    int stridePixels;
    uint8_t *bits;                   /* width * height * 4 bytes */
    pthread_mutex_t lock;            /* held between lock and unlockAndPost */
    atomic_int refcount;
};

tl_window *tl_window_create(int width, int height);
void tl_window_acquire(tl_window *w);
void tl_window_release(tl_window *w);

/* ------------------------------------------------------------------ JSON */

/*
 * Just enough of a JSON writer for the reports. Commas are placed by the
 * writer, not the caller, which is where hand-built JSON usually goes wrong.
 * Any allocation failure is sticky and turns the result into an error object.
 */
typedef struct tl_json {
    char  *buf;
    size_t len, cap;
    bool   failed;
    int    depth;
    bool   after_key;
    bool   need_comma[32];
} tl_json;

void  tl_json_init(tl_json *j);
void  tl_json_begin_object(tl_json *j);
void  tl_json_end_object(tl_json *j);
void  tl_json_begin_array(tl_json *j);
void  tl_json_end_array(tl_json *j);
void  tl_json_key(tl_json *j, const char *key);
void  tl_json_string(tl_json *j, const char *s);   /* NULL writes null */
void  tl_json_int(tl_json *j, long long v);
void  tl_json_bool(tl_json *j, bool v);
char *tl_json_finish(tl_json *j);

/* ------------------------------------------------------------- JIT Memory */

typedef struct tl_dual_mapping {
    uint8_t *rw_addr;
    uint8_t *rx_addr;
    size_t   size;
} tl_dual_mapping;

tl_dual_mapping *tl_find_stikdebug_prewarmed(void);
bool tl_jit_reserve_bottom(size_t end);

/* ------------------------------------------------------------------- ZIP */

typedef struct tl_zip_entry {
    char    *name;
    uint16_t method;        /* 0 stored, 8 deflated */
    uint16_t flags;
    uint64_t csize;
    uint64_t usize;
    uint64_t local_offset;
} tl_zip_entry;

typedef struct tl_zip {
    int            fd;
    const uint8_t *map;
    size_t         size;
    tl_zip_entry  *entries;
    size_t         count;
    uint64_t       touched;   /* bytes read since the mapping was last trimmed */
} tl_zip;

/* Reads the central directory. The file stays mapped until tl_zip_close(). */
bool tl_zip_open(tl_zip *z, const char *path, char *err, size_t errlen);
void tl_zip_close(tl_zip *z);
const tl_zip_entry *tl_zip_find(const tl_zip *z, const char *name);

/*
 * An entry's bytes. A stored entry points straight into the mapping and
 * `*owned` is false; a deflated one is inflated into a buffer the caller must
 * free(), and `*owned` is true. Entries larger than `limit` are refused.
 */
bool tl_zip_data(const tl_zip *z, const tl_zip_entry *e, size_t limit,
                 const uint8_t **out, size_t *out_len, bool *owned,
                 char *err, size_t errlen);

/*
 * A mapped APK holds its touched pages resident, and iOS counts them in the
 * memory footprint it kills on. tl_zip_trim() lets go of them: they are clean
 * file pages, so nothing is lost, and the next read faults them back from the
 * disk (a few milliseconds). The reads here count their volume and trim when
 * it passes a few hundred megabytes, so a game that streams its way through
 * tens of gigabytes keeps the mapping's resident set small all along.
 */
void tl_zip_trim(tl_zip *z);

/*
 * An entry as a stream: it is read one small window at a time, so a deflated
 * asset is never held whole -- a 1 GiB one opens in a quarter of a megabyte.
 * A stored entry reads straight out of the mapping. `pos` runs over the
 * uncompressed bytes; seeks backwards re-inflate what they skip (raw deflate
 * has no memory). Failing paths return `(size_t)-1` for sizes and positions.
 */
typedef struct tl_zip_stream tl_zip_stream;

/* `limit` is as in tl_zip_data(). The stream holds pointers taken from `z`,
 * which must stay open until tl_zip_stream_close(). */
tl_zip_stream *tl_zip_stream_open(const tl_zip *z, const tl_zip_entry *e,
                                  size_t limit, char *err, size_t errlen);
/* Reads up to `n` bytes at `pos`, advancing it. 0 is the end of the entry. */
size_t tl_zip_stream_read(tl_zip_stream *s, void *buf, size_t n, char *err, size_t errlen);
/* Read position and length, in uncompressed bytes. */
uint64_t tl_zip_stream_pos(const tl_zip_stream *s);
uint64_t tl_zip_stream_len(const tl_zip_stream *s);
/* Puts the read at `at` (uncompressed bytes; behind the position means
 * re-inflating from the top). `(uint64_t)-1` is failure. */
uint64_t tl_zip_stream_seek(tl_zip_stream *s, uint64_t at, char *err, size_t errlen);
/* The whole entry, from its first byte, held by the stream. The reads so far
 * do not constrain it. A stored entry's answer points into the mapping. */
const uint8_t *tl_zip_stream_whole(tl_zip_stream *s, char *err, size_t errlen);
void tl_zip_stream_close(tl_zip_stream *s);

/* ------------------------------------------------------------------- ELF */

/*
 * How one page of a library image has to be mapped. A page can hold parts of
 * several segments -- with 4 KiB alignment, a 16 KiB page often does -- so
 * these combine.
 */
enum {
    TL_PAGE_R = 1,          /* something is loaded here */
    TL_PAGE_X = 2,          /* code: must execute */
    TL_PAGE_W = 4,          /* data written after relocation: must be writable */
    TL_PAGE_CARVED = 8,     /* carved into independent RW page */
};

typedef struct tl_segment {
    uint64_t vaddr;
    uint64_t memsz;
    uint32_t flags;         /* PF_R / PF_W / PF_X */
} tl_segment;

/*
 * Lay the loadable segments out on pages of `page` bytes. Writable bytes that
 * PT_GNU_RELRO covers do not count as writable: they are only written while
 * relocating, which the loader does through its own writable alias.
 *
 * Returns the number of pages and fills `flags` (up to `max` entries) and
 * `*base`, the first page's address; 0 means the layout is unusable. With
 * `flags` NULL it only counts, so the caller can size the array.
 * This is the loader's plan as much as the scanner's -- the scanner reports it
 * so that an app's fate is known before anything is loaded.
 */
size_t tl_page_plan(const tl_segment *loads, size_t nloads,
                    const tl_segment *relro, uint64_t page,
                    uint8_t *flags, size_t max, uint64_t *base);

#define TL_MAX_NEEDED 48

typedef struct tl_elf_report {
    bool     ok;
    char     error[160];
    bool     is64;
    uint16_t machine;

    /* layout */
    uint64_t min_align, max_align;
    uint64_t span;              /* bytes, whole 16 KiB pages */
    uint32_t pages, pages_exec, pages_write, pages_conflict;
    char     layout[96];        /* run-length summary, e.g. "R1 X4 R1 W2" */

    /* code */
    uint32_t svc;               /* direct system calls */
    uint32_t tpidr_reads;       /* mrs Xn, tpidr_el0 */
    uint32_t tpidr_writes;      /* msr tpidr_el0, Xn */

    /* dynamic linking */
    bool     has_tls;
    bool     textrel;
    uint32_t relocs;
    uint32_t relocs_tls;
    uint32_t relocs_unsupported;
    uint32_t first_unsupported;
    const char *packing;        /* "none", "android" (APS2) or "relr" */
    uint32_t imports;
    bool     exports_sdl_main;          /* SDL_main / SDL_Main, or SDLActivity natives: a game that carries SDL */
    bool     exports_native_activity;   /* ANativeActivity_onCreate */
    bool     exports_jni_onload;        /* JNI_OnLoad */
    char     soname[96];
    int      needed_count;
    char     needed[TL_MAX_NEEDED][64];
} tl_elf_report;

void tl_elf_analyze(const uint8_t *data, size_t size, tl_elf_report *r);

#endif /* HUSK_TL_INTERNAL_H */
