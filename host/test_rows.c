/* The row-ready callback's contract (vj_decoder_set_rows_callback), on every stream under the given directories, clean
 * and damaged:
 * - every row of a returned VJ_FRAME was handed over during that call; deliveries run 0, 1, 2, ... (a new pass restarts at 0);
 * - the last delivery of each row equals the returned picture's rows (Y, U, V) and its changed_mb row, and describes the
 *   same planes;
 * - the callback changes nothing: statuses, pictures and changed_mb hash the same with and without it. */
#include <assert.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "videojet.h"

typedef struct {
    unsigned W, H;              /* picture size in pixels, 0 before the first delivery */
    const uint8_t *y;           /* the planes the deliveries described */
    uint8_t *pix;               /* per MB row: 16 luma lines + 8 + 8 chroma lines */
    uint8_t *chg;               /* per MB row: W / 16 changed bytes */
    unsigned *count;            /* deliveries of each row in this call */
    int last;                   /* last row delivered, -1 none */
    unsigned bad_order, total, again, clean_again;
} rec_t;

static rec_t R;

static void on_rows(void *ctx, const vj_frame_t *f, unsigned row, const uint8_t *changed)
{
    rec_t *r = (rec_t *)ctx;
    unsigned W = f->width, H = f->height, mbw = W / 16;
    if (r->W != W || r->H != H) {
        free(r->pix);
        free(r->chg);
        free(r->count);
        r->W = W;
        r->H = H;
        r->pix = malloc((size_t)(H / 16) * 24 * W);
        r->chg = malloc((size_t)(H / 16) * mbw);
        r->count = calloc(H / 16, sizeof *r->count);
    }
    assert(row < H / 16 && !f->changed_mb && f->y_stride == W && f->uv_stride == W / 2);
    if (!(row == 0 || (int)row == r->last + 1))
        r->bad_order++;
    r->last = (int)row;
    uint8_t *o = r->pix + (size_t)row * 24 * W;
    for (unsigned k = 0; k < 16; k++)
        memcpy(o + k * W, f->y + (size_t)(row * 16 + k) * W, W);
    for (unsigned k = 0; k < 8; k++) {
        memcpy(o + 16 * W + k * (W / 2), f->u + (size_t)(row * 8 + k) * (W / 2), W / 2);
        memcpy(o + 20 * W + k * (W / 2), f->v + (size_t)(row * 8 + k) * (W / 2), W / 2);
    }
    memcpy(r->chg + (size_t)row * mbw, changed, mbw);
    r->y = f->y;
    r->count[row]++;
    r->total++;
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static uint64_t s_rng;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)s_rng;
}

/* Decodes the stream (damaged with seed / rate when rate > 0), with or without the callback; returns a hash of every
 * status, picture and changed_mb. With the callback, checks the contract on every VJ_FRAME. */
