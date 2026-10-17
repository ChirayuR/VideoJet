/* test_hold STREAMS_DIR [DIR_OR_FILE...]: vj_decoder_set_hold(). Every *.h264 under the arguments (failing/ skipped) and a
 * chain of streams across picture-size changes (DPB resets), each run three ways: whole AUs (the picture numbers jump to
 * the restart point every 7th frame, as in test_api), one NAL per call (pictures finish in a later call than they start),
 * and damaged as fz.c damages (HOLD_SEEDS seeds, default 2, about 1 AU in 5). Two decoders take every call in lockstep,
 * hold off (A) and hold on (B):
 *  - B's last frame (its planes and changed_mb, copied when returned) is unchanged after every later call up to and
 *    including the one that returns B's next frame;
 *  - the same status every call; for a frame the same fields and the same pixels, the whole coded frame. The one
 *    exception videojet.h documents: a picture decoded under a PPS with redundant slices may show, where no slice decoded
 *    it, what its buffer held before (vectors/attack.h264), and the frames predicted from it follow until an IDR;
 *  - B's changed_mb marks every macroblock A's marks (a picture decoded into the extra buffer while that holds no known
 *    picture marks them all), and every macroblock it calls unchanged equals the one in B's previous frame;
 *  - clean runs match the .golden, if any.
 * And it is not vacuous: A decodes into the frame it has just returned (non-reference pictures) and B keeps a frame through
 * a DPB reset. Failures name the stream, mode, seed and call. ASan sees a frame freed while held. */
#undef NDEBUG
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "videojet.h"
#include "vj_hash.h"
#include "h264bsd_storage.h" /* the restart point, the active PPS, and the frame a reset spared (dpb->vjOrphan) */

/* A damaged SPS can ask for gigabytes. On the board that allocation fails; here too (the binary is linked with
 * -Wl,--wrap=malloc), and for both decoders alike: every allocation of the two is the same size or fails the same way. */
void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n)
{
    return n > (8u << 20) ? NULL : __real_malloc(n);
}

static struct {
    unsigned long runs, calls, frames, reuse_a, orphan_calls, diverged, more_changed;
} st;

/* what is running, for a failure report */
static struct {
    const char *name;
    int mode;
    unsigned seed;
    unsigned long call;
} at;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "test_hold: FAIL %s (line %d): %s, mode %d seed %u call %lu\n", #c, __LINE__, at.name,       \
                    at.mode, at.seed, at.call);                                                                        \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)

typedef struct {
    const uint8_t *y, *c; /* B's last frame, NULL before the first */
    unsigned w, h;
    uint8_t *pix, *chg; /* its planes and changed_mb when it was returned */
} snap_t;

typedef struct {
    vj_decoder_t *a, *b;
    snap_t held;
    const uint8_t *last_a; /* A's last frame */
    const unsigned *gold;
    unsigned ngold, frames, restart;
    int diverged; /* the redundant-slice exception happened: B's pixels may differ from A's until an IDR */
} run_t;

static void hold_check(const snap_t *s)
{
    if (s->y)
        CHECK(!memcmp(s->y, s->pix, (size_t)s->w * s->h * 3 / 2) && !memcmp(s->c, s->chg, (size_t)s->w * s->h / 256));
}

/* B's frame f against B's previous frame p: every macroblock called unchanged is bit-identical there */
static void changed_contract(const snap_t *p, const vj_frame_t *f)
{
    unsigned w = f->width / 16, h = f->height / 16;
    size_t ysz = (size_t)f->width * f->height;
    for (unsigned i = 0; i < w * h; i++) {
        if (f->changed_mb[i])
            continue;
        CHECK(p->y && p->w == f->width && p->h == f->height);
        unsigned x = i % w, y = i / w;
        for (unsigned r = 0; r < 16; r++)
            CHECK(!memcmp(f->y + (y * 16 + r) * f->y_stride + x * 16, p->pix + (y * 16 + r) * f->width + x * 16, 16));
        for (unsigned r = 0; r < 8; r++) {
            size_t o = (y * 8 + r) * (size_t)f->uv_stride + x * 8;
            CHECK(!memcmp(f->u + o, p->pix + ysz + o, 8) && !memcmp(f->v + o, p->pix + ysz + ysz / 4 + o, 8));
        }
    }
}

