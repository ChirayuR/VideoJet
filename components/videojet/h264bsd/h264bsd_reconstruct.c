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

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_reconstruct.h"
#include "h264bsd_macroblock_layer.h"
#include "h264bsd_image.h"
#include "h264bsd_util.h"
#include "vj_prof.h"

#ifdef H264DEC_OMXDL
#include "omxtypes.h"
#include "omxVC.h"
#include "armVC.h"
#endif /* H264DEC_OMXDL */

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

/* Luma fractional-sample positions
 *
 *  G a b c H
 *  d e f g
 *  h i j k m
 *  n p q r
 *  M   s   N
 *
 *  G, H, M and N are integer sample positions
 *  a-s are fractional samples that need to be interpolated.
 */
/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

#ifndef H264DEC_OMXDL

/*------------------------------------------------------------------------------

    Luma motion compensation, rewritten for VideoJet (see h264bsd/README.md,
    Local changes); chroma keeps the original h264bsdInterpolateChroma*.
    Bit-exact with the original h264bsdInterpolate* kernels: host/test_mc.c
    checks every position, partition and edge case against the standard's
    equations.

    Each partition reads one reference window: the block plus the 6-tap margins
    its fractional directions need (2 samples before, 3 after). The window is
    read in place when it lies inside the picture, else h264bsdFillBlock pads it
    into a local buffer (LumaEdge). Kernels read the window (s, row stride ss)
    and write the prediction array d (16 per row). Output sample (x, y) takes
    its 6 taps from window columns x..x+5 or rows y..y+5, so the full sample G
    of a fractional direction is window column x+2 or row y+2.

    The kernels keep a 6-sample window in registers and load one sample per
    output; each output overwrites the register of its oldest tap. The big
    buffers live in the helpers that need them (LumaEdge, LumaCentre), so the
    other frames stay small enough for short spill offsets.

------------------------------------------------------------------------------*/

#if defined(ESP_PLATFORM) && defined(CONFIG_VJ_PIE)
#include "vj_mc_pie.h"
#define VJ_PIE 1
#else
#define VJ_PIE 0
#endif

#define VJ_NOINLINE __attribute__((noinline))
#define VJ_INLINE static inline __attribute__((always_inline))

