/*
 * The zip layer's streamed entry reads: what tl_zip_stream_ gives a game that
 * opens an asset is the same bytes tl_zip_data always did, but a deflated
 * entry of any size reads in a few hundred kilobytes of its own.
 *
 *   bash tools/regress/build.sh tools/zipstream-test.c /tmp/zipstream-test && /tmp/zipstream-test
 */
#include "husk-tl-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void check(bool ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); fails++; }
}

/* a byte of the generated file: seed 0 is all zeros, otherwise a shape deflate likes */
static uint8_t byte_at(size_t i, size_t size, unsigned seed)
{
    if (!seed) return 0;
    uint32_t s = (uint32_t)(seed | 1) * 2654435761u + (uint32_t)i * 40503u;
    if (i < size / 3) return (uint8_t)('A' + (s >> 16) % 26);          /* compressible runs */
    if (i < 2 * size / 3) return (uint8_t)('a' + (i % 26));            /* a cycle */
    return (uint8_t)(s >> 13);                                         /* noise */
}

/* a file with a shape deflate likes, built a chunk at a time so big ones are quick to make */
static int make_file(const char *path, size_t size, unsigned seed)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t chunk[65536];
    for (size_t i = 0; i < size;) {
        size_t n = size - i < sizeof(chunk) ? size - i : sizeof(chunk);
        for (size_t k = 0; k < n; k++) chunk[k] = byte_at(i + k, size, seed);
        if (fwrite(chunk, 1, n, f) != n) { fclose(f); return -1; }
        i += n;
    }
    fclose(f);
    return 0;
}

