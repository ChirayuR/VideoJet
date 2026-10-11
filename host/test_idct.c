/* test_idct: h264bsdProcessBlock (dequantisation + 4x4 inverse transform, the DC-only and rows-1..3-zero paths) against the
 * original h264bsd routine below, bit for bit: the return value, and all 16 outputs whenever it is HANTRO_OK (on
 * HANTRO_NOK the block contents are unspecified). Inputs: every coeffMap (all 65536, also ones that disagree with the
 * data) x every qp 0..51 x skip 0/1 x small, large, extreme and structured blocks, then random blocks. Built with
 * -fwrapv: the reference's i32 overflow (corrupt-stream coefficients) is then defined wrap-around. Exit 0 when every
 * case matches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "basetype.h"
#include "h264bsd_transform.h"
#include "h264bsd_util.h"

/* The reference: h264bsdProcessBlock and its tables from h264bsd_transform.c at 0deebdf, unchanged apart from the
 * ref/Ref name prefixes. */
/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 * Modified for VideoJet: copied from h264bsd_transform.c at 0deebdf into host/test_idct.c as its oracle, names prefixed.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* LevelScale function */
static const i32 refLevelScale[6][3] = {
    {10,13,16}, {11,14,18}, {13,16,20}, {14,18,23}, {16,20,25}, {18,23,29}};

/* qp % 6 as a function of qp */
static const u8 refQpMod6[52] = {0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,
    0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3};

/* qp / 6 as a function of qp */
static const u8 refQpDiv6[52] = {0,0,0,0,0,0,1,1,1,1,1,1,2,2,2,2,2,2,3,3,3,3,3,3,
    4,4,4,4,4,4,5,5,5,5,5,5,6,6,6,6,6,6,7,7,7,7,7,7,8,8,8,8};


u32 refProcessBlock(i32 *data, u32 qp, u32 skip, u32 coeffMap)
{

/* Variables */

    i32 tmp0, tmp1, tmp2, tmp3;
    i32 d1, d2, d3;
    u32 row,col;
    u32 qpDiv;
    i32 *ptr;

/* Code */

    qpDiv = refQpDiv6[qp];
    tmp1 = refLevelScale[refQpMod6[qp]][0] << qpDiv;
    tmp2 = refLevelScale[refQpMod6[qp]][1] << qpDiv;
    tmp3 = refLevelScale[refQpMod6[qp]][2] << qpDiv;

    if (!skip)
        data[0] = (data[0] * tmp1);

    /* at least one of the rows 1, 2 or 3 contain non-zero coeffs, mask takes
     * the scanning order into account */
    if (coeffMap & 0xFF9C)
    {
        /* do the zig-zag scan and inverse quantization */
        d1 = data[1];
        d2 = data[14];
        d3 = data[15];
        data[1] = (d1 * tmp2);
        data[14] = (d2 * tmp2);
        data[15] = (d3 * tmp3);

        d1 = data[2];
        d2 = data[5];
        d3 = data[4];
        data[4] = (d1 * tmp2);
        data[2]  = (d2 * tmp1);
        data[5] = (d3 * tmp3);

        d1 = data[8];
        d2 = data[3];
        d3 = data[6];
        tmp0 = (d1 * tmp2);
        data[8] = (d2 * tmp1);
        data[3]  = (d3 * tmp2);
        d1 = data[7];
        d2 = data[12];
        d3 = data[9];
        data[6]  = (d1 * tmp2);
        data[7]  = (d2 * tmp3);
        data[12] = (d3 * tmp2);
        data[9]  = tmp0;

        d1 = data[10];
        d2 = data[11];
        d3 = data[13];
        data[13] = (d1 * tmp3);
        data[10] = (d2 * tmp1);
        data[11] = (d3 * tmp2);

        /* horizontal transform */
        for (row = 4, ptr = data; row--; ptr += 4)
        {
            tmp0 = ptr[0] + ptr[2];
            tmp1 = ptr[0] - ptr[2];
            tmp2 = (ptr[1] >> 1) - ptr[3];
            tmp3 = ptr[1] + (ptr[3] >> 1);
            ptr[0] = tmp0 + tmp3;
            ptr[1] = tmp1 + tmp2;
            ptr[2] = tmp1 - tmp2;
            ptr[3] = tmp0 - tmp3;
        }

        /*lint +e661 +e662*/
        /* then vertical transform */
        for (col = 4; col--; data++)
        {
            tmp0 = data[0] + data[8];
            tmp1 = data[0] - data[8];
            tmp2 = (data[4] >> 1) - data[12];
            tmp3 = data[4] + (data[12] >> 1);
            data[0 ] = (tmp0 + tmp3 + 32)>>6;
            data[4 ] = (tmp1 + tmp2 + 32)>>6;
            data[8 ] = (tmp1 - tmp2 + 32)>>6;
            data[12] = (tmp0 - tmp3 + 32)>>6;
            /* check that each value is in the range [-512,511] */
            if (((u32)(data[0] + 512) > 1023) ||
                ((u32)(data[4] + 512) > 1023) ||
                ((u32)(data[8] + 512) > 1023) ||
                ((u32)(data[12] + 512) > 1023) )
                return(HANTRO_NOK);
        }
    }
    else /* rows 1, 2 and 3 are zero */
    {
        /* only dc-coeff is non-zero, i.e. coeffs at original positions
         * 1, 5 and 6 are zero */
        if ((coeffMap & 0x62) == 0)
        {
            tmp0 = (data[0] + 32) >> 6;
            /* check that value is in the range [-512,511] */
            if ((u32)(tmp0 + 512) > 1023)
                return(HANTRO_NOK);
            data[0] = data[1]  = data[2]  = data[3]  = data[4]  = data[5]  =
                      data[6]  = data[7]  = data[8]  = data[9]  = data[10] =
                      data[11] = data[12] = data[13] = data[14] = data[15] =
                      tmp0;
        }
        else /* at least one of the coeffs 1, 5 or 6 is non-zero */
        {
            data[1] = (data[1] * tmp2);
            data[2] = (data[5] * tmp1);
            data[3] = (data[6] * tmp2);
            tmp0 = data[0] + data[2];
            tmp1 = data[0] - data[2];
            tmp2 = (data[1] >> 1) - data[3];
            tmp3 = data[1] + (data[3] >> 1);
            data[0] = (tmp0 + tmp3 + 32)>>6;
            data[1] = (tmp1 + tmp2 + 32)>>6;
            data[2] = (tmp1 - tmp2 + 32)>>6;
            data[3] = (tmp0 - tmp3 + 32)>>6;
            data[4] = data[8] = data[12] = data[0];
            data[5] = data[9] = data[13] = data[1];
            data[6] = data[10] = data[14] = data[2];
            data[7] = data[11] = data[15] = data[3];
            /* check that each value is in the range [-512,511] */
            if (((u32)(data[0] + 512) > 1023) ||
                ((u32)(data[1] + 512) > 1023) ||
                ((u32)(data[2] + 512) > 1023) ||
                ((u32)(data[3] + 512) > 1023) )
                return(HANTRO_NOK);
        }
    }

    return(HANTRO_OK);

}