VJ_INLINE u32 Clip255(i32 v)
{
    return (u32)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* the 6-tap filter (1, -5, 20, 20, -5, 1); c and d weigh 20. Unsigned
 * wrap-around, read back as i32: the exact sum, without signed overflow */
#define TAP6(a, b, c, d, e, f) \
    ((i32)((a) + (f) + 5 * (4 * ((c) + (d)) - ((b) + (e)))))
/* half sample from a sum of samples, centre sample from a sum of sums */
#define HALF(t) Clip255(((t) + 16) >> 5)
#define CENTRE(t) Clip255(((t) + 512) >> 10)
#define AVG(a, b) (((a) + (u32)(b) + 1) >> 1)
/* output of variant q: 0 r, 1 r averaged with g, 2 with h */
#define OUTQ(r, g, h) (u8)(q == 1 ? AVG(r, g) : q == 2 ? AVG(r, h) : (r))

/* full-sample copy of a w x h block */
static VJ_NOINLINE void LumaCopy(const u8 *s, u32 ss, u8 *d, u32 w, u32 h)
{
    u32 x;

    for (; h; h--, s += ss, d += 16)
        for (x = 0; x < w; x += 4)
        {
            d[x] = s[x];
            d[x + 1] = s[x + 1];
            d[x + 2] = s[x + 2];
            d[x + 3] = s[x + 3];
        }
}

/* rows of the horizontal half sample b. q 1: averaged with G (a), 2: with H
 * (c) */
VJ_INLINE void LumaHorT(const u8 *s, u32 ss, u8 *d, u32 w, u32 h, const u32 q)
{
    for (; h; h--, s += ss, d += 16)
    {
        const u8 *p = s + 5;
        u8 *o = d;
        u32 t0 = s[0], t1 = s[1], t2 = s[2], t3 = s[3], t4 = s[4], t5, x, r;

        for (x = w >> 2; x; x--, p += 4, o += 4)
        {
            t5 = p[0];
            r = HALF(TAP6(t0, t1, t2, t3, t4, t5));
            o[0] = OUTQ(r, t2, t3);
            t0 = p[1];
            r = HALF(TAP6(t1, t2, t3, t4, t5, t0));
            o[1] = OUTQ(r, t3, t4);
            t1 = p[2];
            r = HALF(TAP6(t2, t3, t4, t5, t0, t1));
            o[2] = OUTQ(r, t4, t5);
            t2 = p[3];
            r = HALF(TAP6(t3, t4, t5, t0, t1, t2));
            o[3] = OUTQ(r, t5, t0);
            /* the next window, x+2..x+6, is t4 t5 t0 t1 t2 */
            t3 = t0; t0 = t4; t4 = t2; t2 = t3; t3 = t1; t1 = t5;
        }
    }
}

static VJ_NOINLINE void LumaHor0(const u8 *s, u32 ss, u8 *d, u32 w, u32 h)
{
    LumaHorT(s, ss, d, w, h, 0);
}

static VJ_NOINLINE void LumaHor1(const u8 *s, u32 ss, u8 *d, u32 w, u32 h)
{
    LumaHorT(s, ss, d, w, h, 1);
}

static VJ_NOINLINE void LumaHor2(const u8 *s, u32 ss, u8 *d, u32 w, u32 h)
{
    LumaHorT(s, ss, d, w, h, 2);
}

/* columns of the vertical half sample h. q 1: averaged with G (d), 2: with M
 * (n), 3: with what d holds (the diagonal positions) */
#define VOUT(o, r, g, h) ((o) = q == 3 ? (u8)AVG(r, o) : OUTQ(r, g, h))

VJ_INLINE void LumaVerT(const u8 *s, u32 ss, u8 *d, u32 w, u32 h, const u32 q)
{
    u32 x;

    for (x = 0; x < w; x++)
    {
        u8 *o = d + x;
        u32 k = x, t0, t1, t2, t3, t4, t5, y, r;

        t0 = s[k]; k += ss;
        t1 = s[k]; k += ss;
        t2 = s[k]; k += ss;
        t3 = s[k]; k += ss;
        t4 = s[k]; k += ss;
        for (y = h >> 2; y; y--, o += 64)
        {
            t5 = s[k]; k += ss;
            r = HALF(TAP6(t0, t1, t2, t3, t4, t5));
            VOUT(o[0], r, t2, t3);
            t0 = s[k]; k += ss;
            r = HALF(TAP6(t1, t2, t3, t4, t5, t0));
            VOUT(o[16], r, t3, t4);
            t1 = s[k]; k += ss;
            r = HALF(TAP6(t2, t3, t4, t5, t0, t1));
            VOUT(o[32], r, t4, t5);
            t2 = s[k]; k += ss;
            r = HALF(TAP6(t3, t4, t5, t0, t1, t2));
            VOUT(o[48], r, t5, t0);
            t3 = t0; t0 = t4; t4 = t2; t2 = t3; t3 = t1; t1 = t5;
        }
    }
}

static VJ_NOINLINE void LumaVer0(const u8 *s, u32 ss, u8 *d, u32 w, u32 h)
{
    LumaVerT(s, ss, d, w, h, 0);
}

static VJ_NOINLINE void LumaVer1(const u8 *s, u32 ss, u8 *d, u32 w, u32 h)
{
    LumaVerT(s, ss, d, w, h, 1);
}

static VJ_NOINLINE void LumaVer2(const u8 *s, u32 ss, u8 *d, u32 w, u32 h)
{
    LumaVerT(s, ss, d, w, h, 2);
}

static VJ_NOINLINE void LumaVer3(const u8 *s, u32 ss, u8 *d, u32 w, u32 h)
{
    LumaVerT(s, ss, d, w, h, 3);
}

/* rows of horizontal 6-tap sums (b1 of the standard), w per row of t */
static VJ_NOINLINE void LumaHorSum(const u8 *s, u32 ss, i16 *t, u32 w,
    u32 rows)
{
    for (; rows; rows--, s += ss)
    {
        const u8 *p = s + 5;
        u32 t0 = s[0], t1 = s[1], t2 = s[2], t3 = s[3], t4 = s[4], t5, x;

        for (x = w >> 2; x; x--, p += 4, t += 4)
        {
            t5 = p[0];
            t[0] = (i16)TAP6(t0, t1, t2, t3, t4, t5);
            t0 = p[1];
            t[1] = (i16)TAP6(t1, t2, t3, t4, t5, t0);
            t1 = p[2];
            t[2] = (i16)TAP6(t2, t3, t4, t5, t0, t1);
            t2 = p[3];
            t[3] = (i16)TAP6(t3, t4, t5, t0, t1, t2);
            t3 = t0; t0 = t4; t4 = t2; t2 = t3; t3 = t1; t1 = t5;
        }
    }
}

/* columns of vertical 6-tap sums (h1), h rows of cols in t */
static VJ_NOINLINE void LumaVerSum(const u8 *s, u32 ss, i16 *t, u32 cols,
    u32 h)
{
    u32 x;

    for (x = 0; x < cols; x++)
    {
        u32 k = x, n = x, t0, t1, t2, t3, t4, t5, y;

        t0 = s[k]; k += ss;
        t1 = s[k]; k += ss;
        t2 = s[k]; k += ss;
        t3 = s[k]; k += ss;
        t4 = s[k]; k += ss;
        for (y = h >> 2; y; y--)
        {
            t5 = s[k]; k += ss;
            t[n] = (i16)TAP6(t0, t1, t2, t3, t4, t5); n += cols;
            t0 = s[k]; k += ss;
            t[n] = (i16)TAP6(t1, t2, t3, t4, t5, t0); n += cols;
            t1 = s[k]; k += ss;
            t[n] = (i16)TAP6(t2, t3, t4, t5, t0, t1); n += cols;
            t2 = s[k]; k += ss;
            t[n] = (i16)TAP6(t3, t4, t5, t0, t1, t2); n += cols;
            t3 = t0; t0 = t4; t4 = t2; t2 = t3; t3 = t1; t1 = t5;
        }
    }
}

/* centre sample j from 6-tap sums; q 1: averaged with the half sample of the
 * sum two taps on (b or h: f, i), 2: three taps on (s or m: q, k) */
#define COUT(r, g, h) (u8)(q == 1 ? AVG(r, HALF((i32)(g))) : \
    q == 2 ? AVG(r, HALF((i32)(h))) : (r))

/* columns of j from LumaHorSum's rows (w each): j, f, q */
VJ_INLINE void LumaMidVerT(const i16 *t, u8 *d, u32 w, u32 h, const u32 q)
{
    u32 x;

    for (x = 0; x < w; x++)
    {
        u8 *o = d + x;
        u32 k = x, t0, t1, t2, t3, t4, t5, y, r;

        t0 = (u32)t[k]; k += w;
        t1 = (u32)t[k]; k += w;
        t2 = (u32)t[k]; k += w;
        t3 = (u32)t[k]; k += w;
        t4 = (u32)t[k]; k += w;
        for (y = h >> 2; y; y--, o += 64)
        {
            t5 = (u32)t[k]; k += w;
            r = CENTRE(TAP6(t0, t1, t2, t3, t4, t5));
            o[0] = COUT(r, t2, t3);
            t0 = (u32)t[k]; k += w;
            r = CENTRE(TAP6(t1, t2, t3, t4, t5, t0));
            o[16] = COUT(r, t3, t4);
            t1 = (u32)t[k]; k += w;
            r = CENTRE(TAP6(t2, t3, t4, t5, t0, t1));
            o[32] = COUT(r, t4, t5);
            t2 = (u32)t[k]; k += w;
            r = CENTRE(TAP6(t3, t4, t5, t0, t1, t2));
            o[48] = COUT(r, t5, t0);
            t3 = t0; t0 = t4; t4 = t2; t2 = t3; t3 = t1; t1 = t5;
        }
    }
}

