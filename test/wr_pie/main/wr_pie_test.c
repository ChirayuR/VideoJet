/* On-board oracle for the PIE write-out (see README.md): the library's h264bsdWriteOutputBlocks / h264bsdWriteMacroblock
 * (PIE path, CONFIG_VJ_PIE) against the C code (rec_c.c: the same h264bsd_image.c with PIE off). Whole destination
 * buffers, with 64 guard bytes either side, are compared byte for byte. Output lines start with "WR". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "basetype.h"
#include "h264bsd_image.h"
#include "h264bsd_util.h"

void c_WriteMacroblock(image_t *, u8 *);
void c_WriteOutputBlocks(image_t *, u32, u8 *, i32[][16]);

#define GUARD 64
static unsigned seed = 1;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 16; }

static unsigned long cases, bad;
static int fail_print(const char *what, int w, int h, int mb, int mode, int off, const u8 *a, const u8 *b, size_t n)
{
    size_t i = 0;
    while (a[i] == b[i]) i++;
    if (bad++ < 8)
        printf("WR MISMATCH %s pic %dx%d mb %d mode %d off %d: byte %d (%s) C %d PIE %d\n", what, w, h, mb, mode, off,
               (int)i - GUARD, i < GUARD || i >= n - GUARD ? "GUARD" : "pic", a[i], b[i]);
    return 1;
}

typedef struct { u8 *base; u8 *pic; size_t n; u8 *raw; } buf_t;
static buf_t mk(int w, int h, u32 caps)
{
    buf_t b;
    b.n = (size_t)w * h * 3 / 2 + 2 * GUARD;
    u8 *raw = heap_caps_malloc(b.n + 16, caps);
    if (!raw) { printf("WR FAIL: no memory\n"); vTaskDelete(NULL); }
    b.raw = raw;
    b.base = (u8 *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    b.pic = b.base + GUARD;
    return b;
}

/* residual generation modes */
enum { M_RAND, M_EXTREME, M_PAIR, M_MASK1, M_JUNKEMPTY, M_ALLEMPTY, M_NMODES };
static u8 predA[512] __attribute__((aligned(16)));
static i32 resA[26 * 16 + 4] __attribute__((aligned(16)));

static void gen(int mode, unsigned k, int content)
{
    for (int i = 0; i < 384; i++)
        predA[i] = content == 1 ? ((rnd() & 1) ? 255 : 0) : content == 2 ? 255 : content == 3 ? 0 : (u8)rnd();
    for (int b = 0; b < 26; b++) {
        i32 *r = resA + b * 16;
        int empty = 0;
        for (int i = 0; i < 16; i++) {
            int p = b < 16 ? predA[(i >> 2) * 16 + (i & 3)] : 128; /* only a hint: range below is table-safe for any p */
            (void)p;
            switch (mode) {
            case M_RAND: r[i] = (i32)(rnd() % 1025) - 512; break;
            case M_EXTREME: r[i] = (rnd() & 1) ? -512 : 511; break;
            case M_PAIR: r[i] = (i32)(k % 1024) - 512; break; /* constant: with pred varying over 0..255 */
            default: r[i] = (i32)(rnd() % 1025) - 512; break;
            }
        }
        /* table-safe: pred + res in -512..767 for every pred 0..255: res in -512..512 */
        if (mode == M_MASK1) empty = (b != (int)(k % 24));
        else if (mode == M_ALLEMPTY) empty = 1;
        else if (mode == M_JUNKEMPTY) empty = rnd() & 1;
        else empty = (rnd() % 5) == 0;
        if (empty) r[0] = EMPTY_RESIDUAL_INDICATOR;
    }
}

/* every (pred, residual) pair across the whole table range, every lane: pred constant p, residual constant r */
static void gen_pair(int p, int r)
{
    memset(predA, p, 384);
    for (int b = 0; b < 24; b++)
        for (int i = 0; i < 16; i++)
            resA[b * 16 + i] = r;
}

static void one(int W, int H, u32 mb, int mode, int off, buf_t *A, buf_t *B, int roff)
{
    /* off: shift of the prediction data and residual (0, 4, 8, 12: 4-aligned pointers that still must be handled) */
    static u8 dataA[512] __attribute__((aligned(16)));
    static i32 resB[26 * 16 + 4] __attribute__((aligned(16)));
    memcpy(dataA + off, predA, 384);
    memcpy(resB + roff, resA, sizeof(i32) * 26 * 16);
    /* the picture base moves by 0/4/8/12 B: luma, cb, cr then miss 16 / 8-alignment together (the C path takes them) */
    int poff = (int)(rnd() & 3) * 4;
    image_t ia = {A->pic + poff, (u32)W / 16, (u32)H / 16, 0, 0, 0}, ib = {B->pic + poff, (u32)W / 16, (u32)H / 16, 0, 0, 0};
    memset(A->base, 0xA5, A->n);
    memset(B->base, 0xA5, B->n);
    /* the C path takes the same residual array (it reads, never writes) */
    c_WriteOutputBlocks(&ia, mb, dataA + off, (i32(*)[16])(resB + roff));
    h264bsdWriteOutputBlocks(&ib, mb, dataA + off, (i32(*)[16])(resB + roff));
    cases++;
    if (memcmp(A->base, B->base, A->n)) fail_print("blocks", W, H, (int)mb, mode, off + 100 * roff + 10000 * poff, A->base, B->base, A->n);
}