/*---- the test ----*/

static unsigned long long rs = 0x9E3779B97F4A7C15ull;
static u32 rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (u32)(rs >> 16);
}

static const i32 special[] = {0, 1, -1, 2, -2, 3, 127, -128, 255, 511, 512, -512, -513, 1023, 2047, -2048, 4095,
    32767, -32768, 32768, 65535, -65536, 0x3FFFFFFF, -0x40000000, 0x7FFFFFFF, INT_MIN, INT_MIN + 1, 0x55555555,
    (i32)0xAAAAAAAA, 0x00010001, 0x1FFFFFF, -0x1FFFFFF};
#define NSPECIAL ((int)(sizeof special / sizeof special[0]))

static unsigned long long cases, bad;

static void report(const i32 *in, u32 qp, u32 skip, u32 map, u32 ra, u32 rb, const i32 *a, const i32 *b)
{
    if (bad++ < 10) {
        int i;
        printf("MISMATCH qp %u skip %u map %04x ref %u new %u\n in:", qp, skip, map, ra, rb);
        for (i = 0; i < 16; i++) printf(" %d", in[i]);
        printf("\n ref:"); for (i = 0; i < 16; i++) printf(" %d", a[i]);
        printf("\n new:"); for (i = 0; i < 16; i++) printf(" %d", b[i]);
        printf("\n");
    }
}

/* new vs ref on a 16-byte aligned block (the PIE path on the board), and on every 5th case also on a block that is
 * only 4-byte aligned (the C fallback) */
static void check(const i32 *in, u32 qp, u32 skip, u32 map)
{
    i32 a[16] __attribute__((aligned(16))), b[16] __attribute__((aligned(16)));
    i32 mb[20] __attribute__((aligned(16)));
    u32 ra, rb;
    memcpy(a, in, sizeof a);
    memcpy(b, in, sizeof b);
    ra = refProcessBlock(a, qp, skip, map);
    rb = h264bsdProcessBlock(b, qp, skip, map);
    cases++;
    if (ra != rb || (ra == HANTRO_OK && memcmp(a, b, sizeof a)))
        report(in, qp, skip, map, ra, rb, a, b);
    if (cases % 5 == 0) {
        memcpy(mb + 1, in, sizeof a);
        mb[0] = mb[17] = mb[18] = mb[19] = 0x5A5A5A5A;
        rb = h264bsdProcessBlock(mb + 1, qp, skip, map);
        if (ra != rb || (ra == HANTRO_OK && memcmp(a, mb + 1, sizeof a)) || mb[0] != 0x5A5A5A5A || mb[17] != 0x5A5A5A5A)
            report(in, qp, skip, map, ra, rb, a, mb + 1);
    }
}

/* scale class (0: t1, 1: t2, 2: t3) of each scan position, from the scan -> raster map in h264bsdProcessBlock */
static const int clsOfScan[16] = {0, 1, 1, 0, 2, 0, 1, 1, 1, 1, 2, 0, 2, 1, 1, 2};

