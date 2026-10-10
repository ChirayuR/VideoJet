/* test_mc: h264bsdPredictSamples against the standard's sample interpolation (8.4.2.2), reference coordinates clamped
 * to the picture. Exhaustive over: every quarter-sample luma and eighth-sample chroma position (mv & 7 per axis),
 * every partition (16x16 ... 4x4) at every offset in the macroblock, every integer reference position from fully
 * outside one edge to fully outside the other (plus far outside), on a 3x2 and a 1x1 macroblock picture, with random
 * and random 0/255 content. The whole 384-byte prediction array is compared, so writes outside the partition fail.
 * Prints the case count; exit 0 when every case matches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "basetype.h"
#include "h264bsd_reconstruct.h"

#define PAD 128 /* the model's clamped margin: covers the far-outside positions below */

static int W, H; /* luma size of the current picture */
static u8 *pic;  /* the reference: luma, cb, cr */
static u8 *ext[3]; /* each plane with PAD clamped samples around it */

static void pad_planes(void)
{
    for (int c = 0; c < 3; c++) {
        int w = c ? W / 2 : W, h = c ? H / 2 : H, ew = w + 2 * PAD;
        const u8 *p = pic + (c == 0 ? 0 : c == 1 ? W * H : W * H + W * H / 4);
        for (int y = 0; y < h + 2 * PAD; y++)
            for (int x = 0; x < ew; x++) {
                int cx = x - PAD < 0 ? 0 : x - PAD >= w ? w - 1 : x - PAD;
                int cy = y - PAD < 0 ? 0 : y - PAD >= h ? h - 1 : y - PAD;
                ext[c][y * ew + x] = p[cy * w + cx];
            }
    }
}

static int px(int c, int x, int y)
{
    int w = c ? W / 2 : W;
    return ext[c][(y + PAD) * (w + 2 * PAD) + x + PAD];
}

