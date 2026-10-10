/* On-board oracle for the PIE motion compensation (see README.md): the library's h264bsdPredictSamples (PIE path,
 * CONFIG_VJ_PIE) against the C kernels (rec_c.c: the same reconstruct.c with PIE off) over host/test_mc.c's case
 * space, plus a larger picture where most windows lie inside (the PIE path), plus the standard's equations on a sample
 * of it. Then cycles per call, C vs PIE, per position and partition size, with the reference in internal RAM and in
 * PSRAM. Output lines start with "MC". Pictures are allocated like the DPB: 16-aligned data and 32 bytes of slack
 * after Cr (the kernels' over-read relies on it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "basetype.h"
#include "h264bsd_reconstruct.h"

void c_PredictSamples(u8 *, mv_t *, image_t *, u32, u32, u32, u32, u32, u32);

#define PAD 128
static int W, H;
static u8 *pic, *picAlloc;
static u8 *ext[3];

static void alloc_pic(int w, int h, u32 caps)
{
    W = w; H = h;
    picAlloc = heap_caps_malloc((size_t)W * H * 3 / 2 + 32 + 15, caps);
    if (!picAlloc) {
        printf("MC FAIL: no memory for a %dx%d picture\n", W, H);
        vTaskDelete(NULL);
    }
    pic = (u8 *)(((uintptr_t)picAlloc + 15) & ~(uintptr_t)15);
}

static void free_pic(void) { heap_caps_free(picAlloc); }

static unsigned seed = 1;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 16; }

static void fill(int content)
{
    for (int i = 0; i < W * H * 3 / 2; i++)
        pic[i] = content ? ((rnd() & 1) ? 255 : 0) : (u8)rnd();
}

/* ---- the standard's equations, clamped coordinates (as host/test_mc.c) ---- */
static void pad_planes(void)
{
    for (int c = 0; c < 3; c++) {
        int w = c ? W / 2 : W, h = c ? H / 2 : H, ew = w + 2 * PAD;
        const u8 *p = pic + (c == 0 ? 0 : c == 1 ? W * H : W * H + W * H / 4);
        free(ext[c]);
        ext[c] = malloc((size_t)ew * (h + 2 * PAD));
        for (int y = 0; y < h + 2 * PAD; y++)
            for (int x = 0; x < ew; x++) {
                int cx = x - PAD < 0 ? 0 : x - PAD >= w ? w - 1 : x - PAD;
                int cy = y - PAD < 0 ? 0 : y - PAD >= h ? h - 1 : y - PAD;
                ext[c][y * ew + x] = p[cy * w + cx];
            }
    }
}
static int px(int c, int x, int y) { int w = c ? W / 2 : W; return ext[c][(y + PAD) * (w + 2 * PAD) + x + PAD]; }
static int tap(int a, int b, int c, int d, int e, int f) { return a - 5 * b + 20 * c + 20 * d - 5 * e + f; }
static int clip1(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int b1(int x, int y) { return tap(px(0, x - 2, y), px(0, x - 1, y), px(0, x, y), px(0, x + 1, y), px(0, x + 2, y), px(0, x + 3, y)); }
static int h1(int x, int y) { return tap(px(0, x, y - 2), px(0, x, y - 1), px(0, x, y), px(0, x, y + 1), px(0, x, y + 2), px(0, x, y + 3)); }
static int luma(int x, int y, int xf, int yf)
{
    int G = px(0, x, y), H_ = px(0, x + 1, y), M = px(0, x, y + 1);
    int b = clip1((b1(x, y) + 16) >> 5), s = clip1((b1(x, y + 1) + 16) >> 5);
    int h = clip1((h1(x, y) + 16) >> 5), m = clip1((h1(x + 1, y) + 16) >> 5);
    int j = 0;
    if ((xf == 2 && yf) || (yf == 2 && xf))
        j = clip1((tap(b1(x, y - 2), b1(x, y - 1), b1(x, y), b1(x, y + 1), b1(x, y + 2), b1(x, y + 3)) + 512) >> 10);
    switch (4 * xf + yf) {
    case 0: return G; case 1: return (G + h + 1) >> 1; case 2: return h; case 3: return (M + h + 1) >> 1;
    case 4: return (G + b + 1) >> 1; case 5: return (b + h + 1) >> 1; case 6: return (h + j + 1) >> 1;
    case 7: return (h + s + 1) >> 1; case 8: return b; case 9: return (b + j + 1) >> 1; case 10: return j;
    case 11: return (j + s + 1) >> 1; case 12: return (H_ + b + 1) >> 1; case 13: return (b + m + 1) >> 1;
    case 14: return (j + m + 1) >> 1; default: return (m + s + 1) >> 1;
    }
}
static int chroma(int c, int x, int y, int xf, int yf)
{
    return ((8 - xf) * (8 - yf) * px(c, x, y) + xf * (8 - yf) * px(c, x + 1, y) + (8 - xf) * yf * px(c, x, y + 1) +
            xf * yf * px(c, x + 1, y + 1) + 32) >> 6;
}

static unsigned long cases, bad, badSpec, specCases;
static int checkSpec;

static void one(int xA, int yA, int px0, int py0, int w, int h, int vx, int vy)
{
    static u8 a[384] __attribute__((aligned(16))), b[384] __attribute__((aligned(16)));
    image_t img = {pic, (u32)W / 16, (u32)H / 16, 0, 0, 0};
    mv_t mv = {(i16)vx, (i16)vy};
    memset(a, 0xA5, sizeof a);
    memset(b, 0xA5, sizeof b);
    c_PredictSamples(a, &mv, &img, (u32)xA, (u32)yA, (u32)px0, (u32)py0, (u32)w, (u32)h);
    h264bsdPredictSamples(b, &mv, &img, (u32)xA, (u32)yA, (u32)px0, (u32)py0, (u32)w, (u32)h);
    cases++;
    if (memcmp(a, b, sizeof a) && bad++ < 8) {
        int i = 0;
        while (a[i] == b[i]) i++;
        printf("MC MISMATCH pic %dx%d mb (%d,%d) part (%d,%d) %dx%d mv (%d,%d): byte %d C %d PIE %d\n", W, H, xA, yA,
               px0, py0, w, h, vx, vy, i, a[i], b[i]);
    }
    if (checkSpec && (cases & 63) == 0) {
        int lx = xA + px0 + (vx >> 2), ly = yA + py0 + (vy >> 2), cx = (xA + px0) / 2 + (vx >> 3), cy = (yA + py0) / 2 + (vy >> 3);
        int ok = 1;
        for (int y = 0; y < h && ok; y++)
            for (int x = 0; x < w; x++)
                if (b[(py0 + y) * 16 + px0 + x] != luma(lx + x, ly + y, vx & 3, vy & 3)) { ok = 0; break; }
        for (int c = 1; c < 3 && ok; c++)
            for (int y = 0; y < h / 2 && ok; y++)
                for (int x = 0; x < w / 2; x++)
                    if (b[256 + 64 * (c - 1) + (py0 / 2 + y) * 8 + px0 / 2 + x] != chroma(c, cx + x, cy + y, vx & 7, vy & 7)) { ok = 0; break; }
        specCases++;
        if (!ok && badSpec++ < 4)
            printf("MC SPEC MISMATCH pic %dx%d mb (%d,%d) part (%d,%d) %dx%d mv (%d,%d)\n", W, H, xA, yA, px0, py0, w, h, vx, vy);
    }
}

static void sweep(int xA, int yA, int px0, int py0, int w, int h, int mh0, int mh1, int mv0, int mv1)
{
    for (int vy = mv0; vy <= mv1; vy++)
        for (int vx = mh0; vx <= mh1; vx++)
            one(xA, yA, px0, py0, w, h, vx, vy);
}

static const int S[7][2] = {{16, 16}, {16, 8}, {8, 16}, {8, 8}, {8, 4}, {4, 8}, {4, 4}};

/* host/test_mc.c's sweep: every partition at every offset, integer positions from fully outside one edge to fully
 * outside the other, far outside sides and corners */
static void picture(int xA, int yA)
{
    for (int k = 0; k < 7; k++) {
        int w = S[k][0], h = S[k][1];
        for (int py0 = 0; py0 < 16; py0 += h)
            for (int px0 = 0; px0 < 16; px0 += w) {
                int bx = xA + px0, by = yA + py0;
                vTaskDelay(1); /* let IDLE feed the task watchdog */
                int dx0 = -(w + 8) - bx, dx1 = W + 8 - bx, dy0 = -(h + 8) - by, dy1 = H + 8 - by;
                sweep(xA, yA, px0, py0, w, h, 4 * dx0, 4 * dx1 + 3, 4 * dy0, 4 * dy1 + 3);
                int fx[2] = {-100 - bx, W + 90 - bx}, fy[2] = {-100 - by, H + 90 - by};
                for (int i = 0; i < 2; i++) {
                    sweep(xA, yA, px0, py0, w, h, 4 * fx[i], 4 * fx[i] + 7, 4 * dy0, 4 * dy1 + 3);
                    sweep(xA, yA, px0, py0, w, h, 4 * dx0, 4 * dx1 + 3, 4 * fy[i], 4 * fy[i] + 7);
                    for (int j = 0; j < 2; j++)
                        sweep(xA, yA, px0, py0, w, h, 4 * fx[i], 4 * fx[i] + 7, 4 * fy[j], 4 * fy[j] + 7);
                }
            }
    }
}

/* cycles per call, C vs PIE, by position (16) and size (16x16, 16x8, 8x16, 8x8): N random inside blocks */
static void timing(const char *where)
{
    enum { N = 400 };
    static int bx[N], by[N];
    static u8 out[384] __attribute__((aligned(16)));
    image_t img = {pic, (u32)W / 16, (u32)H / 16, 0, 0, 0};
    printf("MC TIME %s pos size C_cyc PIE_cyc (per call, %d random inside blocks)\n", where, N);
    for (int k = 0; k < 4; k++) {
        int w = S[k][0], h = S[k][1];
        for (int i = 0; i < N; i++) { /* window fully inside: room for 2 before and 3 after */
            bx[i] = 16 + (int)(rnd() % (unsigned)(W - 32 - w));
            by[i] = 16 + (int)(rnd() % (unsigned)(H - 32 - h));
        }
        for (int pos = 0; pos < 16; pos++) {
            u32 cyc[2];
            for (int path = 0; path < 2; path++) {
                u32 t0 = esp_cpu_get_cycle_count();
                for (int i = 0; i < N; i++) {
                    mv_t mv = {(i16)(4 * bx[i] + (pos >> 2)), (i16)(4 * by[i] + (pos & 3))};
                    (path ? h264bsdPredictSamples : c_PredictSamples)(out, &mv, &img, 0, 0, 0, 0, (u32)w, (u32)h);
                }
                cyc[path] = (esp_cpu_get_cycle_count() - t0) / N;
            }
            printf("MC TIME %s %c %dx%d %u %u\n", where, "GdhnaeipbfjqcgkrX"[pos], w, h, (unsigned)cyc[0], (unsigned)cyc[1]);
        }
    }
}

static void run(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    /* host/test_mc.c's case space: 3x2 and 1x1 macroblock pictures, random and random 0/255 content */
    static const int P[3][4] = {{3, 2, 16, 16}, {1, 1, 0, 0}, {6, 4, 32, 16}};
    for (int p = 0; p < 3; p++)
        for (int content = 0; content < (p < 2 ? 2 : 1); content++) {
            alloc_pic(16 * P[p][0], 16 * P[p][1], MALLOC_CAP_INTERNAL);
            fill(content);
            checkSpec = p == 2; /* the extra picture: most windows inside, so mostly PIE; spec checked on 1/64 */
            if (checkSpec)
                pad_planes();
            unsigned long c0 = cases;
            picture(P[p][2], P[p][3]);
            printf("MC pic %dx%d content %d: %lu cases, %lu mismatches so far (%lld s)\n", W, H, content, cases - c0, bad,
                   (esp_timer_get_time() - t0) / 1000000);
            if (p == 1 && content == 1)
                printf("MC BASE %lu cases (host/test_mc.c's space), %lu mismatches PIE vs C\n", cases, bad);
            free_pic();
        }
    printf("MC RESULT %lu cases, %lu mismatches PIE vs C; %lu spec cases, %lu mismatches\n", cases, bad, specCases,
           badSpec);
    printf("MC %s\n", bad || badSpec ? "FAIL" : "PASS");
    alloc_pic(320, 192, MALLOC_CAP_INTERNAL);
    fill(0);
    timing("iram");
    free_pic();
    alloc_pic(800, 480, MALLOC_CAP_SPIRAM);
    fill(0);
    timing("psram");
    free_pic();
    printf("MC done\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    vTaskDelay(10);
    printf("MC start\n");
    xTaskCreatePinnedToCore(run, "mc", 16384, NULL, 5, NULL, 0);
}