static VJ_NOINLINE void LumaMidVer0(const i16 *t, u8 *d, u32 w, u32 h)
{
    LumaMidVerT(t, d, w, h, 0);
}

static VJ_NOINLINE void LumaMidVer1(const i16 *t, u8 *d, u32 w, u32 h)
{
    LumaMidVerT(t, d, w, h, 1);
}

static VJ_NOINLINE void LumaMidVer2(const i16 *t, u8 *d, u32 w, u32 h)
{
    LumaMidVerT(t, d, w, h, 2);
}

/* rows of j from LumaVerSum's rows (w + 5 each): i, k */
VJ_INLINE void LumaMidHorT(const i16 *t, u8 *d, u32 w, u32 h, const u32 q)
{
    for (; h; h--, t += w + 5, d += 16)
    {
        const i16 *p = t + 5;
        u8 *o = d;
        u32 t0 = (u32)t[0], t1 = (u32)t[1], t2 = (u32)t[2], t3 = (u32)t[3];
        u32 t4 = (u32)t[4], t5, x, r;

        for (x = w >> 2; x; x--, p += 4, o += 4)
        {
            t5 = (u32)p[0];
            r = CENTRE(TAP6(t0, t1, t2, t3, t4, t5));
            o[0] = COUT(r, t2, t3);
            t0 = (u32)p[1];
            r = CENTRE(TAP6(t1, t2, t3, t4, t5, t0));
            o[1] = COUT(r, t3, t4);
            t1 = (u32)p[2];
            r = CENTRE(TAP6(t2, t3, t4, t5, t0, t1));
            o[2] = COUT(r, t4, t5);
            t2 = (u32)p[3];
            r = CENTRE(TAP6(t3, t4, t5, t0, t1, t2));
            o[3] = COUT(r, t5, t0);
            t3 = t0; t0 = t4; t4 = t2; t2 = t3; t3 = t1; t1 = t5;
        }
    }
}

static VJ_NOINLINE void LumaMidHor1(const i16 *t, u8 *d, u32 w, u32 h)
{
    LumaMidHorT(t, d, w, h, 1);
}

static VJ_NOINLINE void LumaMidHor2(const i16 *t, u8 *d, u32 w, u32 h)
{
    LumaMidHorT(t, d, w, h, 2);
}

/* the centre positions: 6-tap sums one way into a table, then the other way */
static VJ_NOINLINE void LumaCentre(const u8 *s, u32 ss, u8 *d, u32 w, u32 h,
    u32 xFrac, u32 yFrac)
{
    i16 t[21 * 16];

    if (xFrac == 2) /* f j q */
    {
        LumaHorSum(s, ss, t, w, h + 5);
        if (yFrac == 2)
            LumaMidVer0(t, d, w, h);
        else if (yFrac == 1)
            LumaMidVer1(t, d, w, h);
        else
            LumaMidVer2(t, d, w, h);
    }
    else /* i k */
    {
        LumaVerSum(s, ss, t, w + 5, h);
        if (xFrac == 1)
            LumaMidHor1(t, d, w, h);
        else
            LumaMidHor2(t, d, w, h);
    }
}

/* luma prediction of a w x h block from its window s (stride ss) */
static void LumaPredict(const u8 *s, u32 ss, u8 *d, u32 w, u32 h, u32 xFrac,
    u32 yFrac)
{
    switch (4 * xFrac + yFrac)
    {
        case 0: /* G */
            LumaCopy(s, ss, d, w, h);
            break;
        case 1: /* d */
            LumaVer1(s, ss, d, w, h);
            break;
        case 2: /* h */
            LumaVer0(s, ss, d, w, h);
            break;
        case 3: /* n */
            LumaVer2(s, ss, d, w, h);
            break;
        case 4: /* a */
            LumaHor1(s, ss, d, w, h);
            break;
        case 8: /* b */
            LumaHor0(s, ss, d, w, h);
            break;
        case 12: /* c */
            LumaHor2(s, ss, d, w, h);
            break;
        case 5: /* e */
        case 7: /* p */
        case 13: /* g */
        case 15: /* r: b or s, then averaged with h or m */
            LumaHor0(s + (2 + (yFrac >> 1)) * ss, ss, d, w, h);
            LumaVer3(s + 2 + (xFrac >> 1), ss, d, w, h);
            break;
        default: /* f i j k q */
            LumaCentre(s, ss, d, w, h, xFrac, yFrac);
            break;
    }
}

/* a window that crosses the picture edge: padded, then predicted */
static VJ_NOINLINE void LumaEdge(u8 *ref, u32 width, u32 height, i32 x0,
    i32 y0, u8 *d, u32 w, u32 h, u32 xFrac, u32 yFrac)
{
    u8 win[21 * 21];
    u32 ww = w + (xFrac ? 5 : 0), hh = h + (yFrac ? 5 : 0);

    h264bsdFillBlock(ref, win, x0, y0, width, height, ww, hh, ww);
    LumaPredict(win, ww, d, w, h, xFrac, yFrac);
}

/* windows inside the picture: the PIE kernels for 8 and 16 wide partitions
 * (CONFIG_VJ_PIE), else C */
static void LumaInside(const u8 *s, u32 ss, u8 *d, u32 w, u32 h, u32 xFrac,
    u32 yFrac)
{
#if VJ_PIE
    if (w >= 8)
    {
        switch (4 * xFrac + yFrac)
        {
            case 0: vj_pie_luma_copy(s, ss, d, w, h); return;
            case 1: vj_pie_luma_ver1(s, ss, d, w, h); return;
            case 2: vj_pie_luma_ver0(s, ss, d, w, h); return;
            case 3: vj_pie_luma_ver2(s, ss, d, w, h); return;
            case 4: vj_pie_luma_hor1(s, ss, d, w, h); return;
            case 8: vj_pie_luma_hor0(s, ss, d, w, h); return;
            case 12: vj_pie_luma_hor2(s, ss, d, w, h); return;
            case 5:
            case 7:
            case 13:
            case 15: /* b or s, then averaged with h or m */
                vj_pie_luma_hor0(s + (2 + (yFrac >> 1)) * ss, ss, d, w, h);
                vj_pie_luma_ver3(s + 2 + (xFrac >> 1), ss, d, w, h);
                return;
            case 9:
            case 11: /* f q: j, then averaged with b or s */
                vj_pie_luma_mid(s, ss, d, w, h);
                vj_pie_luma_hor3(s + (2 + (yFrac >> 1)) * ss, ss, d, w, h);
                return;
            case 6:
            case 14: /* i k: j, then averaged with h or m */
                vj_pie_luma_mid(s, ss, d, w, h);
                vj_pie_luma_ver3(s + 2 + (xFrac >> 1), ss, d, w, h);
                return;
            default: /* j */
                vj_pie_luma_mid(s, ss, d, w, h);
                return;
        }
    }
#endif
    LumaPredict(s, ss, d, w, h, xFrac, yFrac);
}

