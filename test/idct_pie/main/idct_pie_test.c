/* On-board oracle for the PIE 4x4 inverse transform (vj_idct_pie.S via h264bsdProcessBlock): runs host/test_idct.c's
 * whole case space (the original routine vs the library, all coeffMaps x qps x skip, extreme and guard-edge
 * coefficients, aligned and misaligned blocks) on the S3, then prints cycles per block, old vs new. */
#include <stdio.h>
#include <string.h>
#include "esp_cpu.h"
#define VJ_IDCT_MAIN idct_cases /* test_idct.c's main */
#include "../../../host/test_idct.c"

#define NB 64
static i32 src[NB][16] __attribute__((aligned(16)));
static i32 w[16] __attribute__((aligned(16)));

static u32 timeit(int which, u32 qp, u32 skip, u32 map)
{
    u32 t0 = 0, t1 = 0, r;
    int i, rep;
    volatile u32 sink = 0;
    for (rep = 0; rep < 3; rep++) {
        t0 = esp_cpu_get_cycle_count();
        for (i = 0; i < NB * 8; i++) {
            memcpy(w, src[i % NB], 64);
            r = which == 0 ? 0 : which == 1 ? refProcessBlock(w, qp, skip, map) : h264bsdProcessBlock(w, qp, skip, map);
            sink += r;
        }
        t1 = esp_cpu_get_cycle_count();
    }
    return (t1 - t0) / (NB * 8);
}

void app_main(void)
{
    int n, i, rc;
    printf("IDCT start\n");
    rc = idct_cases();
    printf("IDCT RESULT %llu cases, %llu mismatches\n", cases, bad);
    printf(rc ? "IDCT FAIL\n" : "IDCT PASS\n");
    /* typical blocks: sparse small levels; maps as the data implies, or fixed */
    for (n = 0; n < NB; n++)
        for (i = 0; i < 16; i++) {
            u32 r = rnd();
            src[n][i] = (r & 3) ? 0 : (i32)((r >> 4) % 9) - 4;
        }
    {
        static const struct { const char *nm; u32 map, skip; } c[] = {
            {"full", 0xFFFF, 0}, {"full-skip", 0xFFFF, 1}, {"mid(1,5,6)", 0x62, 0}, {"dc", 0, 0}, {"dc-skip", 0, 1}};
        for (n = 0; n < 5; n++) {
            u32 q;
            for (q = 24; q <= 36; q += 12) {
                u32 base = timeit(0, q, c[n].skip, c[n].map), o = timeit(1, q, c[n].skip, c[n].map),
                    nw = timeit(2, q, c[n].skip, c[n].map);
                printf("IDCT TIME %s qp %u: old %u new %u cycles/block (copy %u removed)\n", c[n].nm, (unsigned)q,
                       (unsigned)(o - base), (unsigned)(nw - base), (unsigned)base);
            }
        }
    }
    printf("IDCT done\n");
}