static void compare(run_t *r, const vj_frame_t *a, const vj_frame_t *b)
{
    CHECK(a->width == b->width && a->height == b->height && a->y_stride == b->y_stride &&
          a->uv_stride == b->uv_stride && a->crop_x == b->crop_x && a->crop_y == b->crop_y && a->crop_w == b->crop_w &&
          a->crop_h == b->crop_h && a->full_range == b->full_range && a->matrix == b->matrix && a->idr == b->idr &&
          a->error_mbs == b->error_mbs);
    size_t ysz = (size_t)b->width * b->height, nc = ysz / 256;
    CHECK(b->y_stride == b->width && b->uv_stride == b->width / 2);
    CHECK(a->u == a->y + ysz && a->v == a->u + ysz / 4 && b->u == b->y + ysz && b->v == b->u + ysz / 4);
    if (memcmp(a->y, b->y, ysz * 3 / 2)) {
        const picParamSet_t *pps = ((storage_t *)(void *)r->b)->activePps;
        CHECK(r->diverged || (pps && pps->redundantPicCntPresentFlag));
        r->diverged = 1;
        st.diverged++;
    } else if (b->idr) {
        r->diverged = 0;
    }
    CHECK(a->changed_mb && b->changed_mb);
    for (size_t i = 0; i < nc; i++)
        CHECK(b->changed_mb[i] >= a->changed_mb[i] && b->changed_mb[i] <= 1);
    st.more_changed += memcmp(a->changed_mb, b->changed_mb, nc) != 0;
}

static void feed(run_t *r, const uint8_t *src, size_t n)
{
    if (!n)
        return;
    at.call++;
    uint8_t *ca = malloc(n), *cb = malloc(n);
    assert(ca && cb);
    memcpy(ca, src, n); /* a call rewrites its AU in place */
    memcpy(cb, src, n);
    vj_frame_t fa, fb;
    vj_status_t sa = vj_decode_au(r->a, ca, n, &fa), sb = vj_decode_au(r->b, cb, n, &fb);
    free(ca);
    free(cb);
    st.calls++;
    CHECK(sa == sb);
    hold_check(&r->held);
    st.orphan_calls += ((storage_t *)(void *)r->b)->dpb->vjOrphan != NULL;
    if (sb != VJ_FRAME)
        return;
    compare(r, &fa, &fb);
    changed_contract(&r->held, &fb);
    if (r->gold)
        CHECK(r->frames < r->ngold && vj_hash_i420(fb.y, fb.y_stride, fb.u, fb.v, fb.uv_stride, fb.crop_x, fb.crop_y,
                                                   fb.crop_w, fb.crop_h) == r->gold[r->frames]);
    st.reuse_a += fa.y == r->last_a;
    r->last_a = fa.y;
    snap_t *s = &r->held;
    s->w = fb.width;
    s->h = fb.height;
    s->pix = realloc(s->pix, (size_t)s->w * s->h * 3 / 2);
    s->chg = realloc(s->chg, (size_t)s->w * s->h / 256);
    assert(s->pix && s->chg);
    memcpy(s->pix, fb.y, (size_t)s->w * s->h * 3 / 2);
    memcpy(s->chg, fb.changed_mb, (size_t)s->w * s->h / 256);
    s->y = fb.y;
    s->c = fb.changed_mb;
    st.frames++;
    if (++r->frames % 7 == 0 && r->restart) /* both decoders: the next pictures renumber from 1 */
        ((storage_t *)(void *)r->a)->dpb->vjPicIdx = ((storage_t *)(void *)r->b)->dpb->vjPicIdx = VJ_IDX_NEVER - 2;
}

static uint64_t rs;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

/* fz.c's damage, 1 AU in 5: truncate, flip a bit, drop, flip + resend, cut + resend, duplicate, flip a header bit, split
 * in two calls, burst + resend */