#if VJ_PIE
/* chroma of an 8 or 16 wide partition whose window lies inside the picture,
 * both planes, in PIE (a full-sample copy is the bilinear filter with A = 64).
 * Returns 0, doing nothing, when the window crosses the picture edge */
static u32 ChromaPie(u8 *d, u32 xAL, u32 yAL, u32 partWidth, u32 partHeight,
    mv_t *mv, image_t *refPic)
{
    u32 width = 8 * refPic->width, height = 8 * refPic->height;
    u32 xFrac = mv->hor & 0x7, yFrac = mv->ver & 0x7;
    u32 w = partWidth >> 1, h = partHeight >> 1;
    i32 x0 = (i32)(xAL >> 1) + (mv->hor >> 3);
    i32 y0 = (i32)(yAL >> 1) + (mv->ver >> 3);

    if (x0 < 0 || y0 < 0 || (u32)x0 + w + (xFrac ? 1 : 0) > width ||
        (u32)y0 + h + (yFrac ? 1 : 0) > height)
        return 0;
    vj_pie_chroma(refPic->data + 256 * refPic->width * refPic->height +
        (u32)y0 * width + (u32)x0, width, d, w, h,
        (8 - xFrac) * (8 - yFrac) | xFrac * (8 - yFrac) << 8 |
        (8 - xFrac) * yFrac << 16 | xFrac * yFrac << 24, width * height);
    return 1;
}
#endif

/*------------------------------------------------------------------------------

    Function: h264bsdInterpolateChromaHor

        Functional description:
          This function performs chroma interpolation in horizontal direction.
          Overfilling is done only if needed. Reference image (pRef) is
          read at correct position and the predicted part is written to
          macroblock's chrominance (predPartChroma)
        Inputs:
          pRef              pointer to reference frame Cb top-left corner
          x0                integer x-coordinate for prediction
          y0                integer y-coordinate for prediction
          width             width of the reference frame chrominance in pixels
          height            height of the reference frame chrominance in pixels
          xFrac             horizontal fraction for prediction in 1/8 pixels
          chromaPartWidth   width of the predicted part in pixels
          chromaPartHeight  height of the predicted part in pixels
        Outputs:
          predPartChroma    pointer where predicted part is written

------------------------------------------------------------------------------*/
#ifndef H264DEC_ARM11
void h264bsdInterpolateChromaHor(
  u8 *pRef,
  u8 *predPartChroma,
  i32 x0,
  i32 y0,
  u32 width,
  u32 height,
  u32 xFrac,
  u32 chromaPartWidth,
  u32 chromaPartHeight)
{

/* Variables */

    u32 x, y, tmp1, tmp2, tmp3, tmp4, c, val;
    u8 *ptrA, *cbr;
    u32 comp;
    u8 block[9*8*2];

/* Code */

    ASSERT(predPartChroma);
    ASSERT(chromaPartWidth);
    ASSERT(chromaPartHeight);
    ASSERT(xFrac < 8);
    ASSERT(pRef);

    if ((x0 < 0) || ((u32)x0+chromaPartWidth+1 > width) ||
        (y0 < 0) || ((u32)y0+chromaPartHeight > height))
    {
        h264bsdFillBlock(pRef, block, x0, y0, width, height,
            chromaPartWidth + 1, chromaPartHeight, chromaPartWidth + 1);
        pRef += width * height;
        h264bsdFillBlock(pRef, block + (chromaPartWidth+1)*chromaPartHeight,
            x0, y0, width, height, chromaPartWidth + 1,
            chromaPartHeight, chromaPartWidth + 1);

        pRef = block;
        x0 = 0;
        y0 = 0;
        width = chromaPartWidth+1;
        height = chromaPartHeight;
    }

    val = 8 - xFrac;

    for (comp = 0; comp <= 1; comp++)
    {

        ptrA = pRef + (comp * height + (u32)y0) * width + x0;
        cbr = predPartChroma + comp * 8 * 8;

        /* 2x2 pels per iteration
         * bilinear horizontal interpolation */
        for (y = (chromaPartHeight >> 1); y; y--)
        {
            for (x = (chromaPartWidth >> 1); x; x--)
            {
                tmp1 = ptrA[width];
                tmp2 = *ptrA++;
                tmp3 = ptrA[width];
                tmp4 = *ptrA++;
                c = ((val * tmp1 + xFrac * tmp3) << 3) + 32;
                c >>= 6;
                cbr[8] = (u8)c;
                c = ((val * tmp2 + xFrac * tmp4) << 3) + 32;
                c >>= 6;
                *cbr++ = (u8)c;
                tmp1 = ptrA[width];
                tmp2 = *ptrA;
                c = ((val * tmp3 + xFrac * tmp1) << 3) + 32;
                c >>= 6;
                cbr[8] = (u8)c;
                c = ((val * tmp4 + xFrac * tmp2) << 3) + 32;
                c >>= 6;
                *cbr++ = (u8)c;
            }
            cbr += 2*8 - chromaPartWidth;
            ptrA += 2*width - chromaPartWidth;
        }
    }

}

