/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 * Modified for VideoJet: see h264bsd/README.md, Local changes.
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

/*------------------------------------------------------------------------------

    Table of contents

     1. Include headers
     2. External compiler flags
     3. Module defines
     4. Local function prototypes
     5. Functions
          h264bsdProcessBlock
          h264bsdProcessLumaDc
          h264bsdProcessChromaDc

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_transform.h"
#include "h264bsd_util.h"

#if defined(ESP_PLATFORM) && defined(CONFIG_VJ_PIE)
#include <stdint.h>
#include "vj_idct_pie.h"
#define VJ_PIE 1
#else
#define VJ_PIE 0
#endif

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/* Switch off the following Lint messages for this file:
 * Info 701: Shift left of signed quantity (int)
 * Info 702: Shift right of signed quantity (int)
 */
/*lint -e701 -e702 */

/* LevelScale function */
static const i32 levelScale[6][3] = {
    {10,13,16}, {11,14,18}, {13,16,20}, {14,18,23}, {16,20,25}, {18,23,29}};

/* qp % 6 as a function of qp */
static const u8 qpMod6[52] = {0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,
    0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3};

/* qp / 6 as a function of qp */
static const u8 qpDiv6[52] = {0,0,0,0,0,0,1,1,1,1,1,1,2,2,2,2,2,2,3,3,3,3,3,3,
    4,4,4,4,4,4,5,5,5,5,5,5,6,6,6,6,6,6,7,7,7,7,7,7,8,8,8,8};

/* VideoJet: levelScale[qp % 6][0..2] << (qp / 6) for every qp (max 5888, so u16), used by h264bsdProcessBlock. */
static const u16 levelScaleQp[52][3] = {
    {10,13,16}, {11,14,18}, {13,16,20}, {14,18,23},
    {16,20,25}, {18,23,29}, {20,26,32}, {22,28,36},
    {26,32,40}, {28,36,46}, {32,40,50}, {36,46,58},
    {40,52,64}, {44,56,72}, {52,64,80}, {56,72,92},
    {64,80,100}, {72,92,116}, {80,104,128}, {88,112,144},
    {104,128,160}, {112,144,184}, {128,160,200}, {144,184,232},
    {160,208,256}, {176,224,288}, {208,256,320}, {224,288,368},
    {256,320,400}, {288,368,464}, {320,416,512}, {352,448,576},
    {416,512,640}, {448,576,736}, {512,640,800}, {576,736,928},
    {640,832,1024}, {704,896,1152}, {832,1024,1280}, {896,1152,1472},
    {1024,1280,1600}, {1152,1472,1856}, {1280,1664,2048}, {1408,1792,2304},
    {1664,2048,2560}, {1792,2304,2944}, {2048,2560,3200}, {2304,2944,3712},
    {2560,3328,4096}, {2816,3584,4608}, {3328,4096,5120}, {3584,4608,5888},
};

/* One row of the 4x4 inverse transform (the horizontal pass), on u32 so overflow wraps as the i32 original did. */
#define ROW_PASS(A, B, C, D, O0, O1, O2, O3) do { \
    u32 ra_ = (A), rb_ = (B), rc_ = (C), rd_ = (D); \
    u32 r0_ = ra_ + rc_, r1_ = ra_ - rc_; \
    u32 r2_ = (u32)((i32)rb_ >> 1) - rd_; \
    u32 r3_ = rb_ + (u32)((i32)rd_ >> 1); \
    O0 = r0_ + r3_; O1 = r1_ + r2_; O2 = r1_ - r2_; O3 = r0_ - r3_; } while (0)