static int tap(int a, int b, int c, int d, int e, int f) { return a - 5 * b + 20 * c + 20 * d - 5 * e + f; }
static int clip1(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int b1(int x, int y) { return tap(px(0, x - 2, y), px(0, x - 1, y), px(0, x, y), px(0, x + 1, y), px(0, x + 2, y), px(0, x + 3, y)); }
static int h1(int x, int y) { return tap(px(0, x, y - 2), px(0, x, y - 1), px(0, x, y), px(0, x, y + 1), px(0, x, y + 2), px(0, x, y + 3)); }

/* the luma sample at full-sample (x, y) plus (xf, yf) quarters: 8-241..8-261 */
static int luma(int x, int y, int xf, int yf)
{
    int G = px(0, x, y), H_ = px(0, x + 1, y), M = px(0, x, y + 1);
    int b = clip1((b1(x, y) + 16) >> 5), s = clip1((b1(x, y + 1) + 16) >> 5);
    int h = clip1((h1(x, y) + 16) >> 5), m = clip1((h1(x + 1, y) + 16) >> 5);
    int j = 0;
    if ((xf == 2 && yf) || (yf == 2 && xf)) /* f i j k q */
        j = clip1((tap(b1(x, y - 2), b1(x, y - 1), b1(x, y), b1(x, y + 1), b1(x, y + 2), b1(x, y + 3)) + 512) >> 10);
    switch (4 * xf + yf) {
    case 0: return G;
    case 1: return (G + h + 1) >> 1;   /* d */
    case 2: return h;
    case 3: return (M + h + 1) >> 1;   /* n */
    case 4: return (G + b + 1) >> 1;   /* a */
    case 5: return (b + h + 1) >> 1;   /* e */
    case 6: return (h + j + 1) >> 1;   /* i */
    case 7: return (h + s + 1) >> 1;   /* p */
    case 8: return b;
    case 9: return (b + j + 1) >> 1;   /* f */
    case 10: return j;
    case 11: return (j + s + 1) >> 1;  /* q */
    case 12: return (H_ + b + 1) >> 1; /* c */
    case 13: return (b + m + 1) >> 1;  /* g */
    case 14: return (j + m + 1) >> 1;  /* k */
    default: return (m + s + 1) >> 1;  /* r */
    }
}

/* the chroma sample at full-sample (x, y) plus (xf, yf) eighths: 8-266 */
static int chroma(int c, int x, int y, int xf, int yf)
{
    return ((8 - xf) * (8 - yf) * px(c, x, y) + xf * (8 - yf) * px(c, x + 1, y) + (8 - xf) * yf * px(c, x, y + 1) +
            xf * yf * px(c, x + 1, y + 1) + 32) >> 6;
}

static unsigned long cases, bad;

/* one partition at (xA + px0, yA + py0), w x h, every motion vector in [h0, h1] x [v0, v1] quarter samples */
static void sweep(int xA, int yA, int px0, int py0, int w, int h, int mh0, int mh1, int mv0, int mv1)
{
    image_t img = {pic, (u32)W / 16, (u32)H / 16, 0, 0, 0};
    u8 data[384], want[384];
    for (int vy = mv0; vy <= mv1; vy++)
        for (int vx = mh0; vx <= mh1; vx++) {
            mv_t mv = {(i16)vx, (i16)vy};
            int lx = xA + px0 + (vx >> 2), ly = yA + py0 + (vy >> 2);
            int cx = (xA + px0) / 2 + (vx >> 3), cy = (yA + py0) / 2 + (vy >> 3);
            memset(want, 0xA5, sizeof want);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    want[(py0 + y) * 16 + px0 + x] = (u8)luma(lx + x, ly + y, vx & 3, vy & 3);
            for (int c = 1; c < 3; c++)
                for (int y = 0; y < h / 2; y++)
                    for (int x = 0; x < w / 2; x++)
                        want[256 + 64 * (c - 1) + (py0 / 2 + y) * 8 + px0 / 2 + x] =
                            (u8)chroma(c, cx + x, cy + y, vx & 7, vy & 7);
            memset(data, 0xA5, sizeof data);
            h264bsdPredictSamples(data, &mv, &img, (u32)xA, (u32)yA, (u32)px0, (u32)py0, (u32)w, (u32)h);
            cases++;
            if (memcmp(data, want, sizeof data) && bad++ < 5) {
                int i = 0;
                while (data[i] == want[i]) i++;
                fprintf(stderr, "MISMATCH pic %dx%d mb (%d,%d) part (%d,%d) %dx%d mv (%d,%d): byte %d got %d want %d\n",
                        W, H, xA, yA, px0, py0, w, h, vx, vy, i, data[i], want[i]);
            }
        }
}

/* every partition shape at every offset; integer positions from fully outside one edge to fully outside the other */
static void picture(int xA, int yA)
{
    static const int S[7][2] = {{16, 16}, {16, 8}, {8, 16}, {8, 8}, {8, 4}, {4, 8}, {4, 4}};
    for (int k = 0; k < 7; k++) {
        int w = S[k][0], h = S[k][1];
        for (int py0 = 0; py0 < 16; py0 += h)
            for (int px0 = 0; px0 < 16; px0 += w) {
                int bx = xA + px0, by = yA + py0; /* the partition's own position */
                /* integer offsets that put the block from -(w+8) to W+8: every edge case of the 6-tap window */
                int dx0 = -(w + 8) - bx, dx1 = W + 8 - bx, dy0 = -(h + 8) - by, dy1 = H + 8 - by;
                sweep(xA, yA, px0, py0, w, h, 4 * dx0, 4 * dx1 + 3, 4 * dy0, 4 * dy1 + 3);
                /* far outside: each side over the whole other range, and each corner (8 quarter samples each) */
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

int main(void)
{
    static const int P[2][4] = {{3, 2, 16, 16}, {1, 1, 0, 0}}; /* picture in macroblocks, macroblock position */
    unsigned seed = 1;
    for (int p = 0; p < 2; p++)
        for (int content = 0; content < 2; content++) {
            W = 16 * P[p][0];
            H = 16 * P[p][1];
            pic = malloc((size_t)W * H * 3 / 2); /* exact size: ASan sees any read outside the picture */
            for (int c = 0; c < 3; c++)
                ext[c] = malloc((size_t)((c ? W / 2 : W) + 2 * PAD) * ((c ? H / 2 : H) + 2 * PAD));
            if (!pic || !ext[0] || !ext[1] || !ext[2])
                return 2;
            for (int i = 0; i < W * H * 3 / 2; i++) {
                seed = seed * 1103515245u + 12345u;
                pic[i] = content ? ((seed >> 16) & 1 ? 255 : 0) : (u8)(seed >> 16);
            }
            pad_planes();
            picture(P[p][2], P[p][3]);
            free(pic);
            for (int c = 0; c < 3; c++)
                free(ext[c]);
        }
    printf("test_mc: %lu cases, %lu mismatches\n", cases, bad);
    return bad ? 1 : 0;
}