/*------------------------------------------------------------------------------

    Function: h264bsdInterpolateChromaVer

        Functional description:
          This function performs chroma interpolation in vertical direction.
          Overfilling is done only if needed. Reference image (pRef) is
          read at correct position and the predicted part is written to
          macroblock's chrominance (predPartChroma)

------------------------------------------------------------------------------*/

void h264bsdInterpolateChromaVer(
  u8 *pRef,
  u8 *predPartChroma,
  i32 x0,
  i32 y0,
  u32 width,
  u32 height,
  u32 yFrac,
  u32 chromaPartWidth,
  u32 chromaPartHeight)
{

/* Variables */

    u32 x, y, tmp1, tmp2, tmp3, c, val;
    u8 *ptrA, *cbr;
    u32 comp;
    u8 block[9*8*2];

/* Code */

    ASSERT(predPartChroma);
    ASSERT(chromaPartWidth);
    ASSERT(chromaPartHeight);
    ASSERT(yFrac < 8);
    ASSERT(pRef);

    if ((x0 < 0) || ((u32)x0+chromaPartWidth > width) ||
        (y0 < 0) || ((u32)y0+chromaPartHeight+1 > height))
    {
        h264bsdFillBlock(pRef, block, x0, y0, width, height, chromaPartWidth,
            chromaPartHeight + 1, chromaPartWidth);
        pRef += width * height;
        h264bsdFillBlock(pRef, block + chromaPartWidth*(chromaPartHeight+1),
            x0, y0, width, height, chromaPartWidth,
            chromaPartHeight + 1, chromaPartWidth);

        pRef = block;
        x0 = 0;
        y0 = 0;
        width = chromaPartWidth;
        height = chromaPartHeight+1;
    }

    val = 8 - yFrac;

    for (comp = 0; comp <= 1; comp++)
    {

        ptrA = pRef + (comp * height + (u32)y0) * width + x0;
        cbr = predPartChroma + comp * 8 * 8;

        /* 2x2 pels per iteration
         * bilinear vertical interpolation */
        for (y = (chromaPartHeight >> 1); y; y--)
        {
            for (x = (chromaPartWidth >> 1); x; x--)
            {
                tmp3 = ptrA[width*2];
                tmp2 = ptrA[width];
                tmp1 = *ptrA++;
                c = ((val * tmp2 + yFrac * tmp3) << 3) + 32;
                c >>= 6;
                cbr[8] = (u8)c;
                c = ((val * tmp1 + yFrac * tmp2) << 3) + 32;
                c >>= 6;
                *cbr++ = (u8)c;
                tmp3 = ptrA[width*2];
                tmp2 = ptrA[width];
                tmp1 = *ptrA++;
                c = ((val * tmp2 + yFrac * tmp3) << 3) + 32;
                c >>= 6;
                cbr[8] = (u8)c;
                c = ((val * tmp1 + yFrac * tmp2) << 3) + 32;
                c >>= 6;
                *cbr++ = (u8)c;
            }
            cbr += 2*8 - chromaPartWidth;
            ptrA += 2*width - chromaPartWidth;
        }
    }

}
#endif
/*------------------------------------------------------------------------------

    Function: h264bsdInterpolateChromaHorVer

        Functional description:
          This function performs chroma interpolation in horizontal and
          vertical direction. Overfilling is done only if needed. Reference
          image (ref) is read at correct position and the predicted part
          is written to macroblock's chrominance (predPartChroma)

------------------------------------------------------------------------------*/

void h264bsdInterpolateChromaHorVer(
  u8 *ref,
  u8 *predPartChroma,
  i32 x0,
  i32 y0,
  u32 width,
  u32 height,
  u32 xFrac,
  u32 yFrac,
  u32 chromaPartWidth,
  u32 chromaPartHeight)
{
    u8 block[9*9*2];
    u32 x, y, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, valX, valY, plus32 = 32;
    u32 comp;
    u8 *ptrA, *cbr;

/* Code */

    ASSERT(predPartChroma);
    ASSERT(chromaPartWidth);
    ASSERT(chromaPartHeight);
    ASSERT(xFrac < 8);
    ASSERT(yFrac < 8);
    ASSERT(ref);

    if ((x0 < 0) || ((u32)x0+chromaPartWidth+1 > width) ||
        (y0 < 0) || ((u32)y0+chromaPartHeight+1 > height))
    {
        h264bsdFillBlock(ref, block, x0, y0, width, height,
            chromaPartWidth + 1, chromaPartHeight + 1, chromaPartWidth + 1);
        ref += width * height;
        h264bsdFillBlock(ref, block + (chromaPartWidth+1)*(chromaPartHeight+1),
            x0, y0, width, height, chromaPartWidth + 1,
            chromaPartHeight + 1, chromaPartWidth + 1);

        ref = block;
        x0 = 0;
        y0 = 0;
        width = chromaPartWidth+1;
        height = chromaPartHeight+1;
    }

    valX = 8 - xFrac;
    valY = 8 - yFrac;

    for (comp = 0; comp <= 1; comp++)
    {

        ptrA = ref + (comp * height + (u32)y0) * width + x0;
        cbr = predPartChroma + comp * 8 * 8;

        /* 2x2 pels per iteration
         * bilinear vertical and horizontal interpolation */
        for (y = (chromaPartHeight >> 1); y; y--)
        {
            tmp1 = *ptrA;
            tmp3 = ptrA[width];
            tmp5 = ptrA[width*2];
            tmp1 *= valY;
            tmp1 += tmp3 * yFrac;
            tmp3 *= valY;
            tmp3 += tmp5 * yFrac;
            for (x = (chromaPartWidth >> 1); x; x--)
            {
                tmp2 = *++ptrA;
                tmp4 = ptrA[width];
                tmp6 = ptrA[width*2];
                tmp2 *= valY;
                tmp2 += tmp4 * yFrac;
                tmp4 *= valY;
                tmp4 += tmp6 * yFrac;
                tmp1 = tmp1 * valX + plus32;
                tmp3 = tmp3 * valX + plus32;
                tmp1 += tmp2 * xFrac;
                tmp1 >>= 6;
                tmp3 += tmp4 * xFrac;
                tmp3 >>= 6;
                cbr[8] = (u8)tmp3;
                *cbr++ = (u8)tmp1;

                tmp1 = *++ptrA;
                tmp3 = ptrA[width];
                tmp5 = ptrA[width*2];
                tmp1 *= valY;
                tmp1 += tmp3 * yFrac;
                tmp3 *= valY;
                tmp3 += tmp5 * yFrac;
                tmp2 = tmp2 * valX + plus32;
                tmp4 = tmp4 * valX + plus32;
                tmp2 += tmp1 * xFrac;
                tmp2 >>= 6;
                tmp4 += tmp3 * xFrac;
                tmp4 >>= 6;
                cbr[8] = (u8)tmp4;
                *cbr++ = (u8)tmp2;
            }
            cbr += 2*8 - chromaPartWidth;
            ptrA += 2*width - chromaPartWidth;
        }
    }

}