static void damage(run_t *r, const uint8_t *a, size_t au)
{
    uint8_t *t = malloc(au);
    assert(t);
    memcpy(t, a, au);
    unsigned op = (rnd() % 5) ? 0 : 1 + rnd() % 9;
    size_t cut = au > 8 ? 5 + rnd() % (au - 5) : au;
    size_t bit = au > 6 ? (5 + rnd() % (au - 5)) * 8 + rnd() % 8 : 0;
    switch (op) {
    case 0: feed(r, a, au); break;
    case 1: feed(r, a, cut); break;
    case 2: t[bit / 8] ^= 1 << (bit % 8); feed(r, t, au); break;
    case 3: break;
    case 4: t[bit / 8] ^= 1 << (bit % 8); feed(r, t, au); feed(r, a, au); break;
    case 5: feed(r, a, cut); feed(r, a, au); break;
    case 6: feed(r, a, au); feed(r, a, au); break;
    case 7: if (au > 12) t[5 + rnd() % 8] ^= 1 << (rnd() % 8); feed(r, t, au); break;
    case 8: feed(r, a, cut); feed(r, a + cut, au - cut); break;
    case 9:
        for (int k = 0; k < 4; k++) {
            size_t b2 = 5 + rnd() % (au > 6 ? au - 5 : 1);
            if (b2 < au)
                t[b2] = (uint8_t)rnd();
        }
        feed(r, t, au);
        feed(r, a, au);
        break;
    }
    free(t);
}

/* Bytes from the start code at s to the next one: one NAL unit */
static size_t nal_length(const uint8_t *s, size_t len)
{
    for (size_t i = 3; i + 3 <= len; i++)
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)
            return s[i - 1] == 0 ? i - 1 : i;
    return len;
}

enum { WHOLE, NAL, DAMAGED };

static void run(const uint8_t *s, size_t len, int mode, unsigned seed, const unsigned *gold, unsigned ngold)
{
    run_t r = {.gold = mode == DAMAGED ? NULL : gold, .ngold = ngold, .restart = mode == WHOLE};
    at.mode = mode;
    at.seed = seed;
    at.call = 0;
    r.a = vj_decoder_create();
    r.b = vj_decoder_create();
    assert(r.a && r.b && vj_decoder_set_hold(r.b, true));
    rs = 0x9E3779B97F4A7C15ULL ^ (uint64_t)seed * 0x2545F4914F6CDD1DULL;
    for (size_t o = 0, n; o < len; o += n) {
        n = mode == NAL ? nal_length(s + o, len - o) : vj_au_length(s + o, len - o);
        if (!n)
            break;
        if (mode == DAMAGED)
            damage(&r, s + o, n);
        else
            feed(&r, s + o, n);
    }
    CHECK(!r.gold || r.frames == ngold);
    vj_decoder_destroy(r.a);
    vj_decoder_destroy(r.b); /* with a frame held, and maybe one spared by a reset: ASan's leak check */
    free(r.held.pix);
    free(r.held.chg);
    st.runs++;
}

static void run_all(const char *name, const uint8_t *s, size_t len, const unsigned *gold, unsigned ngold, unsigned seeds)
{
    at.name = name;
    run(s, len, WHOLE, 0, gold, ngold);
    run(s, len, NAL, 0, gold, ngold);
    for (unsigned k = 1; k <= seeds; k++)
        run(s, len, DAMAGED, k, NULL, 0);
    printf("ok   %s\n", name);
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len ? *len : 1);
    assert(b && fread(b, 1, *len, f) == *len);
    fclose(f);
    return b;
}

/* X.golden next to X.h264: one hash per frame, appended at gold[*n]. False if there is none */
static int load_gold(const char *h264, unsigned **gold, unsigned *n)
{
    char path[1024];
    snprintf(path, sizeof path, "%.*s.golden", (int)(strlen(h264) - 5), h264);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    unsigned h;
    while (fscanf(f, "%x", &h) == 1) {
        *gold = realloc(*gold, (*n + 1) * sizeof **gold);
        assert(*gold);
        (*gold)[(*n)++] = h;
    }
    fclose(f);
    return 1;
}

