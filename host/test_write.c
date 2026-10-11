/* h264bsdWriteOutputBlocks / h264bsdWriteMacroblock (the C path) against a plain reference: clip(pred + residual) per
 * pixel, empty blocks copied, on every MB of 1x1, 3x2 pictures, with guard bytes. Residuals over -512..767-pred. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "basetype.h"
#include "h264bsd_image.h"
#include "h264bsd_util.h"

static unsigned seed = 7;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 16; }
static const int BX[16] = {0, 4, 0, 4, 8, 12, 8, 12, 0, 4, 0, 4, 8, 12, 8, 12};
static const int BY[16] = {0, 0, 4, 4, 0, 0, 4, 4, 8, 8, 12, 12, 8, 8, 12, 12};

int main(void)
{
    unsigned long cases = 0, bad = 0;
    static const int P[2][2] = {{1, 1}, {3, 2}};
    for (int p = 0; p < 2; p++) {
        int pw = P[p][0], ph = P[p][1], n = pw * ph * 384, nmb = pw * ph;
        u8 *a = malloc(n + 128), *b = malloc(n + 128);
        for (int it = 0; it < 4000; it++)
            for (int mb = 0; mb < nmb; mb++) {
                u8 data[384] __attribute__((aligned(16)));
                i32 res[24][16] __attribute__((aligned(16)));
                int content = it & 3;
                for (int i = 0; i < 384; i++) data[i] = content == 1 ? ((rnd() & 1) ? 255 : 0) : content == 2 ? 255 : (u8)rnd();
                for (int k = 0; k < 24; k++) {
                    for (int i = 0; i < 16; i++) res[k][i] = (it & 4) ? ((rnd() & 1) ? -512 : 512) : (i32)(rnd() % 1025) - 512;
                    if (rnd() % 3 == 0) res[k][0] = EMPTY_RESIDUAL_INDICATOR;
                }
                memset(a, 0xA5, n + 128);
                memset(b, 0xA5, n + 128);
                image_t img = {a + 64, (u32)pw, (u32)ph, 0, 0, 0};
                h264bsdWriteOutputBlocks(&img, (u32)mb, data, res);
                /* reference */
                int row = mb / pw, col = mb % pw, W = pw * 16;
                u8 *Y = b + 64 + row * pw * 256 + col * 16, *C[2];
                C[0] = b + 64 + nmb * 256 + row * pw * 64 + col * 8;
                C[1] = C[0] + nmb * 64;
                for (int k = 0; k < 24; k++) {
                    int x, y, ps, ds;
                    const u8 *src;
                    u8 *dst;
                    if (k < 16) { x = BX[k]; y = BY[k]; src = data + y * 16 + x; ps = 16; dst = Y + y * W + x; ds = W; }
                    else { int j = k & 3; x = (j & 1) * 4; y = (j >> 1) * 4; src = data + 256 + (k >= 20) * 64 + y * 8 + x; ps = 8; dst = C[k >= 20] + y * (W / 2) + x; ds = W / 2; }
                    int empty = res[k][0] == EMPTY_RESIDUAL_INDICATOR;
                    for (int j = 0; j < 4; j++)
                        for (int i = 0; i < 4; i++) {
                            int v = src[j * ps + i] + res[k][j * 4 + i];
                            dst[j * ds + i] = empty ? src[j * ps + i] : v < 0 ? 0 : v > 255 ? 255 : v;
                        }
                }
                cases++;
                if (memcmp(a, b, n + 128) && bad++ < 4) printf("MISMATCH blocks pic %dx%d mb %d\n", pw, ph, mb);
                /* WriteMacroblock */
                memset(a, 0xA5, n + 128);
                memset(b, 0xA5, n + 128);
                image_t i2 = {a + 64, (u32)pw, (u32)ph, a + 64 + row * pw * 256 + col * 16, a + 64 + nmb * 256 + row * pw * 64 + col * 8, 0};
                i2.cr = i2.cb + nmb * 64;
                h264bsdWriteMacroblock(&i2, data);
                for (int j = 0; j < 16; j++) memcpy(Y + j * W, data + j * 16, 16);
                for (int j = 0; j < 8; j++) {
                    memcpy(C[0] + j * (W / 2), data + 256 + j * 8, 8);
                    memcpy(C[1] + j * (W / 2), data + 320 + j * 8, 8);
                }
                cases++;
                if (memcmp(a, b, n + 128) && bad++ < 4) printf("MISMATCH mb pic %dx%d mb %d\n", pw, ph, mb);
            }
        free(a); free(b);
    }
    printf("test_write: %lu cases, %lu mismatches\n", cases, bad);
    return bad != 0;
}