/*------------------------------------------------------------------------------

    Function: PredictChroma

        Functional description:
          Top level chroma prediction function that calls the appropriate
          interpolation function. The output is written to macroblock array.

------------------------------------------------------------------------------*/

static void PredictChroma(
  u8 *mbPartChroma,
  u32 xAL,
  u32 yAL,
  u32 partWidth,
  u32 partHeight,
  mv_t *mv,
  image_t *refPic)
{

/* Variables */

    u32 xFrac, yFrac, width, height, chromaPartWidth, chromaPartHeight;
    i32 xInt, yInt;
    u8 *ref;

/* Code */

    ASSERT(mv);
    ASSERT(refPic);
    ASSERT(refPic->data);
    ASSERT(refPic->width);
    ASSERT(refPic->height);

    width  = 8 * refPic->width;
    height = 8 * refPic->height;

    xInt = (xAL >> 1) + (mv->hor >> 3);
    yInt = (yAL >> 1) + (mv->ver >> 3);
    xFrac = mv->hor & 0x7;
    yFrac = mv->ver & 0x7;

    chromaPartWidth  = partWidth >> 1;
    chromaPartHeight = partHeight >> 1;
    ref = refPic->data + 256 * refPic->width * refPic->height;

    if (xFrac && yFrac)
    {
        h264bsdInterpolateChromaHorVer(ref, mbPartChroma, xInt, yInt, width,
                height, xFrac, yFrac, chromaPartWidth, chromaPartHeight);
    }
    else if (xFrac)
    {
        h264bsdInterpolateChromaHor(ref, mbPartChroma, xInt, yInt, width,
                height, xFrac, chromaPartWidth, chromaPartHeight);
    }
    else if (yFrac)
    {
        h264bsdInterpolateChromaVer(ref, mbPartChroma, xInt, yInt, width,
                height, yFrac, chromaPartWidth, chromaPartHeight);
    }
    else
    {
        h264bsdFillBlock(ref, mbPartChroma, xInt, yInt, width, height,
            chromaPartWidth, chromaPartHeight, 8);
        ref += width * height;
        h264bsdFillBlock(ref, mbPartChroma + 8*8, xInt, yInt, width, height,
            chromaPartWidth, chromaPartHeight, 8);
    }

}


/*------------------------------------------------------------------------------

    Function: h264bsdPredictSamples

        Functional description:
          This function reconstructs a prediction for a macroblock partition.
          The prediction is either copied or interpolated using the reference
          frame and the motion vector. Both luminance and chrominance parts are
          predicted. The prediction is stored in given macroblock array (data).
        Inputs:
          data          pointer to macroblock array (384 bytes) for output
          mv            pointer to motion vector used for prediction
          refPic        pointer to reference picture structure
          xA            x-coordinate for current macroblock
          yA            y-coordinate for current macroblock
          partX         x-offset for partition in macroblock
          partY         y-offset for partition in macroblock
          partWidth     width of partition
          partHeight    height of partition
        Outputs:
          data          macroblock array (16x16+8x8+8x8) where predicted
                        partition is stored at correct position

------------------------------------------------------------------------------*/

void h264bsdPredictSamples(
  u8 *data,
  mv_t *mv,
  image_t *refPic,
  u32 xA,
  u32 yA,
  u32 partX,
  u32 partY,
  u32 partWidth,
  u32 partHeight)

{

/* Variables */

    u32 xFrac, yFrac, width, height, w, h;
    i32 x0, y0;
    u8 *ref, *d;
    VJP_BEGIN(VJP_MC);

/* Code */

    ASSERT(data);
    ASSERT(mv);
    ASSERT(partWidth);
    ASSERT(partHeight);
    ASSERT(refPic);
    ASSERT(refPic->data);
    ASSERT(refPic->width);
    ASSERT(refPic->height);

    /* luma */
    { VJP_BEGIN(VJP_LUMA);
    xFrac = mv->hor & 0x3;
    yFrac = mv->ver & 0x3;
    width = 16 * refPic->width;
    height = 16 * refPic->height;
    /* the window: 2 samples more before and 3 after per fractional axis */
    x0 = (i32)xA + (i32)partX + (mv->hor >> 2) - (xFrac ? 2 : 0);
    y0 = (i32)yA + (i32)partY + (mv->ver >> 2) - (yFrac ? 2 : 0);
    w = partWidth + (xFrac ? 5 : 0);
    h = partHeight + (yFrac ? 5 : 0);
    ref = refPic->data;
    d = data + 16 * partY + partX;
    if (x0 >= 0 && y0 >= 0 && (u32)x0 + w <= width && (u32)y0 + h <= height)
        LumaInside(ref + (u32)y0 * width + (u32)x0, width, d, partWidth,
            partHeight, xFrac, yFrac);
    else if (!xFrac && !yFrac)
        h264bsdFillBlock(ref, d, x0, y0, width, height, partWidth,
            partHeight, 16);
    else
        LumaEdge(ref, width, height, x0, y0, d, partWidth, partHeight, xFrac,
            yFrac);
    VJP_END_AT(VJP_LUMA, VJP_LUMA + 4 * xFrac + yFrac); }

    /* chroma */
    { VJP_BEGIN(VJP_CHROMA);
#if VJ_PIE
    if (partWidth < 8 || !ChromaPie(data + 16*16 + (partY>>1)*8 + (partX>>1),
        xA + partX, yA + partY, partWidth, partHeight, mv, refPic))
#endif
    PredictChroma(
      data + 16*16 + (partY>>1)*8 + (partX>>1),
      xA + partX,
      yA + partY,
      partWidth,
      partHeight,
      mv,
      refPic);
    VJP_END(VJP_CHROMA); }
    VJP_END(VJP_MC);

}