/* One column (the vertical pass): rounds, stores, and ORs (value + 512) into chk for the range check. */
#define COL_PASS(X0, X1, X2, X3, D0, D1, D2, D3) do { \
    u32 c0_ = (X0) + (X2), c1_ = (X0) - (X2); \
    u32 c2_ = (u32)((i32)(X1) >> 1) - (X3); \
    u32 c3_ = (X1) + (u32)((i32)(X3) >> 1); \
    i32 v0_ = (i32)(c0_ + c3_ + 32) >> 6, v1_ = (i32)(c1_ + c2_ + 32) >> 6; \
    i32 v2_ = (i32)(c1_ - c2_ + 32) >> 6, v3_ = (i32)(c0_ - c3_ + 32) >> 6; \
    D0 = v0_; D1 = v1_; D2 = v2_; D3 = v3_; \
    chk |= (u32)(v0_ + 512) | (u32)(v1_ + 512) | (u32)(v2_ + 512) | (u32)(v3_ + 512); } while (0)

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------

    Function: h264bsdProcessBlock

        Functional description:
            Function performs inverse zig-zag scan, inverse scaling and
            inverse transform for a luma or a chroma residual block

        Inputs:
            data            pointer to data to be processed
            qp              quantization parameter
            skip            skip processing of data[0], set to non-zero value
                            if dc coeff hanled separately
            coeffMap        16 lsb's indicate which coeffs are non-zero,
                            bit 0 (lsb) for coeff 0, bit 1 for coeff 1 etc.

        Outputs:
            data            processed data

        Returns:
            HANTRO_OK       success
            HANTRO_NOK      processed data not in valid range [-512, 511]