static void one_mb(int W, int H, u32 mb, int off, buf_t *A, buf_t *B)
{
    static u8 dataA[512] __attribute__((aligned(16)));
    memcpy(dataA + off, predA, 384);
    /* current-MB pointers as the decoder sets them */
    u32 pw = (u32)W / 16, picSize = pw * (u32)H / 16, row = mb / pw, col = mb % pw;
    image_t ia = {A->pic, pw, (u32)H / 16, A->pic + row * pw * 256 + col * 16, A->pic + picSize * 256 + row * pw * 64 + col * 8, 0},
            ib = {B->pic, pw, (u32)H / 16, B->pic + row * pw * 256 + col * 16, B->pic + picSize * 256 + row * pw * 64 + col * 8, 0};
    ia.cr = ia.cb + picSize * 64;
    ib.cr = ib.cb + picSize * 64;
    memset(A->base, 0xA5, A->n);
    memset(B->base, 0xA5, B->n);
    c_WriteMacroblock(&ia, dataA + off);
    h264bsdWriteMacroblock(&ib, dataA + off);
    cases++;
    if (memcmp(A->base, B->base, A->n)) fail_print("mb", W, H, (int)mb, 0, off, A->base, B->base, A->n);
}

static void timing(const char *where, buf_t *B)
{
    static u8 data[384] __attribute__((aligned(16)));
    static i32 res[24][16] __attribute__((aligned(16)));
    image_t img = {B->pic, 3, 2, 0, 0, 0};
    enum { N = 200 };
    for (int i = 0; i < 384; i++) data[i] = (u8)rnd();
    printf("WR TIME %s case C_cyc PIE_cyc (per MB call, mean of %d, MB 0..5 of a 3x2 picture)\n", where, N);
    for (int pct = 0; pct <= 100; pct += 25) {
        for (int b = 0; b < 24; b++) {
            for (int i = 0; i < 16; i++) res[b][i] = (i32)(rnd() % 61) - 30;
            if ((int)(rnd() % 100) >= pct) res[b][0] = EMPTY_RESIDUAL_INDICATOR;
        }
        u32 cyc[2];
        for (int path = 0; path < 2; path++) {
            u32 t0 = esp_cpu_get_cycle_count();
            for (int i = 0; i < N; i++)
                (path ? h264bsdWriteOutputBlocks : c_WriteOutputBlocks)(&img, (u32)(i % 6), data, res);
            cyc[path] = (esp_cpu_get_cycle_count() - t0) / N;
        }
        printf("WR TIME %s blocks-nonempty-%d%% %u %u\n", where, pct, (unsigned)cyc[0], (unsigned)cyc[1]);
    }
    u32 cyc[2];
    for (int path = 0; path < 2; path++) {
        u32 t0 = esp_cpu_get_cycle_count();
        for (int i = 0; i < N; i++) {
            u32 mb = (u32)(i % 6), r = mb / 3, c = mb % 3;
            image_t im = {B->pic, 3, 2, B->pic + r * 3 * 256 + c * 16, B->pic + 6 * 256 + r * 3 * 64 + c * 8, 0};
            im.cr = im.cb + 6 * 64;
            (path ? h264bsdWriteMacroblock : c_WriteMacroblock)(&im, data);
        }
        cyc[path] = (esp_cpu_get_cycle_count() - t0) / N;
    }
    printf("WR TIME %s macroblock %u %u\n", where, (unsigned)cyc[0], (unsigned)cyc[1]);
}

static void run(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    static const int P[3][2] = {{16, 16}, {48, 32}, {64, 16}};
    for (int caps = 0; caps < 2; caps++)
        for (int p = 0; p < 3; p++) {
            int W = P[p][0], H = P[p][1];
            u32 c = caps ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
            buf_t A = mk(W, H, c), B = mk(W, H, c);
            u32 nmb = (u32)(W / 16 * (H / 16));
            /* modes x content x every MB x pointer offsets (data 0/4/8/12, residual 0/4/8/12 B off 16-alignment) */
            for (int mode = 0; mode < M_NMODES; mode++)
                for (int content = 0; content < 4; content++)
                    for (unsigned k = 0; k < (mode == M_MASK1 ? 24u : 6u); k++) {
                        gen(mode, k * 97 + content, content);
                        for (u32 mb = 0; mb < nmb; mb++)
                            for (int off = 0; off < 16; off += 4)
                                one(W, H, mb, mode, off, &A, &B, (int)(rnd() & 3));
                        vTaskDelay(1);
                    }
            /* every (pred, residual) pair, every lane, all 24 blocks non-empty */
            if (p < 2)
                for (int pv = 0; pv < 256; pv++) {
                    for (int r = -512; r <= 767 - pv; r++) {
                        gen_pair(pv, r);
                        one(W, H, (u32)(rnd() % nmb), M_PAIR, 0, &A, &B, 0);
                    }
                    vTaskDelay(1);
                }
            /* skipped-MB copy: data and picture pointers */
            for (int content = 0; content < 4; content++) {
                gen(M_RAND, 1, content);
                for (u32 mb = 0; mb < nmb; mb++)
                    for (int off = 0; off < 16; off += 4)
                        one_mb(W, H, mb, off, &A, &B);
            }
            printf("WR pic %dx%d %s: %lu cases so far, %lu mismatches (%lld s)\n", W, H, caps ? "psram" : "iram", cases,
                   bad, (esp_timer_get_time() - t0) / 1000000);
            heap_caps_free(A.raw); heap_caps_free(B.raw);
        }
    printf("WR RESULT %lu cases, %lu mismatches PIE vs C\n", cases, bad);
    printf("WR %s\n", bad ? "FAIL" : "PASS");
    buf_t T = mk(48, 32, MALLOC_CAP_INTERNAL);
    timing("iram", &T);
    heap_caps_free(T.raw);
    T = mk(48, 32, MALLOC_CAP_SPIRAM);
    timing("psram", &T);
    printf("WR done\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    vTaskDelay(10);
    printf("WR start\n");
    xTaskCreatePinnedToCore(run, "wr", 16384, NULL, 5, NULL, 0);
}