/* One block of the given kind: 0 small (|v| <= 3), 1 typical (|v| < 40, sparse), 2 large (16-bit), 3 special values,
 * 4 full 32-bit random, 5 DC only, 6 one special value in a random place, 7 uniform special value. */
static void fill(i32 *d, int kind)
{
    int i;
    for (i = 0; i < 16; i++) {
        u32 r = rnd();
        switch (kind) {
        case 0: d[i] = (i32)(r % 7) - 3; break;
        case 1: d[i] = (r & 3) ? 0 : (i32)((r >> 4) % 79) - 39; break;
        case 2: d[i] = (i32)(r % 65536) - 32768; break;
        case 3: d[i] = special[r % NSPECIAL]; break;
        case 4: d[i] = (i32)rnd() << 16 ^ (i32)rnd(); break;
        case 5: d[i] = i ? 0 : (i32)(r % 4001) - 2000; break;
        case 6: d[i] = 0; break;
        default: d[i] = special[(rs >> 40) % NSPECIAL]; break;
        }
    }
    if (kind == 6) d[rnd() & 15] = special[rnd() % NSPECIAL];
}

#ifndef VJ_IDCT_MAIN
#define VJ_IDCT_MAIN main
#endif
int VJ_IDCT_MAIN(void)
{
    i32 d[16];
    u32 qp, skip, map;
    int kind, n, i;

    /* every coeffMap x every qp x skip, two blocks each (kinds cycling) */
    for (map = 0; map < 65536; map++)
        for (qp = 0; qp < 52; qp++)
            for (skip = 0; skip < 2; skip++) {
                fill(d, (map + qp + skip) % 8);
                check(d, qp, skip, map);
                fill(d, (map * 3 + qp * 5 + skip + 1) % 8);
                check(d, qp, skip, map);
            }
    /* the structurally relevant maps (empty, DC only, 1/5/6 only, rows 1-3 zero or not, full) x all kinds x qp x skip */
    {
        static const u32 maps[] = {0, 1, 2, 0x20, 0x40, 0x62, 0x63, 0x1C, 0x80, 0x100, 0xFF9C, 0x7FFF, 0xFFFF, 0x8000,
            0xFF00, 0x00FF};
        for (n = 0; n < (int)(sizeof maps / sizeof maps[0]); n++)
            for (qp = 0; qp < 52; qp++)
                for (skip = 0; skip < 2; skip++)
                    for (kind = 0; kind < 8; kind++)
                        for (i = 0; i < 400; i++) {
                            fill(d, kind);
                            check(d, qp, skip, maps[n]);
                        }
    }
    /* each coefficient alone at +-1..+-k, every qp: the scan mapping and the scale class of every position */
    for (i = 0; i < 16; i++)
        for (qp = 0; qp < 52; qp++)
            for (skip = 0; skip < 2; skip++) {
                static const i32 v[] = {1, -1, 2, -3, 7, -64, 255, 3000, -3000};
                int j;
                for (j = 0; j < (int)(sizeof v / sizeof v[0]); j++) {
                    memset(d, 0, sizeof d);
                    d[i] = v[j];
                    check(d, qp, skip, 1u << i);
                    check(d, qp, skip, 0xFFFFu);
                }
            }
    /* the PIE guard: every dequantised product is within +-2^26 or not. Levels at the edge (product 2^26 - 2 .. 2^26 + 1,
     * both signs) in one, several or all positions, others random, every qp, full maps. */
    {
        static const i32 scale[6][3] = {{10,13,16}, {11,14,18}, {13,16,20}, {14,18,23}, {16,20,25}, {18,23,29}};
        int pos, dd;
        for (qp = 0; qp < 52; qp++)
            for (skip = 0; skip < 2; skip++)
                for (n = 0; n < 600; n++) {
                    int mode = n % 4;
                    fill(d, rnd() % 4);
                    for (pos = 0; pos < 16; pos++) {
                        i32 s = scale[qp % 6][clsOfScan[pos]] << (qp / 6);
                        i32 lv = ((1 << 26) + 1) / s;
                        if (mode == 0 && pos != (int)(rnd() & 15)) continue;
                        if (mode == 1 && (rnd() & 3)) continue;
                        dd = (int)(rnd() % 4) - 2;
                        d[pos] = (rnd() & 1) ? lv + dd : -(lv + dd);
                        if (skip && pos == 0) d[0] = (rnd() & 1) ? (1 << 26) + dd : -(1 << 26) - dd; /* unscaled DC */
                    }
                    check(d, qp, skip, 0xFFFF);
                }
    }
    /* random blocks, random maps (biased toward maps that agree with the data) */
    for (n = 0; n < 3000000; n++) {
        u32 m = 0;
        kind = rnd() % 8;
        fill(d, kind);
        if (rnd() & 1) { for (i = 0; i < 16; i++) if (d[i]) m |= 1u << i; }
        else m = rnd() & 0xFFFF;
        check(d, rnd() % 52, rnd() & 1, m);
    }
    printf("test_idct: %llu cases, %llu mismatches\n", cases, bad);
    return bad != 0;
}