#else /* H264DEC_OMXDL */
/*------------------------------------------------------------------------------

    Function: h264bsdPredictSamples

        Functional description:
          This function reconstructs a prediction for a macroblock partition.
          The prediction is either copied or interpolated using the reference
          frame and the motion vector. Both luminance and chrominance parts are
          predicted. The prediction is stored in given macroblock array (data).
        Inputs:
          data          pointer to macroblock array (384 bytes) for output
          mv            pointer to motion vector used for prediction
          refPic        pointer to reference picture structure
          xA            x-coordinate for current macroblock
          yA            y-coordinate for current macroblock
          partX         x-offset for partition in macroblock
          partY         y-offset for partition in macroblock
          partWidth     width of partition
          partHeight    height of partition
        Outputs:
          data          macroblock array (16x16+8x8+8x8) where predicted
                        partition is stored at correct position

------------------------------------------------------------------------------*/

/*lint -e{550} Symbol 'res' not accessed */
void h264bsdPredictSamples(
  u8 *data,
  mv_t *mv,
  image_t *refPic,
  u32 colAndRow,
  u32 part,
  u8 *pFill)

{

/* Variables */

    u32 xFrac, yFrac;
    u32 width, height;
    i32 xInt, yInt, x0, y0;
    u8 *partData, *ref;
    OMXSize roi;
    u32 fillWidth;
    u32 fillHeight;
    OMXResult res;
    u32 xA, yA;
    u32 partX, partY;
    u32 partWidth, partHeight;

/* Code */

    ASSERT(data);
    ASSERT(mv);
    ASSERT(refPic);
    ASSERT(refPic->data);
    ASSERT(refPic->width);
    ASSERT(refPic->height);

    xA = (colAndRow & 0xFFFF0000) >> 16;
    yA = (colAndRow & 0x0000FFFF);

    partX = (part & 0xFF000000) >> 24;
    partY = (part & 0x00FF0000) >> 16;
    partWidth = (part & 0x0000FF00) >> 8;
    partHeight = (part & 0x000000FF);

    ASSERT(partWidth);
    ASSERT(partHeight);

    /* luma */
    partData = data + 16*partY + partX;

    xFrac = mv->hor & 0x3;
    yFrac = mv->ver & 0x3;

    width = 16 * refPic->width;
    height = 16 * refPic->height;

    xInt = (i32)xA + (i32)partX + (mv->hor >> 2);
    yInt = (i32)yA + (i32)partY + (mv->ver >> 2);

    x0 = (xFrac) ? xInt-2 : xInt;
    y0 = (yFrac) ? yInt-2 : yInt;

    if (xFrac)
    {
        if (partWidth == 16)
            fillWidth = 32;
        else
            fillWidth = 16;
    }
    else
        fillWidth = (partWidth*2);
    if (yFrac)
        fillHeight = partHeight+5;
    else
        fillHeight = partHeight;


    if ((x0 < 0) || ((u32)x0+fillWidth > width) ||
        (y0 < 0) || ((u32)y0+fillHeight > height))
    {
        h264bsdFillBlock(refPic->data, (u8*)pFill, x0, y0, width, height,
                fillWidth, fillHeight, fillWidth);

        x0 = 0;
        y0 = 0;
        ref = pFill;
        width = fillWidth;
        if (yFrac)
            ref += 2*width;
        if (xFrac)
            ref += 2;
    }
    else
    {
        /*lint --e(737) Loss of sign */
        ref = refPic->data + yInt*width + xInt;
    }
    /* Luma interpolation */
    roi.width = (i32)partWidth;
    roi.height = (i32)partHeight;

    res = omxVCM4P10_InterpolateLuma(ref, (i32)width, partData, 16,
                                        (i32)xFrac, (i32)yFrac, roi);
    ASSERT(res == 0);

    /* Chroma */
    width  = 8 * refPic->width;
    height = 8 * refPic->height;

    x0 = ((xA + partX) >> 1) + (mv->hor >> 3);
    y0 = ((yA + partY) >> 1) + (mv->ver >> 3);
    xFrac = mv->hor & 0x7;
    yFrac = mv->ver & 0x7;

    ref = refPic->data + 256 * refPic->width * refPic->height;

    roi.width = (i32)(partWidth >> 1);
    fillWidth = ((partWidth >> 1) + 8) & ~0x7;
    roi.height = (i32)(partHeight >> 1);
    fillHeight = (partHeight >> 1) + 1;

    if ((x0 < 0) || ((u32)x0+fillWidth > width) ||
        (y0 < 0) || ((u32)y0+fillHeight > height))
    {
        h264bsdFillBlock(ref, pFill, x0, y0, width, height,
            fillWidth, fillHeight, fillWidth);
        ref += width * height;
        h264bsdFillBlock(ref, pFill + fillWidth*fillHeight,
            x0, y0, width, height, fillWidth,
            fillHeight, fillWidth);

        ref = pFill;
        x0 = 0;
        y0 = 0;
        width = fillWidth;
        height = fillHeight;
    }

    partData = data + 16*16 + (partY>>1)*8 + (partX>>1);

    /* Chroma interpolation */
    /*lint --e(737) Loss of sign */
    ref += y0 * width + x0;
    res = armVCM4P10_Interpolate_Chroma(ref, width, partData, 8,
                            (u32)roi.width, (u32)roi.height, xFrac, yFrac);
    ASSERT(res == 0);
    partData += 8 * 8;
    ref += height * width;
    res = armVCM4P10_Interpolate_Chroma(ref, width, partData, 8,
                            (u32)roi.width, (u32)roi.height, xFrac, yFrac);
    ASSERT(res == 0);

}