------------------------------------------------------------------------------*/
u32 h264bsdProcessBlock(i32 *data, u32 qp, u32 skip, u32 coeffMap)
{

/* Variables */

    u32 t1, t2, t3;
    u32 a, e0, e1, e2, e3;
    u32 chk;

/* Code */

    /* VideoJet: the level scales come pre-shifted by qp / 6 from a table, and the arithmetic below is done in u32 (the
     * same wrap-around as the original i32 code, which every compiler here gives) with the zig-zag scan, inverse
     * scaling, horizontal and vertical pass fused in registers: no intermediate store or reload. The range check ORs
     * (value + 512) over the block: (u32)(v + 512) > 1023 is "a bit >= 10 is set". On HANTRO_NOK the block contents
     * are unspecified (the original left it partly transformed); the caller abandons the macroblock. */
    t1 = levelScaleQp[qp][0];
    t2 = levelScaleQp[qp][1];
    t3 = levelScaleQp[qp][2];

    a = (u32)data[0];
    if (!skip)
        a *= t1;

    /* at least one of the rows 1, 2 or 3 contain non-zero coeffs, mask takes
     * the scanning order into account */
    if (coeffMap & 0xFF9C)
    {
        u32 h00, h01, h02, h03, h10, h11, h12, h13;
        u32 h20, h21, h22, h23, h30, h31, h32, h33;

#if VJ_PIE
        /* PIE: the products go to a transposed, 16-aligned scratch block, vj_pie_idct4 does both passes. It returns 2,
         * with data untouched, when a product is outside +-2^26 (its saturating arithmetic could then differ from the
         * C wrap-around), and the C code below runs. Needs a 16-aligned data (ProcessResidual's blocks are). */
        if (!((uintptr_t)data & 15))
        {
            i32 t[16] __attribute__((aligned(16)));
            u32 r;

            t[0] = (i32)a;
            t[4] = (i32)((u32)data[1] * t2);   t[8] = (i32)((u32)data[5] * t1);   t[12] = (i32)((u32)data[6] * t2);
            t[1] = (i32)((u32)data[2] * t2);   t[5] = (i32)((u32)data[4] * t3);
            t[9] = (i32)((u32)data[7] * t2);   t[13] = (i32)((u32)data[12] * t3);
            t[2] = (i32)((u32)data[3] * t1);   t[6] = (i32)((u32)data[8] * t2);
            t[10] = (i32)((u32)data[11] * t1); t[14] = (i32)((u32)data[13] * t2);
            t[3] = (i32)((u32)data[9] * t2);   t[7] = (i32)((u32)data[10] * t3);
            t[11] = (i32)((u32)data[14] * t2); t[15] = (i32)((u32)data[15] * t3);
            r = vj_pie_idct4(t, data, vj_idct_k);
            if (r < 2)
                return r ? HANTRO_NOK : HANTRO_OK;
        }
#endif

        /* scan position -> raster position: 0 1 5 6 / 2 4 7 12 / 3 8 11 13 / 9 10 14 15 */
        ROW_PASS(a, (u32)data[1] * t2, (u32)data[5] * t1, (u32)data[6] * t2,
                 h00, h01, h02, h03);
        ROW_PASS((u32)data[2] * t2, (u32)data[4] * t3, (u32)data[7] * t2, (u32)data[12] * t3,
                 h10, h11, h12, h13);
        ROW_PASS((u32)data[3] * t1, (u32)data[8] * t2, (u32)data[11] * t1, (u32)data[13] * t2,
                 h20, h21, h22, h23);
        ROW_PASS((u32)data[9] * t2, (u32)data[10] * t3, (u32)data[14] * t2, (u32)data[15] * t3,
                 h30, h31, h32, h33);

        chk = 0;
        COL_PASS(h00, h10, h20, h30, data[0], data[4], data[8], data[12]);
        COL_PASS(h01, h11, h21, h31, data[1], data[5], data[9], data[13]);
        COL_PASS(h02, h12, h22, h32, data[2], data[6], data[10], data[14]);
        COL_PASS(h03, h13, h23, h33, data[3], data[7], data[11], data[15]);
        if (chk > 1023)
            return(HANTRO_NOK);
    }
    else /* rows 1, 2 and 3 are zero */
    {
        /* only dc-coeff is non-zero, i.e. coeffs at original positions
         * 1, 5 and 6 are zero */
        if ((coeffMap & 0x62) == 0)
        {
            i32 v = (i32)(a + 32) >> 6;
            /* check that value is in the range [-512,511] */
            if ((u32)(v + 512) > 1023)
                return(HANTRO_NOK);
            data[0] = data[1]  = data[2]  = data[3]  = data[4]  = data[5]  =
                      data[6]  = data[7]  = data[8]  = data[9]  = data[10] =
                      data[11] = data[12] = data[13] = data[14] = data[15] =
                      v;
        }
        else /* at least one of the coeffs 1, 5 or 6 is non-zero */
        {
            i32 o0, o1, o2, o3;

            ROW_PASS(a, (u32)data[1] * t2, (u32)data[5] * t1, (u32)data[6] * t2,
                     e0, e1, e2, e3);
            o0 = (i32)(e0 + 32) >> 6;
            o1 = (i32)(e1 + 32) >> 6;
            o2 = (i32)(e2 + 32) >> 6;
            o3 = (i32)(e3 + 32) >> 6;
            /* check that each value is in the range [-512,511] */
            if (((u32)(o0 + 512) | (u32)(o1 + 512) | (u32)(o2 + 512) | (u32)(o3 + 512)) > 1023)
                return(HANTRO_NOK);
            data[0] = data[4] = data[8]  = data[12] = o0;
            data[1] = data[5] = data[9]  = data[13] = o1;
            data[2] = data[6] = data[10] = data[14] = o2;
            data[3] = data[7] = data[11] = data[15] = o3;
        }
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: h264bsdProcessLumaDc

        Functional description:
            Function performs inverse zig-zag scan, inverse transform and
            inverse scaling for a luma DC coefficients block

        Inputs:
            data            pointer to data to be processed
            qp              quantization parameter

        Outputs:
            data            processed data

        Returns:
            none

------------------------------------------------------------------------------*/
void h264bsdProcessLumaDc(i32 *data, u32 qp)
{

/* Variables */

    i32 tmp0, tmp1, tmp2, tmp3;
    u32 row,col;
    u32 qpMod, qpDiv;
    i32 levScale;
    i32 *ptr;

/* Code */

    qpMod = qpMod6[qp];
    qpDiv = qpDiv6[qp];

    /* zig-zag scan */
    tmp0 = data[2];
    data[2]  = data[5];
    data[5] = data[4];
    data[4] = tmp0;

    tmp0 = data[8];
    data[8] = data[3];
    data[3]  = data[6];
    data[6]  = data[7];
    data[7]  = data[12];
    data[12] = data[9];
    data[9]  = tmp0;

    tmp0 = data[10];
    data[10] = data[11];
    data[11] = data[13];
    data[13] = tmp0;

    /* horizontal transform */
    for (row = 4, ptr = data; row--; ptr += 4)
    {
        tmp0 = ptr[0] + ptr[2];
        tmp1 = ptr[0] - ptr[2];
        tmp2 = ptr[1] - ptr[3];
        tmp3 = ptr[1] + ptr[3];
        ptr[0] = tmp0 + tmp3;
        ptr[1] = tmp1 + tmp2;
        ptr[2] = tmp1 - tmp2;
        ptr[3] = tmp0 - tmp3;
    }

    /*lint +e661 +e662*/
    /* then vertical transform and inverse scaling */
    levScale = levelScale[ qpMod ][0];
    if (qp >= 12)
    {
        levScale <<= (qpDiv-2);
        for (col = 4; col--; data++)
        {
            tmp0 = data[0] + data[8 ];
            tmp1 = data[0] - data[8 ];
            tmp2 = data[4] - data[12];
            tmp3 = data[4] + data[12];
            data[0 ] = ((tmp0 + tmp3)*levScale);
            data[4 ] = ((tmp1 + tmp2)*levScale);
            data[8 ] = ((tmp1 - tmp2)*levScale);
            data[12] = ((tmp0 - tmp3)*levScale);
        }
    }
    else
    {
        i32 tmp;
        tmp = ((1 - qpDiv) == 0) ? 1 : 2;
        for (col = 4; col--; data++)
        {
            tmp0 = data[0] + data[8 ];
            tmp1 = data[0] - data[8 ];
            tmp2 = data[4] - data[12];
            tmp3 = data[4] + data[12];
            data[0 ] = ((tmp0 + tmp3)*levScale+tmp) >> (2-qpDiv);
            data[4 ] = ((tmp1 + tmp2)*levScale+tmp) >> (2-qpDiv);
            data[8 ] = ((tmp1 - tmp2)*levScale+tmp) >> (2-qpDiv);
            data[12] = ((tmp0 - tmp3)*levScale+tmp) >> (2-qpDiv);
        }
    }

}

/*------------------------------------------------------------------------------

    Function: h264bsdProcessChromaDc

        Functional description:
            Function performs inverse transform and inverse scaling for a
            chroma DC coefficients block

        Inputs:
            data            pointer to data to be processed
            qp              quantization parameter

        Outputs:
            data            processed data

        Returns:
            none

------------------------------------------------------------------------------*/
void h264bsdProcessChromaDc(i32 *data, u32 qp)
{

/* Variables */

    i32 tmp0, tmp1, tmp2, tmp3;
    u32 qpDiv;
    i32 levScale;
    u32 levShift;

/* Code */

    qpDiv = qpDiv6[qp];
    levScale = levelScale[ qpMod6[qp] ][0];

    if (qp >= 6)
    {
        levScale <<= (qpDiv-1);
        levShift = 0;
    }
    else
    {
        levShift = 1;
    }

    tmp0 = data[0] + data[2];
    tmp1 = data[0] - data[2];
    tmp2 = data[1] - data[3];
    tmp3 = data[1] + data[3];
    data[0] = ((tmp0 + tmp3) * levScale) >> levShift;
    data[1] = ((tmp0 - tmp3) * levScale) >> levShift;
    data[2] = ((tmp1 + tmp2) * levScale) >> levShift;
    data[3] = ((tmp1 - tmp2) * levScale) >> levShift;
    tmp0 = data[4] + data[6];
    tmp1 = data[4] - data[6];
    tmp2 = data[5] - data[7];
    tmp3 = data[5] + data[7];
    data[4] = ((tmp0 + tmp3) * levScale) >> levShift;
    data[5] = ((tmp0 - tmp3) * levScale) >> levShift;
    data[6] = ((tmp1 + tmp2) * levScale) >> levShift;
    data[7] = ((tmp1 - tmp2) * levScale) >> levShift;

}

/*lint +e701 +e702 */