int main(void)
{
    char dir[] = "/tmp/husk-zipstream-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }

    enum { BIG = 13 << 20, TINY = 4096 };
    char big[300], tiny[300], stored[300], apkp[300], cmd[900];
    snprintf(big, sizeof(big), "%s/big.bin", dir);
    snprintf(tiny, sizeof(tiny), "%s/tiny.txt", dir);
    snprintf(stored, sizeof(stored), "%s/stored.bin", dir);
    snprintf(apkp, sizeof(apkp), "%s/archive.apk", dir);

    if (make_file(big, BIG, 7) || make_file(tiny, TINY, 9) || make_file(stored, BIG, 11)) { perror("make_file"); return 2; }
    snprintf(cmd, sizeof(cmd), "cd '%s' && : > empty.bin && zip -q -X archive.apk big.bin tiny.txt empty.bin && zip -q -X -0 archive.apk stored.bin", dir);
    if (system(cmd) != 0) { printf("FAIL: zip command\n"); return 2; }

    tl_zip z; char err[256];
    if (!tl_zip_open(&z, apkp, err, sizeof(err))) { printf("FAIL: open: %s\n", err); return 2; }

    /* ---- the point of all this: a huge deflated entry streams in a little memory ----
     * It runs before the test's own truth buffers are allocated, so the peak below
     * is the stream's and nothing else's. */
    {
        enum { HUGE = 256 << 20 };
        char hugep[300], hugeapk[300];
        snprintf(hugep, sizeof(hugep), "%s/huge.bin", dir);
        snprintf(hugeapk, sizeof(hugeapk), "%s/huge.apk", dir);
        if (make_file(hugep, HUGE, 0) == 0) {
            snprintf(cmd, sizeof(cmd), "cd '%s' && zip -q -9 huge.apk huge.bin", dir);
            if (system(cmd) == 0) {
                tl_zip z2;
                if (tl_zip_open(&z2, hugeapk, err, sizeof(err))) {
                    const tl_zip_entry *e = tl_zip_find(&z2, "huge.bin");
                    check(e && e->usize == HUGE, "the huge entry is as big as it was written");
                    tl_zip_stream *s = e ? tl_zip_stream_open(&z2, e, 1u << 30, err, sizeof(err)) : NULL;
                    check(s != NULL, "the huge entry streams");
                    if (s) {
                        size_t step = 64 << 10;
                        uint8_t *buf = malloc(step);
                        uint64_t at = 0;
                        bool same = true;
                        for (;;) {
                            size_t r = tl_zip_stream_read(s, buf, step, err, sizeof(err));
                            if (r == (size_t)-1) { same = false; break; }
                            if (!r) break;
                            for (size_t k = 0; k < r; k++)
                                if (buf[k] != byte_at((size_t)at + k, HUGE, 0)) { same = false; break; }
                            if (!same) break;
                            at += r;
                        }
                        check(same && at == HUGE, "the huge entry's bytes are right");
                        free(buf);
                        tl_zip_stream_close(s);
                    }
                    tl_zip_close(&z2);
                }
                /* Read in windows, the process never came near holding the entry: without that,
                 * this run's peak would be a quarter of a gigabyte. */
                struct rusage ru;
                getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
                uint64_t peak = (uint64_t)ru.ru_maxrss;              /* bytes */
#else
                uint64_t peak = (uint64_t)ru.ru_maxrss << 10;        /* KiB */
#endif
                if (peak >= (32ull << 20)) printf("# peak after streaming 256 MiB: %.1f MiB\n", (double)peak / (1024.0 * 1024.0));
                check(peak < (32ull << 20), "streaming a 256 MiB entry stays in tens of megabytes");
            }
            unlink(hugep); unlink(hugeapk);
        }
    }

    /* the originals, kept on disk, are the truth the streams must answer with */
    uint8_t *truth_big = malloc(BIG), *truth_stored = malloc(BIG), *truth_tiny = malloc(TINY);
    FILE *f = fopen(big, "rb"); if (fread(truth_big, 1, BIG, f) != BIG) return 2; fclose(f);
    f = fopen(stored, "rb"); if (fread(truth_stored, 1, BIG, f) != BIG) return 2; fclose(f);
    f = fopen(tiny, "rb"); if (fread(truth_tiny, 1, TINY, f) != TINY) return 2; fclose(f);

    struct { const char *name; const uint8_t *truth; size_t size; bool expect_stored; } cases[] = {
        { "big.bin", truth_big, BIG, false },
        { "tiny.txt", truth_tiny, TINY, false },
        { "stored.bin", truth_stored, BIG, true },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const tl_zip_entry *e = tl_zip_find(&z, cases[i].name);
        char what[160]; snprintf(what, sizeof(what), "%s found", cases[i].name);
        check(e != NULL, what);
        if (!e) continue;

        tl_zip_stream *s = tl_zip_stream_open(&z, e, 1u << 30, err, sizeof(err));
        snprintf(what, sizeof(what), "%s opened (%s)", cases[i].name, err);
        check(s != NULL, what);
        if (!s) continue;
        snprintf(what, sizeof(what), "%s length", cases[i].name);
        check(tl_zip_stream_len(s) == cases[i].size, what);
        snprintf(what, sizeof(what), "%s stored-ness", cases[i].name);
        check((e->method == 0) == cases[i].expect_stored, what);

        /* in odd-sized chunks, at every offset the odd size can put the read at */
        uint8_t *got = malloc(cases[i].size);
        size_t step = 3997;
        size_t r; size_t gotn = 0;
        while ((r = tl_zip_stream_read(s, got + gotn, step, err, sizeof(err))) > 0 && r != (size_t)-1) gotn += r;
        snprintf(what, sizeof(what), "%s sequential read (%s)", cases[i].name, err);
        check(gotn == cases[i].size, what);
        snprintf(what, sizeof(what), "%s sequential bytes", cases[i].name);
        check(memcmp(got, cases[i].truth, cases[i].size) == 0, what);

        /* a whole-entry read moves the stream: nothing it did above may play with this */
        const uint8_t *whole = tl_zip_stream_whole(s, err, sizeof(err));
        snprintf(what, sizeof(what), "%s whole (%s)", cases[i].name, err);
        check(whole && memcmp(whole, cases[i].truth, cases[i].size) == 0, what);

        snprintf(what, sizeof(what), "%s whole vs tl_zip_data", cases[i].name);
        const uint8_t *old; size_t oldn; bool owned;
        check(tl_zip_data(&z, e, 1u << 30, &old, &oldn, &owned, err, sizeof(err))
              && oldn == cases[i].size && memcmp(old, cases[i].truth, oldn) == 0, what);
        if (owned) free((void *)old);

        /* random seeks against the truth, reads straddling what the window holds */
        srand((unsigned)i + 1);
        bool ok = true;
        for (int k = 0; k < 40; k++) {
            uint64_t at = ((uint64_t)rand() << 32 | (unsigned)rand()) % cases[i].size;
            uint64_t want = cases[i].size - at > 70000 ? 70000 : cases[i].size - at;
            if (tl_zip_stream_seek(s, at, err, sizeof(err)) != at) { ok = false; break; }
            r = tl_zip_stream_read(s, got, (size_t)want, err, sizeof(err));
            if (r != want || memcmp(got, cases[i].truth + at, (size_t)want) != 0) { ok = false; break; }
            /* straight after: the little forward read */
            r = tl_zip_stream_read(s, got, 12345, err, sizeof(err));
            at += want;
            uint64_t w2 = cases[i].size - at > 12345 ? 12345 : cases[i].size - at;
            if (r != w2 || memcmp(got, cases[i].truth + at, (size_t)w2) != 0) { ok = false; break; }
        }
        snprintf(what, sizeof(what), "%s random seeks", cases[i].name);
        check(ok, what);

        /* the window is half a megabyte: reads around its edges and one past it */
        uint64_t edges[][2] = { { 0, 700000 }, { 524288 - 3, 9 }, { 524288, 524288 },
                                { cases[i].size - 1, 1 }, { cases[i].size / 2, 700000 } };
        ok = true;
        for (size_t k = 0; k < sizeof(edges) / sizeof(edges[0]); k++) {
            uint64_t at = edges[k][0], want = edges[k][1];
            if (at >= cases[i].size) continue;
            if (want > cases[i].size - at) want = cases[i].size - at;
            if (tl_zip_stream_seek(s, at, err, sizeof(err)) != at) { ok = false; break; }
            r = tl_zip_stream_read(s, got, (size_t)want, err, sizeof(err));
            if (r != want || memcmp(got, cases[i].truth + at, (size_t)want) != 0) { ok = false; break; }
        }
        snprintf(what, sizeof(what), "%s window edges", cases[i].name);
        check(ok, what);

        /* after whole(), the reads still know where they are */
        ok = tl_zip_stream_seek(s, 1000, err, sizeof(err)) == 1000
             && tl_zip_stream_read(s, got, 2000, err, sizeof(err)) == 2000
             && memcmp(got, cases[i].truth + 1000, 2000) == 0;
        snprintf(what, sizeof(what), "%s reads after whole", cases[i].name);
        check(ok, what);
        free(got);

        /* and a second stream over the same zip, alive at the same time */
        tl_zip_stream *s2 = tl_zip_stream_open(&z, e, 1u << 30, err, sizeof(err));
        check(s2 != NULL, "second stream");
        if (s2) {
            uint8_t a[4096], b[4096];
            check(tl_zip_stream_seek(s, 0, err, sizeof(err)) == 0, "seek back to the start");
            check(tl_zip_stream_read(s, a, sizeof(a), err, sizeof(err)) == sizeof(a), "first stream reads");
            check(tl_zip_stream_read(s2, b, sizeof(b), err, sizeof(err)) == sizeof(b), "second stream reads");
            check(memcmp(a, b, sizeof(a)) == 0, "two streams agree");
            check(memcmp(a, cases[i].truth, sizeof(a)) == 0, "two streams read the truth");
            tl_zip_stream_close(s2);
        }
        tl_zip_stream_close(s);
    }

    /* an empty entry still opens, and reads nothing */
    { const tl_zip_entry *e = tl_zip_find(&z, "empty.bin");
      check(e != NULL, "empty.bin found");
      if (e) {
          tl_zip_stream *s = tl_zip_stream_open(&z, e, 1u << 30, err, sizeof(err));
          uint8_t b[8];
          check(s != NULL, "empty entry opens");
          check(s && tl_zip_stream_len(s) == 0, "empty entry has length 0");
          check(s && tl_zip_stream_read(s, b, sizeof(b), err, sizeof(err)) == 0, "empty entry reads nothing");
          check(s && tl_zip_stream_whole(s, err, sizeof(err)) != NULL, "empty entry is whole at once");
          tl_zip_stream_close(s);
      } }

    /* a missing entry answers NULL, and the error path is the one tl_zip_data keeps */
    check(tl_zip_find(&z, "no-such-file") == NULL, "a missing entry is not found");
    { const uint8_t *d; size_t n; bool owned;
      check(!tl_zip_data(&z, &z.entries[0], 8, &d, &n, &owned, err, sizeof(err)), "a limit below the entry is refused"); }
    { tl_zip_stream *s = tl_zip_stream_open(&z, &z.entries[0], 8, err, sizeof(err));
      check(s == NULL, "a stream refuses an entry over its limit"); }

    tl_zip_close(&z);

    /* a damaged deflated entry is refused: it cannot answer with bytes it never had, and it cannot spin */
    {
        char copyp[300];
        snprintf(copyp, sizeof(copyp), "%s/corrupt.apk", dir);
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", apkp, copyp);
        if (system(cmd) == 0) {
            tl_zip cz;
            long off = -1;
            if (tl_zip_open(&cz, copyp, err, sizeof(err))) {
                const tl_zip_entry *e = tl_zip_find(&cz, "big.bin");
                if (e) {
                    const uint8_t *l = cz.map + e->local_offset;
                    off = (long)(e->local_offset + 30 + (l[26] | l[27] << 8) + (l[28] | l[29] << 8));
                }
                tl_zip_close(&cz);
            }
            if (off > 0) {
                FILE *fp = fopen(copyp, "r+b");
                if (fp) {
                    fseek(fp, off, SEEK_SET);
                    fputc(0x07, fp);               /* BFINAL with the reserved block type: damage, every time */
                    fclose(fp);
                    tl_zip cz3;
                    if (tl_zip_open(&cz3, copyp, err, sizeof(err))) {
                        const tl_zip_entry *e = tl_zip_find(&cz3, "big.bin");
                        tl_zip_stream *s = e ? tl_zip_stream_open(&cz3, e, 1u << 30, err, sizeof(err)) : NULL;
                        check(s != NULL, "a damaged entry still opens");
                        uint8_t *b = malloc(1 << 16);
                        size_t r = s ? tl_zip_stream_read(s, b, 1 << 16, err, sizeof(err)) : 0;
                        check(s && r == (size_t)-1, "a damaged entry reads as an error");
                        free(b);
                        if (s) tl_zip_stream_close(s);
                        tl_zip_close(&cz3);
                    }
                }
            }
            unlink(copyp);
        }
    }

    /* an entry whose directory promises 4 KiB more than its deflate stream holds: damage, never a quiet short read */
    {
        char copyp[300];
        snprintf(copyp, sizeof(copyp), "%s/lying.apk", dir);
        FILE *src = fopen(apkp, "rb");
        long alen = 0; uint8_t *all = NULL;
        if (src) {
            fseek(src, 0, SEEK_END); alen = ftell(src); fseek(src, 0, SEEK_SET);
            all = malloc((size_t)alen);
            if (!all || fread(all, 1, (size_t)alen, src) != (size_t)alen) { free(all); all = NULL; }
            fclose(src);
        }
        bool lied = false;
        for (long i = 0; all && i + 46 <= alen && !lied; i++) {
            if (memcmp(all + i, "PK\001\002", 4) != 0) continue;              /* a directory entry */
            int nl = all[i + 28] | (all[i + 29] << 8);
            if (nl != 7 || memcmp(all + i + 46, "big.bin", 7) != 0) continue;
            uint32_t u = (uint32_t)all[i + 24] | (uint32_t)all[i + 25] << 8 | (uint32_t)all[i + 26] << 16 | (uint32_t)all[i + 27] << 24;
            u += 4096;
            all[i + 24] = (uint8_t)u; all[i + 25] = (uint8_t)(u >> 8);
            all[i + 26] = (uint8_t)(u >> 16); all[i + 27] = (uint8_t)(u >> 24);
            lied = true;
        }
        if (all && lied) {
            FILE *dst = fopen(copyp, "wb");
            if (dst) { fwrite(all, 1, (size_t)alen, dst); fclose(dst); }
            tl_zip lz;
            if (tl_zip_open(&lz, copyp, err, sizeof(err))) {
                const tl_zip_entry *e = tl_zip_find(&lz, "big.bin");
                check(e && e->usize == (uint64_t)BIG + 4096, "the lying entry reads back as it was written");
                tl_zip_stream *s = e ? tl_zip_stream_open(&lz, e, 1u << 30, err, sizeof(err)) : NULL;
                uint8_t *b = malloc(1 << 16);
                bool errored = false;
                uint64_t at = 0;
                for (;;) {
                    size_t r = s ? tl_zip_stream_read(s, b, 1 << 16, err, sizeof(err)) : (size_t)-1;
                    if (r == (size_t)-1) { errored = true; break; }
                    if (!r) break;
                    at += r;
                }
                check(s && errored && at && at <= BIG, "an entry that runs out early reads as an error");
                free(b);
                if (s) tl_zip_stream_close(s);
                tl_zip_close(&lz);
            }
            unlink(copyp);
        }
        free(all);
    }

    free(truth_big); free(truth_stored); free(truth_tiny);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* leaving the folder behind is fine */ }

    printf(fails ? "%d checks failed\n" : "all zip stream checks passed\n", fails);
    return fails ? 1 : 0;
}