static char **files;
static size_t nfiles;

static void collect(const char *path)
{
    size_t l = strlen(path);
    if (l > 5 && !strcmp(path + l - 5, ".h264")) {
        files = realloc(files, (nfiles + 1) * sizeof *files);
        assert(files);
        files[nfiles++] = strdup(path);
        return;
    }
    DIR *d = opendir(path);
    if (!d)
        return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "failing"))
            continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        collect(sub);
    }
    closedir(d);
}

static int by_name(const void *x, const void *y)
{
    return strcmp(*(char *const *)x, *(char *const *)y);
}

/* The setting is fixed by the first vj_decode_au() call, whatever it returns */
static void test_rules(void)
{
    static uint8_t junk[] = {0, 0, 1, 0x67};
    vj_frame_t f;
    assert(!vj_decoder_set_hold(NULL, true));
    vj_decoder_t *d = vj_decoder_create();
    assert(vj_decoder_set_hold(d, true) && vj_decoder_set_hold(d, false) && vj_decoder_set_hold(d, true));
    assert(vj_decode_au(d, NULL, 4, &f) == VJ_ERR_ARG);
    assert(!vj_decoder_set_hold(d, false) && !vj_decoder_set_hold(d, true));
    assert(vj_decode_au(d, junk, sizeof junk, &f) != VJ_FRAME);
    assert(!vj_decoder_set_hold(d, false));
    vj_decoder_destroy(d);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../streams";
    const char *env = getenv("HOLD_SEEDS");
    unsigned seeds = env ? (unsigned)atoi(env) : 2;
    test_rules();
    for (int i = 1; i < argc; i++)
        collect(argv[i]);
    if (argc < 2)
        collect(dir);
    qsort(files, nfiles, sizeof *files, by_name);
    for (size_t i = 0; i < nfiles; i++) {
        size_t len;
        uint8_t *s = slurp(files[i], &len);
        unsigned *gold = NULL, ng = 0;
        int has = load_gold(files[i], &gold, &ng);
        assert(s);
        run_all(files[i], s, len, has ? gold : NULL, ng, seeds);
        free(gold);
        free(s);
        free(files[i]);
    }
    free(files);

    /* Picture-size changes: each stream's SPS replaces the active one, so its IDR resets the DPB with B's last frame
     * held. 800x480, QCIF (one reference), 800x480 in 4 slices (one NAL per call: the reset call ends inside the
     * picture, so the spared frame must outlive more calls), 640x368 coded, QCIF with non-reference pictures (three
     * references), 800x480 */
    static const char *const chain[] = {"search/aux_card", "heldout/BA_MW_D", "heldout/aux_map_dark",
                                        "heldout/test_640x360", "heldout/NRF_MW_E", "search/aux_card"};
    uint8_t *cat = NULL;
    size_t clen = 0;
    unsigned *gold = NULL, ng = 0;
    for (size_t i = 0; i < sizeof chain / sizeof *chain; i++) {
        char path[1024];
        size_t len;
        snprintf(path, sizeof path, "%s/%s.h264", dir, chain[i]);
        uint8_t *s = slurp(path, &len);
        assert(s && load_gold(path, &gold, &ng));
        cat = realloc(cat, clen + len);
        assert(cat);
        memcpy(cat + clen, s, len);
        clen += len;
        free(s);
    }
    run_all("chain aux_card + BA_MW_D + aux_map_dark + test_640x360 + NRF_MW_E + aux_card", cat, clen, gold, ng, seeds);
    free(cat);
    free(gold);

    printf("test_hold: %lu runs, %lu calls, %lu frames; hold off decoded into the frame it had just returned %lu times; a "
           "frame outlived a reset in %lu calls; changed_mb marked more %lu times; redundant-slice pixels differed in %lu "
           "frames\n", st.runs, st.calls, st.frames, st.reuse_a, st.orphan_calls, st.more_changed, st.diverged);
    assert(st.reuse_a > 0 && st.orphan_calls > 0);
    puts("test_hold: all passed");
    return 0;
}