#endif /* H264DEC_OMXDL */


/*------------------------------------------------------------------------------

    Function: FillRow1

        Functional description:
          This function gets a row of reference pels in a 'normal' case when no
          overfilling is necessary.

------------------------------------------------------------------------------*/

static void FillRow1(
  u8 *ref,
  u8 *fill,
  i32 left,
  i32 center,
  i32 right)
{
#ifndef FLASCC
    ASSERT(ref);
    ASSERT(fill);

    memcpy(fill, ref, center);
#else
    int i = 0;    
    u8 *pdest = (u8*) fill;
    u8 *psrc = (u8*) ref;
    int loops = (center / sizeof(u32));

    ASSERT(ref);
    ASSERT(fill);

    for(i = 0; i < loops; ++i)
    {
        *((u32*)pdest) = *((u32*)psrc);
        pdest += sizeof(u32);
        psrc += sizeof(u32);
    }

    loops = (center % sizeof(u32));
    for (i = 0; i < loops; ++i)
    {
        *pdest = *psrc;
        ++pdest;
        ++psrc;
    }
#endif

    /*lint -e(715) */
}


/*------------------------------------------------------------------------------

    Function: h264bsdFillRow7

        Functional description:
          This function gets a row of reference pels when horizontal coordinate
          is partly negative or partly greater than reference picture width
          (overfilling some pels on left and/or right edge).
        Inputs:
          ref       pointer to reference samples
          left      amount of pixels to overfill on left-edge
          center    amount of pixels to copy
          right     amount of pixels to overfill on right-edge
        Outputs:
          fill      pointer where samples are stored

------------------------------------------------------------------------------*/
#ifndef H264DEC_NEON
void h264bsdFillRow7(
  u8 *ref,
  u8 *fill,
  i32 left,
  i32 center,
  i32 right)
{
    u8 tmp = '\0';

    ASSERT(ref);
    ASSERT(fill);

    if (left)
        tmp = *ref;

    for ( ; left; left--)
        /*lint -esym(644,tmp)  tmp is initialized if used */
        *fill++ = tmp;

    for ( ; center; center--)
        *fill++ = *ref++;

    if (right)
        tmp = ref[-1];

    for ( ; right; right--)
        /*lint -esym(644,tmp)  tmp is initialized if used */
        *fill++ = tmp;
}
#endif
/*------------------------------------------------------------------------------

    Function: h264bsdFillBlock

        Functional description:
          This function gets a block of reference pels. It determines whether
          overfilling is needed or not and repeatedly calls an appropriate
          function (by using a function pointer) that fills one row the block.
        Inputs:
          ref               pointer to reference frame
          x0                x-coordinate for block
          y0                y-coordinate for block
          width             width of reference frame
          height            height of reference frame
          blockWidth        width of block
          blockHeight       height of block
          fillScanLength    length of a line in output array (pixels)
        Outputs:
          fill              pointer to array where output block is written

------------------------------------------------------------------------------*/

void h264bsdFillBlock(
  u8 *ref,
  u8 *fill,
  i32 x0,
  i32 y0,
  u32 width,
  u32 height,
  u32 blockWidth,
  u32 blockHeight,
  u32 fillScanLength)

{

/* Variables */

    i32 xstop, ystop;
    void (*fp)(u8*, u8*, i32, i32, i32);
    i32 left, x, right;
    i32 top, y, bottom;

/* Code */

    ASSERT(ref);
    ASSERT(fill);
    ASSERT(width);
    ASSERT(height);
    ASSERT(fill);
    ASSERT(blockWidth);
    ASSERT(blockHeight);

    xstop = x0 + (i32)blockWidth;
    ystop = y0 + (i32)blockHeight;

    /* Choose correct function whether overfilling on left-edge or right-edge
     * is needed or not */
    if (x0 >= 0 && xstop <= (i32)width)
        fp = FillRow1;
    else
        fp = h264bsdFillRow7;

    if (ystop < 0)
        y0 = -(i32)blockHeight;

    if (xstop < 0)
        x0 = -(i32)blockWidth;

    if (y0 > (i32)height)
        y0 = (i32)height;

    if (x0 > (i32)width)
        x0 = (i32)width;

    xstop = x0 + (i32)blockWidth;
    ystop = y0 + (i32)blockHeight;

    if (x0 > 0)
        ref += x0;

    if (y0 > 0)
        ref += y0 * (i32)width;

    left = x0 < 0 ? -x0 : 0;
    right = xstop > (i32)width ? xstop - (i32)width : 0;
    x = (i32)blockWidth - left - right;

    top = y0 < 0 ? -y0 : 0;
    bottom = ystop > (i32)height ? ystop - (i32)height : 0;
    y = (i32)blockHeight - top - bottom;

    if (x0 >= 0 && xstop <= (i32)width)
    {
        for ( ; top; top-- )
        {
            FillRow1(ref, fill, left, x, right);
            fill += fillScanLength;
        }
        for ( ; top; top-- )
        {
            FillRow1(ref, fill, left, x, right);            
        }
        for ( ; y; y-- )
        {
            FillRow1(ref, fill, left, x, right);
            ref += width;
            fill += fillScanLength;
        }
    }
    else
    {
        for ( ; top; top-- )
        {
            h264bsdFillRow7(ref, fill, left, x, right);
            fill += fillScanLength;
        }
        for ( ; top; top-- )
        {
            h264bsdFillRow7(ref, fill, left, x, right);            
        }
        for ( ; y; y-- )
        {
            h264bsdFillRow7(ref, fill, left, x, right);
            ref += width;
            fill += fillScanLength;
        }
    }
    /* Top-overfilling */
    

    /* Lines inside reference image */
    

    ref -= width;

    /* Bottom-overfilling */
    for ( ; bottom; bottom-- )
    {
        //(*fp)(ref, fill, left, x, right);
        if (x0 >= 0 && xstop <= (i32)width)
            FillRow1(ref, fill, left, x, right);
        else
            h264bsdFillRow7(ref, fill, left, x, right);
        fill += fillScanLength;
    }
}

/*lint +e701 +e702 */