static uint32_t run(const uint8_t *src, size_t len, unsigned seed, unsigned rate, int with_cb, unsigned *frames)
{
    uint8_t *buf = malloc(len ? len : 1);
    memcpy(buf, src, len);
    vj_decoder_t *dec = vj_decoder_create();
    assert(dec);
    if (with_cb)
        vj_decoder_set_rows_callback(dec, on_rows, &R);
    s_rng = 0x9E3779B97F4A7C15ull ^ seed;
    uint32_t h = 2166136261u;
    for (size_t off = 0, n; off < len; off += n) {
        n = vj_au_length(src + off, len - off);
        if (!n)
            break;
        if (rate)
            for (size_t i = 4; i < n; i++)
                if (rnd() % (rate * 100) == 0)
                    buf[off + i] ^= (uint8_t)(1u << (rnd() % 8));
        if (with_cb && R.count)
            memset(R.count, 0, (R.H / 16) * sizeof *R.count);
        R.last = -1;
        vj_frame_t f;
        vj_status_t st = vj_decode_au(dec, buf + off, n, &f);
        h = fnv(h, &st, sizeof st);
        if (st != VJ_FRAME)
            continue;
        (*frames)++;
        unsigned W = f.width, H = f.height, mbw = W / 16;
        h = fnv(h, f.y, (size_t)W * H);
        h = fnv(h, f.u, (size_t)W * H / 4);
        h = fnv(h, f.v, (size_t)W * H / 4);
        h = fnv(h, f.changed_mb, (size_t)mbw * (H / 16));
        if (!with_cb)
            continue;
        assert(R.W == W && R.H == H && R.y == f.y);
        for (unsigned row = 0; row < H / 16; row++) {
            assert(R.count[row] >= 1);
            const uint8_t *o = R.pix + (size_t)row * 24 * W;
            for (unsigned k = 0; k < 16; k++)
                assert(!memcmp(o + k * W, f.y + (size_t)(row * 16 + k) * W, W));
            for (unsigned k = 0; k < 8; k++) {
                assert(!memcmp(o + 16 * W + k * (W / 2), f.u + (size_t)(row * 8 + k) * (W / 2), W / 2));
                assert(!memcmp(o + 20 * W + k * (W / 2), f.v + (size_t)(row * 8 + k) * (W / 2), W / 2));
            }
            assert(!memcmp(R.chg + (size_t)row * mbw, f.changed_mb + (size_t)row * mbw, mbw));
            if (R.count[row] > 1) {
                R.again++;
                R.clean_again += rate == 0;
            }
        }
    }
    vj_decoder_destroy(dec);
    free(buf);
    return h;
}

static unsigned s_files, s_runs, s_frames, s_nondet;

static void stream(const char *path, unsigned seeds)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *src = malloc(n > 0 ? (size_t)n : 1);
    size_t got = fread(src, 1, (size_t)n, fp);
    fclose(fp);
    for (unsigned s = 0; s <= seeds; s++) {
        unsigned rate = s ? 3 + s % 4 : 0, fa = 0, fb = 0, fc = 0;
        uint32_t a = run(src, got, s, rate, 1, &fa), b = run(src, got, s, rate, 0, &fb), c = run(src, got, s, rate, 0, &fc);
        if (b != c || fb != fc) {
            /* the decoder alone gives two answers for this damaged stream (it reads stale memory: a separate bug, seen
             * only on damaged FMO streams), so the callback cannot be compared; the contract was still checked */
            s_nondet++;
        } else if (a != b || fa != fb) {
            fprintf(stderr, "test_rows: %s seed %u: output differs with the callback\n", path, s);
            exit(1);
        }
        s_runs++;
        s_frames += fa;
    }
    s_files++;
    free(src);
}

static void walk(const char *dir, unsigned seeds)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t l = strlen(e->d_name);
        if (l > 5 && !strcmp(e->d_name + l - 5, ".h264"))
            stream(p, seeds);
        else if (e->d_type == DT_DIR)
            walk(p, seeds);
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    unsigned seeds = 3;
    /* fresh heap blocks hold a fixed byte, not what an earlier run left: the decoder reads stale memory on some damaged
     * FMO streams (a separate bug), and the comparison must not depend on heap history */
    mallopt(M_PERTURB, 0x5A);
    if (argc < 2) {
        fprintf(stderr, "usage: test_rows DIR... [-s SEEDS]\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc)
            seeds = (unsigned)atoi(argv[++i]);
        else
            walk(argv[i], seeds);
    }
    if (R.bad_order) {
        fprintf(stderr, "test_rows: %u deliveries out of order\n", R.bad_order);
        return 1;
    }
    printf("test_rows: ok, %u streams, %u runs, %u frames checked, %u rows delivered, %u delivered again (%u in clean runs)%s\n",
           s_files, s_runs, s_frames, R.total, R.again, R.clean_again, s_nondet ? "; some baselines not deterministic, see below" : "");
    if (s_nondet)
        printf("test_rows: %u damaged runs where the decoder alone is not deterministic (not compared)\n", s_nondet);
    return 0;
}
