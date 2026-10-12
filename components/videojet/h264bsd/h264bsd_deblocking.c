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
          h264bsdDeblockRecord
          h264bsdFilterPicture
          FilterVerLumaEdge
          FilterHorLumaEdge
          FilterHorLuma
          FilterVerChromaEdge
          FilterHorChromaEdge
          FilterHorChroma
          InnerBoundaryStrength
          EdgeBoundaryStrength
          GetBoundaryStrengths
          IsSliceBoundaryOnLeft
          IsSliceBoundaryOnTop
          GetMbFilteringFlags
          GetEdgeThresholds
          FilterLuma
          FilterChroma

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_util.h"
#include "h264bsd_macroblock_layer.h"
#include "h264bsd_deblocking.h"
#include "h264bsd_dpb.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef VJ_DBW_STATS
#include <stdio.h>
#endif
#include "vj_alloc.h"
#include "vj_prof.h"
#ifdef VJ_DEBLOCK_WORKER
#include "vj_worker.h"
#endif

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

/* array of alpha values, from the standard */
static const u8 alphas[52] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,5,6,7,8,9,10,
    12,13,15,17,20,22,25,28,32,36,40,45,50,56,63,71,80,90,101,113,127,144,162,
    182,203,226,255,255};

/* array of beta values, from the standard */
static const u8 betas[52] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,3,3,3,3,4,4,
    4,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18};



#ifndef H264DEC_OMXDL
/* array of tc0 values, from the standard, each triplet corresponds to a
 * column in the table. Indexing goes as tc0[indexA][bS-1] */
static const u8 tc0[52][3] = {
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,1},{0,1,1},{1,1,1},
    {1,1,1},{1,1,1},{1,1,1},{1,1,2},{1,1,2},{1,1,2},{1,1,2},{1,2,3},
    {1,2,3},{2,2,3},{2,2,4},{2,3,4},{2,3,4},{3,3,5},{3,4,6},{3,4,6},
    {4,5,7},{4,5,8},{4,6,9},{5,7,10},{6,8,11},{6,8,13},{7,10,14},{8,11,16},
    {9,12,18},{10,13,20},{11,15,23},{13,17,25}
};
#else
/* array of tc0 values, from the standard, each triplet corresponds to a
 * column in the table. Indexing goes as tc0[indexA][bS] */
static const u8 tc0[52][5] = {
    {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0}, {0, 0, 0, 1, 0}, {0, 0, 0, 1, 0}, {0, 0, 0, 1, 0},
    {0, 0, 0, 1, 0}, {0, 0, 1, 1, 0}, {0, 0, 1, 1, 0}, {0, 1, 1, 1, 0},
    {0, 1, 1, 1, 0}, {0, 1, 1, 1, 0}, {0, 1, 1, 1, 0}, {0, 1, 1, 2, 0},
    {0, 1, 1, 2, 0}, {0, 1, 1, 2, 0}, {0, 1, 1, 2, 0}, {0, 1, 2, 3, 0},
    {0, 1, 2, 3, 0}, {0, 2, 2, 3, 0}, {0, 2, 2, 4, 0}, {0, 2, 3, 4, 0},
    {0, 2, 3, 4, 0}, {0, 3, 3, 5, 0}, {0, 3, 4, 6, 0}, {0, 3, 4, 6, 0},
    {0, 4, 5, 7, 0}, {0, 4, 5, 8, 0}, {0, 4, 6, 9, 0}, {0, 5, 7, 10, 0},
    {0, 6, 8, 11, 0}, {0, 6, 8, 13, 0}, {0, 7, 10, 14, 0},
    {0, 8, 11, 16, 0}, {0, 9, 12, 18, 0}, {0, 10, 13, 20, 0},
    {0, 11, 15, 23, 0}, {0, 13, 17, 25, 0}
};
#endif


#ifndef H264DEC_OMXDL
/* mapping of raster scan block index to 4x4 block index */
static const u32 mb4x4Index[16] =
    {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};

typedef struct {
    const u8 *tc0;
    u32 alpha;
    u32 beta;
} edgeThreshold_t;

typedef struct {
    u32 top;
    u32 left;
} bS_t;

enum { TOP = 0, LEFT = 1, INNER = 2 };
#endif /* H264DEC_OMXDL */

#define FILTER_LEFT_EDGE    0x04
#define FILTER_TOP_EDGE     0x02
#define FILTER_INNER_EDGE   0x01


/* clipping table defined in intra_prediction.c */
extern const u8 h264bsdClip[];

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

static u32 InnerBoundaryStrength(mbStorage_t *mb1, u32 i1, u32 i2);

#ifndef H264DEC_OMXDL
static u32 EdgeBoundaryStrength(mbStorage_t *mb1, mbStorage_t *mb2,
    u32 i1, u32 i2);
#else
static u32 InnerBoundaryStrength2(mbStorage_t *mb1, u32 i1, u32 i2);
static u32 EdgeBoundaryStrengthLeft(mbStorage_t *mb1, mbStorage_t *mb2);
static u32 EdgeBoundaryStrengthTop(mbStorage_t *mb1, mbStorage_t *mb2);
#endif

static u32 IsSliceBoundaryOnLeft(mbStorage_t *mb, u32 base);

static u32 IsSliceBoundaryOnTop(mbStorage_t *mb, u32 base);

static u32 GetMbFilteringFlags(mbStorage_t *mb, u32 base);

#ifndef H264DEC_OMXDL

static u32 GetBoundaryStrengths(mbStorage_t *mb, bS_t *bs, u32 flags);

static void FilterLuma(u8 *data, bS_t *bS, edgeThreshold_t *thresholds,
        u32 imageWidth);

static void FilterChroma(u8 *cb, u8 *cr, bS_t *bS, edgeThreshold_t *thresholds,
        u32 imageWidth);

static void FilterVerLumaEdge( u8 *data, u32 bS, edgeThreshold_t *thresholds,
        u32 imageWidth);
static void FilterHorLumaEdge( u8 *data, u32 bS, edgeThreshold_t *thresholds,
        i32 imageWidth);
static void FilterHorLuma( u8 *data, u32 bS, edgeThreshold_t *thresholds,
        i32 imageWidth);

static void FilterVerChromaEdge( u8 *data, u32 bS, edgeThreshold_t *thresholds,
  u32 imageWidth);
static void FilterHorChromaEdge( u8 *data, u32 bS, edgeThreshold_t *thresholds,
  i32 imageWidth);
static void FilterHorChroma( u8 *data, u32 bS, edgeThreshold_t *thresholds,
  i32 imageWidth);

static void GetEdgeThresholds(
  edgeThreshold_t *thresholds,
  const deblockRec_t *rec,
  u32 qpTop,
  u32 qpLeft,
  u32 chroma);

#else /* H264DEC_OMXDL */

static u32 GetBoundaryStrengths(mbStorage_t *mb, u8 (*bs)[16], u32 flags);

static void GetLumaEdgeThresholds(
    mbStorage_t *mb,
    u8 (*alpha)[2],
    u8 (*beta)[2],
    u8 (*threshold)[16],
    u8 (*bs)[16],
    u32 filteringFlags );

static void GetChromaEdgeThresholds(
    mbStorage_t *mb,
    u8 (*alpha)[2],
    u8 (*beta)[2],
    u8 (*threshold)[8],
    u8 (*bs)[16],
    u32 filteringFlags,
    i32 chromaQpIndexOffset);

#endif /* H264DEC_OMXDL */

/*------------------------------------------------------------------------------

    Function: IsSliceBoundaryOnLeft

        Functional description:
            Function to determine if there is a slice boundary on the left side
            of a macroblock. base is the picture's picSliceBase: mb[] is not
            cleared per picture, so ids below it (MBs no slice of this picture
            touched, possible with failed redundant slices) all count as base,
            as the old cleared 0s compared equal.

------------------------------------------------------------------------------*/
u32 IsSliceBoundaryOnLeft(mbStorage_t *mb, u32 base)
{

/* Variables */

/* Code */

    ASSERT(mb && mb->mbA);

    if (MAX(mb->sliceId, base) != MAX(mb->mbA->sliceId, base))
        return(HANTRO_TRUE);
    else
        return(HANTRO_FALSE);

}

/*------------------------------------------------------------------------------

    Function: IsSliceBoundaryOnTop

        Functional description:
            Function to determine if there is a slice boundary above the
            current macroblock. base: see IsSliceBoundaryOnLeft.

------------------------------------------------------------------------------*/
u32 IsSliceBoundaryOnTop(mbStorage_t *mb, u32 base)
{

/* Variables */

/* Code */

    ASSERT(mb && mb->mbB);

    if (MAX(mb->sliceId, base) != MAX(mb->mbB->sliceId, base))
        return(HANTRO_TRUE);
    else
        return(HANTRO_FALSE);

}

/*------------------------------------------------------------------------------

    Function: GetMbFilteringFlags

        Functional description:
          Function to determine which edges of a macroblock has to be
          filtered. Output is a bit-wise OR of FILTER_LEFT_EDGE,
          FILTER_TOP_EDGE and FILTER_INNER_EDGE, depending on which edges
          shall be filtered.

------------------------------------------------------------------------------*/
u32 GetMbFilteringFlags(mbStorage_t *mb, u32 base)
{

/* Variables */

    u32 flags = 0;

/* Code */

    ASSERT(mb);

    /* nothing will be filtered if disableDeblockingFilterIdc == 1 */
    if (mb->disableDeblockingFilterIdc != 1)
    {
        flags |= FILTER_INNER_EDGE;

        /* filterLeftMbEdgeFlag, left mb is MB_A */
        if (mb->mbA &&
            ((mb->disableDeblockingFilterIdc != 2) ||
             !IsSliceBoundaryOnLeft(mb, base)))
            flags |= FILTER_LEFT_EDGE;

        /* filterTopMbEdgeFlag */
        if (mb->mbB &&
            ((mb->disableDeblockingFilterIdc != 2) ||
             !IsSliceBoundaryOnTop(mb, base)))
            flags |= FILTER_TOP_EDGE;
    }

    return(flags);

}

/*------------------------------------------------------------------------------

    Function: InnerBoundaryStrength

        Functional description:
            Function to calculate boundary strength value bs for an inner
            edge of a macroblock. Macroblock type is checked before this is
            called -> no intra mb condition here.

------------------------------------------------------------------------------*/
u32 InnerBoundaryStrength(mbStorage_t *mb1, u32 ind1, u32 ind2)
{
    i32 tmp1, tmp2;
    i32 mv1, mv2, mv3, mv4;

    tmp1 = mb1->totalCoeff[ind1];
    tmp2 = mb1->totalCoeff[ind2];
    mv1 = mb1->mv[ind1].hor;
    mv2 = mb1->mv[ind2].hor;
    mv3 = mb1->mv[ind1].ver;
    mv4 = mb1->mv[ind2].ver;

    if (tmp1 || tmp2)
    {
        return 2;
    }
    else if ( ((u32)ABS(mv1 - mv2) >= 4) || ((u32)ABS(mv3 - mv4) >= 4) ||
              (mb1->refAddr[ind1 >> 2] != mb1->refAddr[ind2 >> 2]) )
    {
        return 1;
    }
    else
        return 0;
}

/*------------------------------------------------------------------------------

    Function: InnerBoundaryStrength2

        Functional description:
            Function to calculate boundary strength value bs for an inner
            edge of a macroblock. The function is the same as
            InnerBoundaryStrength but without checking totalCoeff.

------------------------------------------------------------------------------*/
u32 InnerBoundaryStrength2(mbStorage_t *mb1, u32 ind1, u32 ind2)
{
    i32 tmp1, tmp2, tmp3, tmp4;

    tmp1 = mb1->mv[ind1].hor;
    tmp2 = mb1->mv[ind2].hor;
    tmp3 = mb1->mv[ind1].ver;
    tmp4 = mb1->mv[ind2].ver;

    if ( ((u32)ABS(tmp1 - tmp2) >= 4) || ((u32)ABS(tmp3 - tmp4) >= 4) ||
         (mb1->refAddr[ind1 >> 2] != mb1->refAddr[ind2 >> 2]))
    {
        return 1;
    }
    else
        return 0;
}
#ifndef H264DEC_OMXDL
/*------------------------------------------------------------------------------

    Function: EdgeBoundaryStrength

        Functional description:
            Function to calculate boundary strength value bs for left- or
            top-most edge of a macroblock. Macroblock types are checked
            before this is called -> no intra mb conditions here.

------------------------------------------------------------------------------*/
u32 EdgeBoundaryStrength(mbStorage_t *mb1, mbStorage_t *mb2,
    u32 ind1, u32 ind2)
{

    if (mb1->totalCoeff[ind1] || mb2->totalCoeff[ind2])
    {
        return 2;
    }
    else if ((mb1->refAddr[ind1 >> 2] != mb2->refAddr[ind2 >> 2]) ||
             ((u32)ABS(mb1->mv[ind1].hor - mb2->mv[ind2].hor) >= 4) ||
             ((u32)ABS(mb1->mv[ind1].ver - mb2->mv[ind2].ver) >= 4))
    {
        return 1;
    }
    else
        return 0;
}

#else /* H264DEC_OMXDL */

/*------------------------------------------------------------------------------

    Function: EdgeBoundaryStrengthTop

        Functional description:
            Function to calculate boundary strength value bs for
            top-most edge of a macroblock. Macroblock types are checked
            before this is called -> no intra mb conditions here.

------------------------------------------------------------------------------*/
u32 EdgeBoundaryStrengthTop(mbStorage_t *mb1, mbStorage_t *mb2)
{
    u32 topBs = 0;
    u32 tmp1, tmp2, tmp3, tmp4;

    tmp1 = mb1->totalCoeff[0];
    tmp2 = mb2->totalCoeff[10];
    tmp3 = mb1->totalCoeff[1];
    tmp4 = mb2->totalCoeff[11];
    if (tmp1 || tmp2)
    {
        topBs = 2<<0;
    }
    else if (((u32)ABS(mb1->mv[0].hor - mb2->mv[10].hor) >= 4) ||
             ((u32)ABS(mb1->mv[0].ver - mb2->mv[10].ver) >= 4) ||
             (mb1->refAddr[0] != mb2->refAddr[10 >> 2]))
    {
        topBs = 1<<0;
    }
    tmp1 = mb1->totalCoeff[4];
    tmp2 = mb2->totalCoeff[14];
    if (tmp3 || tmp4)
    {
        topBs += 2<<8;
    }
    else if (((u32)ABS(mb1->mv[1].hor - mb2->mv[11].hor) >= 4) ||
             ((u32)ABS(mb1->mv[1].ver - mb2->mv[11].ver) >= 4) ||
             (mb1->refAddr[0] != mb2->refAddr[11 >> 2]))
    {
        topBs += 1<<8;
    }
    tmp3 = mb1->totalCoeff[5];
    tmp4 = mb2->totalCoeff[15];
    if (tmp1 || tmp2)
    {
        topBs += 2<<16;
    }
    else if (((u32)ABS(mb1->mv[4].hor - mb2->mv[14].hor) >= 4) ||
             ((u32)ABS(mb1->mv[4].ver - mb2->mv[14].ver) >= 4) ||
             (mb1->refAddr[4 >> 2] != mb2->refAddr[14 >> 2]))
    {
        topBs += 1<<16;
    }
    if (tmp3 || tmp4)
    {
        topBs += 2<<24;
    }
    else if (((u32)ABS(mb1->mv[5].hor - mb2->mv[15].hor) >= 4) ||
             ((u32)ABS(mb1->mv[5].ver - mb2->mv[15].ver) >= 4) ||
             (mb1->refAddr[5 >> 2] != mb2->refAddr[15 >> 2]))
    {
        topBs += 1<<24;
    }

    return topBs;
}

/*------------------------------------------------------------------------------

    Function: EdgeBoundaryStrengthLeft

        Functional description:
            Function to calculate boundary strength value bs for left-
            edge of a macroblock. Macroblock types are checked
            before this is called -> no intra mb conditions here.

------------------------------------------------------------------------------*/
u32 EdgeBoundaryStrengthLeft(mbStorage_t *mb1, mbStorage_t *mb2)
{
    u32 leftBs = 0;
    u32 tmp1, tmp2, tmp3, tmp4;

    tmp1 = mb1->totalCoeff[0];
    tmp2 = mb2->totalCoeff[5];
    tmp3 = mb1->totalCoeff[2];
    tmp4 = mb2->totalCoeff[7];

    if (tmp1 || tmp2)
    {
        leftBs = 2<<0;
    }
    else if (((u32)ABS(mb1->mv[0].hor - mb2->mv[5].hor) >= 4) ||
             ((u32)ABS(mb1->mv[0].ver - mb2->mv[5].ver) >= 4) ||
             (mb1->refAddr[0] != mb2->refAddr[5 >> 2]))
    {
        leftBs = 1<<0;
    }
    tmp1 = mb1->totalCoeff[8];
    tmp2 = mb2->totalCoeff[13];
    if (tmp3 || tmp4)
    {
        leftBs += 2<<8;
    }
    else if (((u32)ABS(mb1->mv[2].hor - mb2->mv[7].hor) >= 4) ||
             ((u32)ABS(mb1->mv[2].ver - mb2->mv[7].ver) >= 4) ||
             (mb1->refAddr[0] != mb2->refAddr[7 >> 2]))
    {
        leftBs += 1<<8;
    }
    tmp3 = mb1->totalCoeff[10];
    tmp4 = mb2->totalCoeff[15];
    if (tmp1 || tmp2)
    {
        leftBs += 2<<16;
    }
    else if (((u32)ABS(mb1->mv[8].hor - mb2->mv[13].hor) >= 4) ||
             ((u32)ABS(mb1->mv[8].ver - mb2->mv[13].ver) >= 4) ||
             (mb1->refAddr[8 >> 2] != mb2->refAddr[13 >> 2]))
    {
        leftBs += 1<<16;
    }
    if (tmp3 || tmp4)
    {
        leftBs += 2<<24;
    }
    else if (((u32)ABS(mb1->mv[10].hor - mb2->mv[15].hor) >= 4) ||
             ((u32)ABS(mb1->mv[10].ver - mb2->mv[15].ver) >= 4) ||
             (mb1->refAddr[10 >> 2] != mb2->refAddr[15 >> 2]))
    {
        leftBs += 1<<24;
    }

    return leftBs;
}
#endif /* H264DEC_OMXDL */
#ifndef H264DEC_OMXDL
/*------------------------------------------------------------------------------

    Function: h264bsdDeblockRecord

        Functional description:
          Store in rec what h264bsdFilterPicture needs of one macroblock:
          the boundary strengths that its filtering flags and its own and
          its left and top neighbours' storage give, packed, and the inputs
          of its edge thresholds. Called when the macroblock is decoded,
          while that storage is in cache, and by the pass itself when the
          picture's records may be stale.

------------------------------------------------------------------------------*/
void h264bsdDeblockRecord(mbStorage_t *mb, deblockRec_t *rec,
  u32 picSliceBase, u32 justDecoded)
{

/* Variables */

    u32 i, flags;
    u32 top = 0, left = 0;
    bS_t bS[16];

/* Code */

    ASSERT(mb);
    ASSERT(rec);

    flags = GetMbFilteringFlags(mb, picSliceBase);

    /* P_Skip just decoded (justDecoded): totalCoeff is all 0 and the 16 mvs
     * and 4 refAddrs are one value, so the inner edges are bS 0 and only
     * the MB edges need a look, with the same tests as GetBoundaryStrengths.
     * Not for storage of unknown history (a failed decode leaves stale mvs) */
    if (justDecoded && flags && mb->mbType == P_Skip)
    {
        if (flags & FILTER_TOP_EDGE)
        {
            mbStorage_t *b = mb->mbB;

            if (IS_INTRA_MB(*b))
                top = 0xFF;
            else
                top = EdgeBoundaryStrength(mb, b, 0, 10) |
                    (EdgeBoundaryStrength(mb, b, 1, 11) << 2) |
                    (EdgeBoundaryStrength(mb, b, 4, 14) << 4) |
                    (EdgeBoundaryStrength(mb, b, 5, 15) << 6);
        }
        if (flags & FILTER_LEFT_EDGE)
        {
            mbStorage_t *a = mb->mbA;

            if (IS_INTRA_MB(*a))
                left = (3 << 0) | (3 << 8) | (3 << 16) | (3 << 24);
            else
                left = EdgeBoundaryStrength(mb, a, 0, 5) |
                    (EdgeBoundaryStrength(mb, a, 2, 7) << 8) |
                    (EdgeBoundaryStrength(mb, a, 8, 13) << 16) |
                    (EdgeBoundaryStrength(mb, a, 10, 15) << 24);
        }
    }
    /* GetBoundaryStrengths function returns non-zero value if any of
     * the bS values for the macroblock being processed was non-zero */
    else if (flags && GetBoundaryStrengths(mb, bS, flags))
    {
        for (i = 16; i--;)
        {
            /* bS 4 (MB edges only) is code 3 */
            top = (top << 2) | (bS[i].top - (bS[i].top >> 2));
            left = (left << 2) | (bS[i].left - (bS[i].left >> 2));
        }
    }

    rec->bs[0] = top;
    rec->bs[1] = left;
    rec->qp = (u8)mb->qpY;
    rec->offA = (i8)mb->filterOffsetA;
    rec->offB = (i8)mb->filterOffsetB;
    rec->chromaQpOff = (i8)mb->chromaQpIndexOffset;

}

#ifdef VJ_DEBLOCK_WORKER
/* Words of undo log per macroblock: what its filtering may modify. Luma: 3
 * columns left of the MB (in an aligned word per line), the 16 x 16 pixels,
 * 3 lines above. Chroma, per plane: 1 column, 8 x 8, 1 line. */
#define DBW_SLOT_WORDS 144
#define DBW_NOBANK 0xFF
#define DBW_L_LEFT 0
#define DBW_L_OWN 16
#define DBW_L_TOP 80
#define DBW_C_BASE 92
#define DBW_C_SIZE 26 /* left 8 words, own 16, top 2 */

static void SaveFootprint(u32 *s, image_t *image, u32 mbRow, u32 mbCol)
{
    u32 W = image->width, N = W * image->height, k, j;
    u32 *p, *q;
    u32 stride = W * 16;

    /* luma */
    p = (u32*)(image->data + mbRow * 16 * stride + mbCol * 16);
    stride >>= 2; /* words */
    for (k = 0; k < 16; k++)
    {
        q = p + k * stride;
        if (mbCol)
            s[DBW_L_LEFT + k] = q[-1];
        s[DBW_L_OWN + 4*k + 0] = q[0];
        s[DBW_L_OWN + 4*k + 1] = q[1];
        s[DBW_L_OWN + 4*k + 2] = q[2];
        s[DBW_L_OWN + 4*k + 3] = q[3];
    }
    if (mbRow)
        for (k = 0; k < 3; k++)
        {
            q = p + ((i32)k - 3) * (i32)stride;
            s[DBW_L_TOP + 4*k + 0] = q[0];
            s[DBW_L_TOP + 4*k + 1] = q[1];
            s[DBW_L_TOP + 4*k + 2] = q[2];
            s[DBW_L_TOP + 4*k + 3] = q[3];
        }

    /* chroma */
    for (j = 0; j < 2; j++)
    {
        u32 *t = s + DBW_C_BASE + j * DBW_C_SIZE;

        stride = W * 8;
        p = (u32*)(image->data + N * 256 + j * N * 64 +
            mbRow * 8 * stride + mbCol * 8);
        stride >>= 2;
        for (k = 0; k < 8; k++)
        {
            q = p + k * stride;
            if (mbCol)
                t[k] = q[-1];
            t[8 + 2*k + 0] = q[0];
            t[8 + 2*k + 1] = q[1];
        }
        if (mbRow)
        {
            q = p - (i32)stride;
            t[24] = q[0];
            t[25] = q[1];
        }
    }
}

/* SaveFootprint, backwards */
static void RestoreFootprint(const u32 *s, image_t *image, u32 mbRow,
    u32 mbCol)
{
    u32 W = image->width, N = W * image->height, k, j;
    u32 *p, *q;
    u32 stride = W * 16;

    p = (u32*)(image->data + mbRow * 16 * stride + mbCol * 16);
    stride >>= 2;
    for (k = 0; k < 16; k++)
    {
        q = p + k * stride;
        if (mbCol)
            q[-1] = s[DBW_L_LEFT + k];
        q[0] = s[DBW_L_OWN + 4*k + 0];
        q[1] = s[DBW_L_OWN + 4*k + 1];
        q[2] = s[DBW_L_OWN + 4*k + 2];
        q[3] = s[DBW_L_OWN + 4*k + 3];
    }
    if (mbRow)
        for (k = 0; k < 3; k++)
        {
            q = p + ((i32)k - 3) * (i32)stride;
            q[0] = s[DBW_L_TOP + 4*k + 0];
            q[1] = s[DBW_L_TOP + 4*k + 1];
            q[2] = s[DBW_L_TOP + 4*k + 2];
            q[3] = s[DBW_L_TOP + 4*k + 3];
        }

    for (j = 0; j < 2; j++)
    {
        const u32 *t = s + DBW_C_BASE + j * DBW_C_SIZE;

        stride = W * 8;
        p = (u32*)(image->data + N * 256 + j * N * 64 +
            mbRow * 8 * stride + mbCol * 8);
        stride >>= 2;
        for (k = 0; k < 8; k++)
        {
            q = p + k * stride;
            if (mbCol)
                q[-1] = t[k];
            q[0] = t[8 + 2*k + 0];
            q[1] = t[8 + 2*k + 1];
        }
        if (mbRow)
        {
            q = p - (i32)stride;
            q[0] = t[24];
            q[1] = t[25];
        }
    }
}

#endif /* VJ_DEBLOCK_WORKER */

/* How FilterMbRows marks the MBs it changes (skip elision). The picture's own
 * map takes 1 for every mark. The worker's map keeps the three kinds apart, so
 * that what a rolled-back row marked can be told from the rest:
 * DBW_MARK_OWN the MB was filtered with edges (and has a logged footprint if
 * its row has a log bank), DBW_MARK_LEFT its right neighbour filtered across
 * their edge, DBW_MARK_TOP the MB below did. */
#define DBW_MARK_OWN  1
#define DBW_MARK_LEFT 2
#define DBW_MARK_TOP  4

/*------------------------------------------------------------------------------

    Function: FilterMbRows

        Functional description:
          Filter macroblocks mbFrom .. mbTo - 1 of a picture, in raster order
          (the body of h264bsdFilterPicture, which filters them all). When
          log is not NULL every macroblock that gets filtered first saves the
          pixels its filtering can modify (SaveFootprint), so that
          UndoRows can put them back.

          The marks of skip elision go to changed (bitMarks: the worker's
          kinds of marks, else 1). Filtering row r reads and modifies only
          rows r - 1 and r of the picture, the records of row r (and the qps
          of row r - 1), and (recValid false only) the storage of rows r - 1
          and r.

          The records: recValid true reads them from rec, recRows rows of
          picWidthInMbs in a ring (row r in slot row r % recRows); false
          makes each from the storage as the pass always did, with the
          neighbours' qps from the storage too (the record's qp is the
          storage's qpY). The qps of the row above: from qpUp, one per
          column, which the worker's run keeps as it filters (so row r - 1 is
          not read again), else from the records of row r - 1 or the storage.

------------------------------------------------------------------------------*/
static inline __attribute__((always_inline)) void FilterMbRows(
  image_t *image,
  mbStorage_t *mb, u32 picSliceBase,
  u8 *changed, u32 bitMarks, u8 *qpUp,
  const deblockRec_t *rec, u32 recRows, u32 recValid,
  u32 mbFrom, u32 mbTo, const u8 *rowBank, u32 *log)
{

/* Variables */

    u32 i, n, top, left, qpTop, qpLeft;
    u32 picSizeInMbs, mbRow, mbCol;
    u32 picWidthInMbs;
    u8 *data;
    const deblockRec_t *r;
    const deblockRec_t *rowRec = NULL, *upRec = NULL;
    deblockRec_t made;
    bS_t bS[16];
    edgeThreshold_t thresholds[3];

/* Code */

    picWidthInMbs = image->width;
    picSizeInMbs = picWidthInMbs * image->height;

    /* VideoJet: the range is in macroblocks (raster order), so the deblock
     * worker can filter a few at a time and parse in between */
    mbRow = mbFrom / picWidthInMbs;
    mbCol = mbFrom % picWidthInMbs;
    for (n = mbFrom; n < mbTo; n++)
    {
        /* records made at decode time hold only if the picture's MBs were
         * decoded once each in raster order (h264bsdDecodeSliceData) and the
         * ring still holds them. Otherwise make each from the storage now,
         * as the pass always did */
        if (recValid)
        {
            if (mbCol == 0 || n == mbFrom)
            {
                rowRec = rec + (size_t)(mbRow % recRows) * picWidthInMbs;
                if (!qpUp)
                    upRec = mbRow ?
                        rec + (size_t)((mbRow - 1) % recRows) * picWidthInMbs :
                        rowRec;
            }
            r = rowRec + mbCol;
        }
        else
        {
            h264bsdDeblockRecord(mb + n, &made, picSliceBase, 0);
            r = &made;
        }

        top = r->bs[0];
        left = r->bs[1];

        /* nothing to filter: neither storage nor pixels are touched */
        if (top | left)
        {
            /* skip elision: mark every MB this filtering may modify,
             * itself and, across an MB edge with bS > 0, its left or
             * top neighbour. Chroma edges use these luma bS */
            u8 *c = changed + n;

            for (i = 0; i < 16; i++, top >>= 2, left >>= 2)
            {
                bS[i].top = top & 3;
                bS[i].left = left & 3;
            }
            for (i = 0; i < 4; i++)
            {
                if (bS[i].top == 3)
                    bS[i].top = 4;
                if (bS[4*i].left == 3)
                    bS[4*i].left = 4;
            }

            /* the worker's map is clear when its own mark is made: the
             * neighbours' come later in raster order */
            c[0] = 1;
            if (bS[0].left | bS[4].left | bS[8].left | bS[12].left)
            {
                if (bitMarks)
                    c[-1] |= DBW_MARK_LEFT;
                else
                    c[-1] = 1;
            }
            if (bS[0].top | bS[1].top | bS[2].top | bS[3].top)
            {
                if (bitMarks)
                    c[-(i32)picWidthInMbs] |= DBW_MARK_TOP;
                else
                    c[-(i32)picWidthInMbs] = 1;
            }

            /* at the picture border the neighbour's qp is never used, as
             * that edge is not filtered */
            if (qpUp)
            {
                qpTop = mbRow ? qpUp[mbCol] : r->qp;
                qpLeft = mbCol ? r[-1].qp : r->qp;
            }
            else if (recValid)
            {
                qpTop = upRec[mbCol].qp;
                qpLeft = mbCol ? r[-1].qp : r->qp;
            }
            else
            {
                qpTop = mbRow ? (u8)mb[n - picWidthInMbs].qpY : r->qp;
                qpLeft = mbCol ? (u8)mb[n - 1].qpY : r->qp;
            }

#ifdef VJ_DEBLOCK_WORKER
            if (log && rowBank[mbRow] != DBW_NOBANK)
                SaveFootprint(log + ((size_t)rowBank[mbRow] *
                    picWidthInMbs + mbCol) * DBW_SLOT_WORDS, image,
                    mbRow, mbCol);
#endif

            /* luma */
            GetEdgeThresholds(thresholds, r, qpTop, qpLeft, 0);
            data = image->data + mbRow * picWidthInMbs * 256 + mbCol * 16;

            FilterLuma((u8*)data, bS, thresholds, picWidthInMbs*16);

            /* chroma */
            GetEdgeThresholds(thresholds, r, qpTop, qpLeft, 1);
            data = image->data + picSizeInMbs * 256 +
                mbRow * picWidthInMbs * 64 + mbCol * 8;

            FilterChroma((u8*)data, data + 64*picSizeInMbs, bS,
                    thresholds, picWidthInMbs*8);
        }

        /* the qp of this MB is the one above for the MB below it */
        if (qpUp)
            qpUp[mbCol] = r->qp;

        mbCol++;
        if (mbCol == picWidthInMbs)
        {
            mbCol = 0;
            mbRow++;
        }
    }

}

/*------------------------------------------------------------------------------

    Function: h264bsdFilterPicture

        Functional description:
          Perform deblocking filtering for a picture. Filter does not copy
          the original picture anywhere but filtering is performed directly
          on the original image. Parameters controlling the filtering process
          come from the deblock records (h264bsdDeblockRecord), computed
          from macroblock structures of the filtered macroblock, macroblock
          above and macroblock on the left of the filtered one.

        Inputs:
          image         pointer to image to be filtered
          mb            pointer to macroblock data structure of the top-left
                        macroblock of the picture
          picSliceBase  slice id base of the picture (IsSliceBoundaryOnLeft)
          changed       per-MB change flags (skip elision), set for every MB
                        filtering may modify
          rec           deblock records of the picture's macroblocks: recRows
                        rows of picWidthInMbs, row r in slot r % recRows (all
                        rows of the picture, or the ring of VJ_DEBLOCK_WORKER)
          recValid      HANTRO_TRUE if the records were made at decode time
                        and the rows from rowFrom - 1 on are still in rec,
                        otherwise they are made here
          rowFrom       first macroblock row to filter: the rows before it
                        were filtered already (deblock worker)

        Outputs:
          image         filtered image stored here

        Returns:
          none

------------------------------------------------------------------------------*/
void h264bsdFilterPicture(
  image_t *image,
  mbStorage_t *mb, u32 picSliceBase,
  u8 *changed, deblockRec_t *rec, u32 recRows, u32 recValid, u32 rowFrom)
{

/* Code */

    ASSERT(image);
    ASSERT(mb);
    ASSERT(rec);
    ASSERT(recRows);
    ASSERT(image->data);
    ASSERT(image->width);
    ASSERT(image->height);

    FilterMbRows(image, mb, picSliceBase, changed, 0, NULL, rec, recRows,
        recValid, rowFrom * image->width, image->width * image->height, NULL,
        NULL);

}

#ifdef VJ_DEBLOCK_WORKER
/*------------------------------------------------------------------------------

    Row-lagged deblock worker (VJ_DEBLOCK_WORKER)

    The decoding thread decodes the picture; a worker thread filters macroblock
    row r once rows 0 .. r + 1 are decoded, in raster order, with the records
    made at decode time (kept in a ring of rows, see 4.).

    1. Bit-exact while nothing goes wrong. Intra prediction reads only
       unfiltered pixels of its own slice: the rows above (their bottom line,
       the above-right MB) and the MBs to the left. Filtering row r changes
       pixels of rows r - 1 and r only, and the decoder reads row r's pixels
       only while it decodes row r + 1, which is complete before row r is
       filtered. Inter prediction reads other pictures. The worker's input
       for row r is what the sequential pass sees: rows 0 .. r - 1 filtered,
       row r unfiltered, the records as made at decode time. It touches
       memory the decoder does not write meanwhile: pixels of rows r - 1 and
       r, records of rows r - 1 and r, its own marks (skip elision; they join
       the picture's when it ends) and its own undo log.

    2. Bit-exact when something does. The sequential decoder, after a slice
       error in slice B (first MB s), holds every MB of B as damaged (decoded
       count 0, as MarkSliceCorrupted leaves a P slice) and the MBs not yet
       decoded after it, all at addresses >= s; the damaged MBs are rewritten
       whole, by a later slice or by concealment, before the picture is
       filtered. What the worker filtered must not show in what the
       sequential decoder would hold then:
         - row r's footprint is rows r - 1 and r. If its lowest MB address
           (that of row r - 1, or 0) is >= s, every pixel it modified is in
           a damaged MB, which is rewritten: nothing to put back. Single
           slice pictures (s = 0) are always this case.
         - else it modified pixels of MBs before s, which concealment may
           read and the final pass must see unfiltered: its footprints are
           logged before filtering (SaveFootprint, a bank per such row, at
           most DBW_BANKS of them per slice: a row that would need a fifth
           is not posted, so it is filtered when the picture ends).
       On a slice error in B, h264bsdDbwAbortError waits for the worker, puts
       back the logged rows posted since B opened (r0 of them are older: their
       footprints are before s, as the rows posted are at most row(s) - 1
       when B opens, and those MBs stay as they are), and the final pass
       resumes at row r0. The marks of the rows below r0 are kept, those the
       undone rows made are dropped (the worker's map keeps own, left and top
       marks apart for that, see DBW_MARK_*).
       Other slices of the picture cannot disturb filtered rows: a primary
       slice that starts on an MB already decoded fails before it writes.
       Slices that are not P slices (I: concealment of an I slice reads the
       decoded MBs next to the damage) are decoded with no worker posts.
       Concealment of lost MBs at the end of a picture (h264bsdDbwAbortKeep)
       only touches MBs at or after vjRecNext, rows after those posted.
    3. No worker at all for pictures with slice groups or redundant slices
       (h264bsdDbwBegin): they re-decode MBs that have already been filtered.
    4. The records are a ring of K = VJ_REC_ROWS rows (recRows, fewer for a
       small picture): the record of MB n is slot n % (K * width), ring row
       r % K holds row r. The decoding thread writes them in raster order
       (vjRecNext); the worker, filtering row j, reads the records of row j
       only (the qps of row j - 1 it keeps itself, qpUp), and nobody reads
       them otherwise except the final pass.
       Records stay a pure function of the storage as the MB was decoded, so
       what is not in the ring can be made again from the storage: the final
       pass does that for every row from the first it filters on, unless the
       ring still holds all of them (RingHolds: every ring row carries the
       row and the picture id it was filled for).
       - The decoding thread starts row r (end of row r - 1, h264bsdDbwRowEnd)
         by writing over row r - K, so the worker must be done with it. After
         posting what became postable the decoding thread waits until the
         worker's published position (fmb, a store-release after every
         segment) is in row r - K + 1 or later (DbwWaitRows), then tags ring
         row r % K. Only posted rows are waited for. Posting a row needs its
         ring row to survive the row about to start; a row that can no longer
         be posted never can be, as the ring only moves on: the worker is out
         of the picture for good ("lost": the rows already posted are
         filtered, the final pass remakes the rest). It happens when rows go
         unposted for K - 2 rows: the rows of I slices, a slice that needs
         more than DBW_BANKS log banks, a worker that did not run for
         VJ_WORKER_WAIT_MS.
       - Progress. The decoding thread waits only for posted rows, and the
         worker's task does deblock work whenever its parser has nothing to
         do: while the decoding thread waits it frees no parse slot, so the
         parse ring is full after VJ_RING_DEPTH records and the task runs
         deblock segments; a task asleep on a full parse ring is woken by the
         post that precedes the wait. The worker never waits for the
         decoding thread (DbwRun has no wait, the parser only yields to it),
         so there is no cycle. A worker that is not scheduled at all ends the
         wait like any other: after VJ_WORKER_WAIT_MS the decoding thread
         takes the posted run back and filters the rows itself.
       - Marks. The map of the worker has three marks (DBW_MARK_*), so that
         neither the undo of a slice error nor the rebuild after it needs
         the records of rows the ring no longer holds.

------------------------------------------------------------------------------*/

/* log banks: one row's footprints each. A slice logs at most its first rows */
#define DBW_BANKS 4

struct dbw
{
    vj_worker_t *w;
    image_t img;        /* the picture being filtered */
    deblockRec_t *rec;  /* the record ring: recRows rows of picture width */
    u32 recRows;
    u8 *changed;        /* the picture's skip elision flags */
    u8 *marks;          /* the worker's marks (DBW_MARK_*), joined into changed */
    u8 *rowBank;        /* per row: log bank, DBW_NOBANK for none */
    u8 *qpUp;           /* per column: the qp of the last MB the worker filtered there */
    u32 *log;           /* DBW_BANKS * picture width footprints */
    u32 capMbs, capW;   /* what marks, rowBank and log have room for */
    u32 active;         /* the worker takes part in this picture */
    u32 resume;         /* rows filtered when it is not active */
    /* the decoding thread's */
    u32 rowsComplete;   /* rows decoded in raster order */
    u32 nextRowEnd;     /* vjRecNext at which the next row is complete */
    u32 posted;         /* rows handed to the worker */
    u32 sOpen;          /* first MB of the slice being decoded */
    u32 postOk;         /* that slice may post rows (P, in raster order) */
    u32 r0;             /* rows posted when it opened */
    u32 banksUsed;
    u32 lost;           /* a row could not be posted before the ring moved on: no more posts */
    u32 pic;            /* picture id (every Begin): what the ring tags mean */
    struct { u32 pic, row; } tag[VJ_REC_ROWS]; /* what each ring row was filled for */
    /* the worker's while it runs, the decoding thread's when it is idle */
    u32 filtered;       /* rows filtered */
    u32 fmb;            /* next macroblock to filter (a run in progress); the decoding
                           thread reads it (acquire) to see which ring rows are free */
    /* counters (tests, stats builds): row waits of the decoding thread, pictures the worker
     * got out of, final passes that remade records the ring did not hold */
    unsigned nRecWait, nLost, nRemade;
#ifdef VJ_DBW_STATS
    unsigned nBegin, nFinish, nErr, nErrUndo, nKeep, nWaitUs;
#endif
};

/* Filter macroblocks per call of the worker's run, so the worker can parse
 * between the pieces (~80 us on the S3) */
#ifndef VJ_DBW_SEG_MBS
#define VJ_DBW_SEG_MBS 10
#endif

#ifdef VJ_DBW_SEG_DELAY_US /* host test: a slow worker, so the decoding thread runs ahead and waits for ring room */
#include <unistd.h>
static unsigned DbwDelay(void)
{
    static __thread unsigned x = 0x2545F491u;

    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}
#endif

/* the worker: filter the rows posted since the last call, a segment at a time.
 * Returns 1 when the run is complete (filtered == upto), 0 when it wants to be
 * called again with the same upto. fmb is the next macroblock to filter; it is
 * filtered * width whenever no run is in progress. It is published after every
 * segment (release): the decoding thread reads it to know which rows of the
 * record ring the worker is done with. */
static int DbwRun(void *arg, uint32_t upto)
{
    dbw_t *d = (dbw_t*)arg;
    u32 W = d->img.width, end, to, fmb;

    if (upto <= d->filtered)
        return 1;
    fmb = __atomic_load_n(&d->fmb, __ATOMIC_RELAXED);
    to = upto * W;
    end = fmb + VJ_DBW_SEG_MBS;
    if (end > to)
        end = to;
#ifdef VJ_DBW_SEG_DELAY_US
    if (DbwDelay() % 4 == 0)
        usleep(DbwDelay() % VJ_DBW_SEG_DELAY_US);
#endif
    FilterMbRows(&d->img, NULL, 0, d->marks, 1, d->qpUp, d->rec, d->recRows,
        HANTRO_TRUE, fmb, end, d->rowBank, d->log);
    __atomic_store_n(&d->fmb, end, __ATOMIC_RELEASE);
    if (end < to)
        return 0;
    d->filtered = upto;
    return 1;
}

/* ring row of row r is filled for this picture, from now on (its first record
 * is written next) */
static void TagRow(dbw_t *d, u32 row)
{
    d->tag[row % d->recRows].pic = d->pic;
    d->tag[row % d->recRows].row = row;
}

/* The ring holds the records of rows from .. height - 1 of this picture: more
 * than the ring has rows are not, and each ring row must be the one filled for
 * the row (and this picture) */
static __attribute__((noinline)) u32 RingHolds(const dbw_t *d, u32 height,
    u32 from)
{
    u32 r;

    if (from >= height)
        return HANTRO_TRUE;
    if (height - from > d->recRows)
        return HANTRO_FALSE;
    for (r = from; r < height; r++)
        if (d->tag[r % d->recRows].pic != d->pic ||
            d->tag[r % d->recRows].row != r)
            return HANTRO_FALSE;
    return HANTRO_TRUE;
}

/* The worker takes no more rows of this picture */
static void Lose(dbw_t *d)
{
    if (!d->lost)
    {
        d->lost = 1;
        d->nLost++;
    }
}

/* The decoding thread waits until the worker has filtered rows 0 .. rows - 1
 * (its published position is in row rows or later). A worker that does not
 * run is taken over by the decoding thread (vj_worker_wait_progress) and
 * dropped from the picture */
static void DbwWaitRows(dbw_t *d, u32 rows)
{
    u32 need = rows * d->img.width;

    if (__atomic_load_n(&d->fmb, __ATOMIC_ACQUIRE) >= need)
        return;
    d->nRecWait++;
#ifdef VJ_DBW_STATS
    {
        uint32_t t0 = vj_ticks();

        if (vj_worker_wait_progress(d->w, &d->fmb, need))
            Lose(d);
        d->nWaitUs += (unsigned)(vj_ticks() - t0);
    }
#else
    if (vj_worker_wait_progress(d->w, &d->fmb, need))
        Lose(d);
#endif
}

dbw_t *h264bsdDbwCreate(void)
{
    dbw_t *d = (dbw_t*)calloc(1, sizeof(dbw_t));

    if (d)
    {
        d->w = vj_worker_create(DbwRun, d);
        if (!d->w)
        {
            free(d);
            d = NULL;
        }
    }
    return d;
}

/* The worker's task also runs the parser (VJ_PARSE_PIPELINE): step is called
 * between deblock segments, kick wakes the task when a slice is armed */
void h264bsdDbwSetStep(dbw_t *d, int (*step)(void *, int), void *arg)
{
    if (d)
        vj_worker_set_step(d->w, step, arg);
}

void h264bsdDbwKick(dbw_t *d)
{
    if (d)
        vj_worker_kick(d->w);
}

/* the worker announced a sleep on a full ring: a slot was freed */
void h264bsdDbwWake(dbw_t *d)
{
    if (d)
        vj_worker_wake(d->w);
}

/* tests and profile builds: slice arms, ring-full sleeps and wakes of the worker */
void h264bsdDbwStats(dbw_t *d, unsigned *kicks, unsigned *sleeps, unsigned *wakes)
{
    vj_worker_stats(d->w, kicks, sleeps, wakes);
}

/* the worker runs on another core than the calling task */
u32 h264bsdDbwParallel(dbw_t *d)
{
    return d && vj_worker_parallel(d->w);
}

void h264bsdDbwDestroy(dbw_t *d)
{
    if (d)
    {
#ifdef VJ_DBW_STATS
        fprintf(stderr, "dbw: %u pictures armed, %u finished by the worker, "
            "%u slice errors (%u with logged rows to undo), %u kept at a "
            "concealment; ring: %u waits (%u us), %u pictures the worker "
            "left, %u final passes remade records\n", d->nBegin, d->nFinish,
            d->nErr, d->nErrUndo, d->nKeep, d->nRecWait, d->nWaitUs,
            d->nLost, d->nRemade);
#endif
        vj_worker_destroy(d->w);
        free(d->marks);
        free(d->log);
        free(d);
    }
}

/* Start of a picture: decide whether the worker takes part and arm it. The
 * worker is idle here. Returns the vjRecNext value at which the first row is
 * complete, 0 when the worker is not taking part */
u32 h264bsdDbwBegin(dbw_t *d, image_t *image, deblockRec_t *rec, u32 recRows,
    u8 *changed, u32 numSliceGroups, u32 redundantPicCntPresent)
{
    u32 n = image->width * image->height;

    h264bsdDbwAbortKeep(d);
    d->resume = 0;
    d->filtered = 0;
    __atomic_store_n(&d->fmb, 0, __ATOMIC_RELAXED);
    d->posted = 0;
    /* a new id: no ring row is tagged for this picture yet (also when the
     * worker takes no part, so its tags never pass for this picture's) */
    d->pic++;
    d->lost = 0;
#ifdef VJ_REC_REMAKE /* test: the worker takes no rows, so the final pass makes every record from the storage */
    d->lost = 1;
#endif
    d->recRows = recRows;
    if (numSliceGroups != 1 || redundantPicCntPresent ||
        image->height < 3 || ((uintptr_t)image->data & 3))
        return 0;
    if (d->capMbs != n || d->capW != image->width)
    {
        free(d->marks);
        free(d->log);
        d->capMbs = d->capW = 0;
        d->marks = (u8*)vj_alloc_psram(n + image->height + image->width);
        d->log = (u32*)vj_alloc_psram((size_t)DBW_BANKS * image->width *
            DBW_SLOT_WORDS * 4);
        if (!d->marks || !d->log)
        {
            free(d->marks);
            free(d->log);
            d->marks = NULL;
            d->log = NULL;
            vj_worker_warn("no memory for the deblock worker's undo log: filtering in the pass");
            return 0;
        }
        d->capMbs = n;
        d->capW = image->width;
        memset(d->marks, 0, n);
    }
    d->rowBank = d->marks + n;
    d->qpUp = d->rowBank + image->height;
    d->img = *image;
    d->rec = rec;
    d->changed = changed;
    d->active = HANTRO_TRUE;
    TagRow(d, 0);
    d->rowsComplete = 0;
    d->nextRowEnd = image->width;
    d->sOpen = 0;
    d->postOk = HANTRO_FALSE;
    d->r0 = 0;
    d->banksUsed = 0;
#ifdef VJ_DBW_STATS
    d->nBegin++;
#endif
    return d->nextRowEnd;
}

/* A slice is about to be decoded; recNext is vjRecNext */
void h264bsdDbwSliceStart(dbw_t *d, u32 firstMb, u32 isP, u32 recNext)
{
    if (d && d->active)
    {
        d->sOpen = firstMb;
        d->postOk = isP && firstMb == recNext;
        d->r0 = d->posted;
        d->banksUsed = 0;
    }
}

/* vjRecNext reached the value Begin or the last RowEnd returned: another row
 * is complete, in raster order, and its records are made. The rows before the
 * last complete one are now safe to filter. Returns the next such value */
u32 h264bsdDbwRowEnd(dbw_t *d)
{
    u32 W = d->img.width, H = d->img.height, K = d->recRows;
    u32 before = d->posted, next, held, lowRow;

    /* rows 0 .. next - 1 are complete; row next is the next to be written
     * (none after the last), over ring row next - K */
    next = ++d->rowsComplete;
    d->nextRowEnd += W;
    /* the lowest row the ring will hold once row next is started */
    held = next < H ? next + 1 : H;
    lowRow = held > K ? held - K : 0;

    if (d->postOk && !d->lost)
    {
        while (d->posted + 1 < next)
        {
            u32 r = d->posted;
            /* lowest MB the filtering of row r modifies: the first of the
             * row above */
            u32 lowest = r ? (r - 1) * W : 0;

            /* row r needs its records */
            if (r < lowRow)
                break;
            if (lowest < d->sOpen)
            {
                if (d->banksUsed == DBW_BANKS)
                    break;
                d->rowBank[r] = (u8)d->banksUsed++;
            }
            else
                d->rowBank[r] = DBW_NOBANK;
            d->posted++;
        }
        if (d->posted != before)
            vj_worker_post(d->w, d->posted);
    }
    /* the row that is next to be posted is gone from the ring, so every later
     * one is too: the worker is out of this picture */
    if (d->posted < lowRow)
        Lose(d);
    if (next < H)
    {
        /* row next is about to overwrite row next - K, which the worker reads
         * while it filters it, if it was posted: wait for that */
        if (next >= K && d->posted > next - K)
        {
            VJP_BEGIN(VJP_DEBLOCK);
            DbwWaitRows(d, next - K + 1);
            VJP_END(VJP_DEBLOCK);
        }
        TagRow(d, next);
    }
    return d->nextRowEnd;
}

/* For tests: a posted run has not finished */
u32 h264bsdDbwBusy(dbw_t *d)
{
    return d && vj_worker_busy(d->w);
}

u32 h264bsdDbwActive(const dbw_t *d)
{
    return d && d->active;
}

/* The worker is idle and filtered rows [0, keep) stay filtered: the marks of
 * the rest are dropped, those the kept rows made go to the picture's, and the
 * final pass resumes at keep. Filtering row r marks MBs of row r (own, left)
 * and of row r - 1 (top), so the kept marks are the own and left ones of rows
 * below keep and the top ones of rows below keep - 1 (those of row keep - 1
 * come from filtering row keep). */
static void DbwSettle(dbw_t *d, u32 keep)
{
    u32 W = d->img.width, n;

    for (n = 0; n < keep * W; n++)
    {
        u8 m = d->marks[n];

        if ((m & (DBW_MARK_OWN | DBW_MARK_LEFT)) ||
            ((m & DBW_MARK_TOP) && n < (keep - 1) * W))
            d->changed[n] = 1;
    }
    memset(d->marks, 0, d->img.height * W);
    d->resume = keep;
    d->filtered = 0;
    __atomic_store_n(&d->fmb, 0, __ATOMIC_RELAXED);
    d->posted = 0;
    d->active = HANTRO_FALSE;
}

/* A slice failed. Put back what the worker filtered under it (rows posted
 * since it opened, those with a log), see 2. above. The caller clears its
 * row-end value */
void h264bsdDbwAbortError(dbw_t *d)
{
    u32 W, r, c;

    if (!d || !d->active)
        return;
    vj_worker_wait(d->w);
    W = d->img.width;
#ifdef VJ_DBW_STATS
    d->nErr++;
    {
        u32 k;
        for (k = d->r0; k < d->filtered; k++)
            if (d->rowBank[k] != DBW_NOBANK)
            {
                d->nErrUndo++;
                break;
            }
    }
#endif
    for (r = d->filtered; r-- > d->r0;)
    {
        if (d->rowBank[r] == DBW_NOBANK)
            continue;
        for (c = W; c--;)
        {
            /* a footprint was logged for every MB filtered with edges, and
             * only for those: they carry their own mark */
            if (d->marks[r * W + c] & DBW_MARK_OWN)
                RestoreFootprint(d->log +
                    ((size_t)d->rowBank[r] * W + c) * DBW_SLOT_WORDS,
                    &d->img, r, c);
        }
    }
    DbwSettle(d, d->r0);
}

/* The picture goes on without the worker, keeping what it filtered: for the
 * concealment of lost macroblocks and a picture that left raster order (see
 * 2. above) */
void h264bsdDbwAbortKeep(dbw_t *d)
{
    if (!d || !d->active)
        return;
    vj_worker_wait(d->w);
#ifdef VJ_DBW_STATS
    d->nKeep++;
#endif
    DbwSettle(d, d->filtered);
}

/* The row the final pass starts at, after taking the worker out of the
 * picture: 0 without a worker */
u32 h264bsdDbwResume(dbw_t *d)
{
    if (!d)
        return 0;
    h264bsdDbwAbortKeep(d);
    return d->resume;
}

/* Wait until the worker has nothing in flight (its rows stay filtered). For
 * the exits of h264bsdDecode that do not abort or finish */
void h264bsdDbwJoin(dbw_t *d)
{
    if (d && d->active)
        vj_worker_wait(d->w);
}

/* The picture is complete, decoded once in raster order, records valid, no
 * error to come: the worker filters the rows left (no log), and its marks join
 * the picture's */
void h264bsdDbwFinish(dbw_t *d)
{
    u32 n = d->img.width * d->img.height, i;

    for (i = d->posted; i < d->img.height; i++)
        d->rowBank[i] = DBW_NOBANK;
#ifdef VJ_DBW_STATS
    d->nFinish++;
#endif
    vj_worker_post(d->w, d->img.height);
    vj_worker_wait(d->w);
    for (i = 0; i < n; i++)
        if (d->marks[i])
            d->changed[i] = 1;
    memset(d->marks, 0, n);
    d->resume = d->img.height;
    d->filtered = 0;
    __atomic_store_n(&d->fmb, 0, __ATOMIC_RELAXED);
    d->posted = 0;
    d->active = HANTRO_FALSE;
}

/* The worker can finish the picture: it is active, and the ring holds the
 * records of the rows it has not been posted (the qps of the row above the
 * first of them it has, from filtering it) */
u32 h264bsdDbwFinishable(dbw_t *d)
{
    return d && d->active && RingHolds(d, d->img.height, d->posted);
}

/* The final pass from row `from` of a picture of `height` rows decoded in
 * raster order may read its records from the ring: it holds them all (without
 * a worker the ring is the whole picture). Otherwise the pass makes them. */
u32 h264bsdDbwRecCovers(dbw_t *d, u32 height, u32 from)
{
    u32 covers = !d || RingHolds(d, height, from ? from - 1 : 0);

    if (d && !covers)
        d->nRemade++;
    return covers;
}

/* tests and stats builds: waits of the decoding thread for ring room, pictures
 * the worker got out of, final passes that made records the ring did not hold */
void h264bsdDbwRingStats(dbw_t *d, unsigned *waits, unsigned *lost,
    unsigned *remade)
{
    *waits = d->nRecWait;
    *lost = d->nLost;
    *remade = d->nRemade;
}
#endif /* VJ_DEBLOCK_WORKER */


int sample = 0;
unsigned int hashA = 0;
unsigned int hashB = 0;
unsigned int hashC = 0;
unsigned int hashD = 0;

/*------------------------------------------------------------------------------

    Function: FilterVerLumaEdge

        Functional description:
            Filter one vertical 4-pixel luma edge.

------------------------------------------------------------------------------*/
void FilterVerLumaEdge(
  u8 *data,
  u32 bS,
  edgeThreshold_t *thresholds,
  u32 imageWidth)
{

/* Variables */

    i32 delta, tc, tmp;
    u32 i;
    i32 p0, q0, p1, q1, p2, q2;
    u32 tmpFlag;
    const u8 *clp = h264bsdClip + 512;

    u32 alpha = thresholds->alpha;
    u32 beta = thresholds->beta;
    i32 val;

/* Code */

    ASSERT(data);
    ASSERT(bS && bS <= 4);
    ASSERT(thresholds);

    if (bS < 4)
    {
        tc = thresholds->tc0[bS-1];
        tmp = tc;
        for (i = 4; i; i--, data += imageWidth)
        {
            p1 = data[-2]; p0 = data[-1];
            q0 = data[0]; q1 = data[1];

            if ( ((u32)ABS(p0 - q0) < alpha) &&
                 ((u32)ABS(p1 - p0) < beta)  &&
                 ((u32)ABS(q1 - q0) < beta) )
            {
                p2 = data[-3];
                q2 = data[2];

                if ((u32)ABS(p2 - p0) < beta)
                {
                    val = (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1;
                    data[-2] = (p1 + CLIP3(-tc, tc, val));
                    tmp++;
                }

                if ((u32)ABS(q2 - q0) < beta)
                {
                    val = (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1;
                    data[1] = (q1 + CLIP3(-tc, tc, val));
                    tmp++;
                }

                val = (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3;
                delta = CLIP3(-tmp, tmp, val);

                p0 = clp[p0 + delta];
                q0 = clp[q0 - delta];
                tmp = tc;
                data[-1] = p0;
                data[ 0] = q0;
            }
            // hashA += data[-2] + data[-1] + data[0] + data[1];
        }
    }
    else
    {
        for (i = 4; i; i--, data += imageWidth)
        {
            p1 = data[-2]; p0 = data[-1];
            q0 = data[0]; q1 = data[1];
            if ( ((u32)ABS(p0-q0) < alpha) &&
                 ((u32)ABS(p1-p0) < beta)  &&
                 ((u32)ABS(q1-q0) < beta) )
            {
                tmpFlag = ((u32)ABS(p0 - q0) < ((alpha >> 2) +2)) ? HANTRO_TRUE : HANTRO_FALSE;

                p2 = data[-3];
                q2 = data[2];

                if (tmpFlag && (u32)ABS(p2-p0) < beta)
                {
                    tmp = p1 + p0 + q0;
                    data[-1] = ((p2 + 2 * tmp + q1 + 4) >> 3);
                    data[-2] = ((p2 + tmp + 2) >> 2);
                    data[-3] = ((2 * data[-4] + 3 * p2 + tmp + 4) >> 3);
                }
                else
                    data[-1] = (2 * p1 + p0 + q1 + 2) >> 2;

                if (tmpFlag && (u32)ABS(q2-q0) < beta)
                {
                    tmp = p0 + q0 + q1;
                    data[0] = ((p1 + 2 * tmp + q2 + 4) >> 3);
                    data[1] = ((tmp + q2 + 2) >> 2);
                    data[2] = ((2 * data[3] + 3 * q2 + tmp + 4) >> 3);
                }
                else
                    data[0] = ((2 * q1 + q0 + p1 + 2) >> 2);
            }
        }
    }

}

/*------------------------------------------------------------------------------

    Function: FilterHorLumaEdge

        Functional description:
            Filter one horizontal 4-pixel luma edge

------------------------------------------------------------------------------*/
void FilterHorLumaEdge(
  u8 *data,
  u32 bS,
  edgeThreshold_t *thresholds,
  i32 imageWidth)
{

/* Variables */

    i32 delta, tc, tmp;
    u32 i;
    u8 p0, q0, p1, q1, p2, q2;
    const u8 *clp = h264bsdClip + 512;
    i32 val;

/* Code */

    ASSERT(data);
    ASSERT(bS < 4);
    ASSERT(thresholds);

//    if (sample ++ % (1024 * 128) == 0) {
//        printf("Hash A: %d, Hash B: %d\n", hashA, hashB);
//    }

    tc = thresholds->tc0[bS-1];
    tmp = tc;
    for (i = 4; i; i--, data++)
    {
        p1 = data[-imageWidth*2]; p0 = data[-imageWidth];
        q0 = data[0]; q1 = data[imageWidth];
        if ( ((u32)ABS(p0-q0) < thresholds->alpha) &&
             ((u32)ABS(p1-p0) < thresholds->beta)  &&
             ((u32)ABS(q1-q0) < thresholds->beta) )
        {
            p2 = data[-imageWidth*3];

            if ((u32)ABS(p2-p0) < thresholds->beta)
            {
                val = (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1;
                data[-imageWidth*2] = (p1 + CLIP3(-tc, tc, val));
                tmp++;
            }

            q2 = data[imageWidth*2];

            if ((u32)ABS(q2-q0) < thresholds->beta)
            {
                val = (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1;
                data[imageWidth] = (q1 + CLIP3(-tc, tc, val));
                tmp++;
            }

            val = ((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            delta = CLIP3(-tmp, tmp, val);

            p0 = clp[p0 + delta];
            q0 = clp[q0 - delta];
            tmp = tc;
            data[-imageWidth] = p0;
            data[  0] = q0;
        }

        // hashB += data[-imageWidth*2] + data[-imageWidth] + data[0] + data[imageWidth];
    }
}

/*------------------------------------------------------------------------------

    Function: FilterHorLuma

        Functional description:
            Filter all four successive horizontal 4-pixel luma edges. This can
            be done when bS is equal to all four edges.

------------------------------------------------------------------------------*/
void FilterHorLuma(
  u8 *data,
  u32 bS,
  edgeThreshold_t *thresholds,
  i32 imageWidth)
{
/* Variables */

    i32 delta, tc, tmp;
    u32 i;
    i32 p0, q0, p1, q1, p2, q2;
    u32 tmpFlag;
    const u8 *clp = h264bsdClip + 512;
    u32 alpha = thresholds->alpha;
    u32 beta = thresholds->beta;
    i32 val;
/* Code */

    ASSERT(data);
    ASSERT(bS <= 4);
    ASSERT(thresholds);

//    if (sample ++ % (1024 * 64) == 0) {
//        printf("Hash A: %d, Hash B: %d\n", hashA, hashB);
//    }

    if (bS < 4)
    {
        tc = thresholds->tc0[bS-1];
        tmp = tc;
        for (i = 16; i; i--, data++)
        {
            p1 = data[-imageWidth*2]; p0 = data[-imageWidth];
            q0 = data[0]; q1 = data[imageWidth];
            if ( ((u32)ABS(p0 - q0) < alpha) &&
                 ((u32)ABS(p1 - p0) < beta)  &&
                 ((u32)ABS(q1 - q0) < beta) )
            {
                p2 = data[-imageWidth*3];

                if ((u32)ABS(p2 - p0) < beta)
                {
                    val = (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1;
                    data[-imageWidth*2] = (u8)(p1 + CLIP3(-tc, tc, val));
                    tmp++;
                }

                q2 = data[imageWidth*2];

                if ((u32)ABS(q2-q0) < beta)
                {
                    val = (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1;
                    data[imageWidth] = (u8)(q1 + CLIP3(-tc, tc, val));
                    tmp++;
                }

                val = ((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
                delta = CLIP3(-tmp, tmp, val);

                p0 = clp[p0 + delta];
                q0 = clp[q0 - delta];
                tmp = tc;
                data[-imageWidth] = p0;
                data[  0] = q0;
            }
        }
    }
    else
    {
        for (i = 16; i; i--, data++)
        {
            p1 = data[-imageWidth*2]; p0 = data[-imageWidth];
            q0 = data[0]; q1 = data[imageWidth];
            if ( ((u32)ABS(p0 - q0) < alpha) &&
                 ((u32)ABS(p1 - p0) < beta)  &&
                 ((u32)ABS(q1 - q0) < beta) )
            {
                tmpFlag = ((u32)ABS(p0 - q0) < ((alpha >> 2) +2))
                            ? HANTRO_TRUE : HANTRO_FALSE;

                p2 = data[-imageWidth*3];
                q2 = data[imageWidth*2];

                if (tmpFlag && (u32)ABS(p2 - p0) < beta)
                {
                    tmp = p1 + p0 + q0;
                    data[-imageWidth] = (u8)((p2 + 2 * tmp + q1 + 4) >> 3);
                    data[-imageWidth*2] = (u8)((p2 + tmp + 2) >> 2);
                    data[-imageWidth*3] = (u8)((2 * data[-imageWidth*4] +
                                           3 * p2 + tmp + 4) >> 3);
                }
                else
                    data[-imageWidth] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);

                if (tmpFlag && (u32)ABS(q2 - q0) < beta)
                {
                    tmp = p0 + q0 + q1;
                    data[ 0] = (u8)((p1 + 2 * tmp + q2 + 4) >> 3);
                    data[imageWidth] = (u8)((tmp + q2 + 2) >> 2);
                    data[imageWidth*2] = (u8)((2 * data[imageWidth*3] +
                                          3 * q2 + tmp + 4) >> 3);
                }
                else
                    data[0] = (2 * q1 + q0 + p1 + 2) >> 2;
            }
        }
    }

    // hashA += data[-imageWidth*2] + data[-imageWidth] + data[0] + data[imageWidth];

}

/*------------------------------------------------------------------------------

    Function: FilterVerChromaEdge

        Functional description:
            Filter one vertical 2-pixel chroma edge

------------------------------------------------------------------------------*/
void FilterVerChromaEdge(
  u8 *data,
  u32 bS,
  edgeThreshold_t *thresholds,
  u32 width)
{

/* Variables */

    i32 delta, tc;
    u8 p0, q0, p1, q1;
    const u8 *clp = h264bsdClip + 512;

/* Code */

    ASSERT(data);
    ASSERT(bS <= 4);
    ASSERT(thresholds);

    p1 = data[-2]; p0 = data[-1];
    q0 = data[0]; q1 = data[1];
    if ( ((u32)ABS(p0-q0) < thresholds->alpha) &&
         ((u32)ABS(p1-p0) < thresholds->beta)  &&
         ((u32)ABS(q1-q0) < thresholds->beta) )
    {
        if (bS < 4)
        {
            tc = thresholds->tc0[bS-1] + 1;
            delta = CLIP3(-tc, tc, ((((q0 - p0) << 2) +
                      (p1 - q1) + 4) >> 3));
            p0 = clp[p0 + delta];
            q0 = clp[q0 - delta];
            data[-1] = p0;
            data[ 0] = q0;
        }
        else
        {
            data[-1] = (2 * p1 + p0 + q1 + 2) >> 2;
            data[ 0] = (2 * q1 + q0 + p1 + 2) >> 2;
        }
    }
    data += width;
    p1 = data[-2]; p0 = data[-1];
    q0 = data[0]; q1 = data[1];
    if ( ((u32)ABS(p0-q0) < thresholds->alpha) &&
         ((u32)ABS(p1-p0) < thresholds->beta)  &&
         ((u32)ABS(q1-q0) < thresholds->beta) )
    {
        if (bS < 4)
        {
            tc = thresholds->tc0[bS-1] + 1;
            delta = CLIP3(-tc, tc, ((((q0 - p0) << 2) +
                      (p1 - q1) + 4) >> 3));
            p0 = clp[p0 + delta];
            q0 = clp[q0 - delta];
            data[-1] = p0;
            data[ 0] = q0;
        }
        else
        {
            data[-1] = (2 * p1 + p0 + q1 + 2) >> 2;
            data[ 0] = (2 * q1 + q0 + p1 + 2) >> 2;
        }
    }

}

/*------------------------------------------------------------------------------

    Function: FilterHorChromaEdge

        Functional description:
            Filter one horizontal 2-pixel chroma edge

------------------------------------------------------------------------------*/
void FilterHorChromaEdge(
  u8 *data,
  u32 bS,
  edgeThreshold_t *thresholds,
  i32 width)
{

/* Variables */

    i32 delta, tc;
    u32 i;
    u8 p0, q0, p1, q1;
    const u8 *clp = h264bsdClip + 512;

/* Code */

    ASSERT(data);
    ASSERT(bS < 4);
    ASSERT(thresholds);

    tc = thresholds->tc0[bS-1] + 1;
    for (i = 2; i; i--, data++)
    {
        p1 = data[-width*2]; p0 = data[-width];
        q0 = data[0]; q1 = data[width];
        if ( ((u32)ABS(p0-q0) < thresholds->alpha) &&
             ((u32)ABS(p1-p0) < thresholds->beta)  &&
             ((u32)ABS(q1-q0) < thresholds->beta) )
        {
            delta = CLIP3(-tc, tc, ((((q0 - p0) << 2) +
                      (p1 - q1) + 4) >> 3));
            p0 = clp[p0 + delta];
            q0 = clp[q0 - delta];
            data[-width] = p0;
            data[  0] = q0;
        }
    }
}

/*------------------------------------------------------------------------------

    Function: FilterHorChroma

        Functional description:
            Filter all four successive horizontal 2-pixel chroma edges. This
            can be done if bS is equal for all four edges.

------------------------------------------------------------------------------*/
void FilterHorChroma(
  u8 *data,
  u32 bS,
  edgeThreshold_t *thresholds,
  i32 width)
{

/* Variables */

    i32 delta, tc;
    u32 i;
    u8 p0, q0, p1, q1;
    const u8 *clp = h264bsdClip + 512;

/* Code */

    ASSERT(data);
    ASSERT(bS <= 4);
    ASSERT(thresholds);

    if (bS < 4)
    {
        tc = thresholds->tc0[bS-1] + 1;
        for (i = 8; i; i--, data++)
        {
            p1 = data[-width*2]; p0 = data[-width];
            q0 = data[0]; q1 = data[width];
            if ( ((u32)ABS(p0-q0) < thresholds->alpha) &&
                 ((u32)ABS(p1-p0) < thresholds->beta)  &&
                 ((u32)ABS(q1-q0) < thresholds->beta) )
            {
                delta = CLIP3(-tc, tc, ((((q0 - p0) << 2) +
                          (p1 - q1) + 4) >> 3));
                p0 = clp[p0 + delta];
                q0 = clp[q0 - delta];
                data[-width] = p0;
                data[  0] = q0;
            }
        }
    }
    else
    {
        for (i = 8; i; i--, data++)
        {
            p1 = data[-width*2]; p0 = data[-width];
            q0 = data[0]; q1 = data[width];
            if ( ((u32)ABS(p0-q0) < thresholds->alpha) &&
                 ((u32)ABS(p1-p0) < thresholds->beta)  &&
                 ((u32)ABS(q1-q0) < thresholds->beta) )
            {
                    data[-width] = (2 * p1 + p0 + q1 + 2) >> 2;
                    data[  0] = (2 * q1 + q0 + p1 + 2) >> 2;
            }
        }
    }

}


void GetBoundaryStrengthsA(mbStorage_t *mb, bS_t *bS) {
    bS[4].top = mb->totalCoeff[2] || mb->totalCoeff[0] ? 2 : 0;
    bS[5].top = mb->totalCoeff[3] || mb->totalCoeff[1] ? 2 : 0;
    bS[6].top = mb->totalCoeff[6] || mb->totalCoeff[4] ? 2 : 0;
    bS[7].top = mb->totalCoeff[7] || mb->totalCoeff[5] ? 2 : 0;
    bS[8].top = mb->totalCoeff[8] || mb->totalCoeff[2] ? 2 : 0;
    bS[9].top = mb->totalCoeff[9] || mb->totalCoeff[3] ? 2 : 0;
    bS[10].top = mb->totalCoeff[12] || mb->totalCoeff[6] ? 2 : 0;
    bS[11].top = mb->totalCoeff[13] || mb->totalCoeff[7] ? 2 : 0;
    bS[12].top = mb->totalCoeff[10] || mb->totalCoeff[8] ? 2 : 0;
    bS[13].top = mb->totalCoeff[11] || mb->totalCoeff[9] ? 2 : 0;
    bS[14].top = mb->totalCoeff[14] || mb->totalCoeff[12] ? 2 : 0;
    bS[15].top = mb->totalCoeff[15] || mb->totalCoeff[13] ? 2 : 0;

    bS[1].left = mb->totalCoeff[1] || mb->totalCoeff[0] ? 2 : 0;
    bS[2].left = mb->totalCoeff[4] || mb->totalCoeff[1] ? 2 : 0;
    bS[3].left = mb->totalCoeff[5] || mb->totalCoeff[4] ? 2 : 0;
    bS[5].left = mb->totalCoeff[3] || mb->totalCoeff[2] ? 2 : 0;
    bS[6].left = mb->totalCoeff[6] || mb->totalCoeff[3] ? 2 : 0;
    bS[7].left = mb->totalCoeff[7] || mb->totalCoeff[6] ? 2 : 0;
    bS[9].left = mb->totalCoeff[9] || mb->totalCoeff[8] ? 2 : 0;
    bS[10].left = mb->totalCoeff[12] || mb->totalCoeff[9] ? 2 : 0;
    bS[11].left = mb->totalCoeff[13] || mb->totalCoeff[12] ? 2 : 0;
    bS[13].left = mb->totalCoeff[11] || mb->totalCoeff[10] ? 2 : 0;
    bS[14].left = mb->totalCoeff[14] || mb->totalCoeff[11] ? 2 : 0;
    bS[15].left = mb->totalCoeff[15] || mb->totalCoeff[14] ? 2 : 0;
}

/*------------------------------------------------------------------------------

    Function: GetBoundaryStrengths

        Functional description:
            Function to calculate boundary strengths for all edges of a
            macroblock. Function returns HANTRO_TRUE if any of the bS values for
            the macroblock had non-zero value, HANTRO_FALSE otherwise.

------------------------------------------------------------------------------*/
u32 GetBoundaryStrengths(mbStorage_t *mb, bS_t *bS, u32 flags)
{

/* Variables */

    /* this flag is set HANTRO_TRUE as soon as any boundary strength value is
     * non-zero */
    u32 nonZeroBs = HANTRO_FALSE;

/* Code */

    ASSERT(mb);
    ASSERT(bS);
    ASSERT(flags);

//    if (sample ++ % (1024 * 128) == 0) {
//        printf("Hash A: %d, Hash B: %d, Hash C: %d, Hash D: %d\n", hashA, hashB, hashC, hashD);
//    }

    /* top edges */
    if (flags & FILTER_TOP_EDGE)
    {
        if (IS_INTRA_MB(*mb) || IS_INTRA_MB(*mb->mbB))
        {
            bS[0].top = bS[1].top = bS[2].top = bS[3].top = 4;
            nonZeroBs = HANTRO_TRUE;
        }
        else
        {
            bS[0].top = EdgeBoundaryStrength(mb, mb->mbB, 0, 10);
            bS[1].top = EdgeBoundaryStrength(mb, mb->mbB, 1, 11);
            bS[2].top = EdgeBoundaryStrength(mb, mb->mbB, 4, 14);
            bS[3].top = EdgeBoundaryStrength(mb, mb->mbB, 5, 15);
            if (bS[0].top || bS[1].top || bS[2].top || bS[3].top)
                nonZeroBs = HANTRO_TRUE;
        }
    }
    else
    {
        bS[0].top = bS[1].top = bS[2].top = bS[3].top = 0;
    }

    /* left edges */
    if (flags & FILTER_LEFT_EDGE)
    {
        if (IS_INTRA_MB(*mb) || IS_INTRA_MB(*mb->mbA))
        {
            bS[0].left = bS[4].left = bS[8].left = bS[12].left = 4;
            nonZeroBs = HANTRO_TRUE;
        }
        else
        {
            bS[0].left = EdgeBoundaryStrength(mb, mb->mbA, 0, 5);
            bS[4].left = EdgeBoundaryStrength(mb, mb->mbA, 2, 7);
            bS[8].left = EdgeBoundaryStrength(mb, mb->mbA, 8, 13);
            bS[12].left = EdgeBoundaryStrength(mb, mb->mbA, 10, 15);
            if (!nonZeroBs &&
                (bS[0].left || bS[4].left || bS[8].left || bS[12].left))
                nonZeroBs = HANTRO_TRUE;
        }
    }
    else
    {
        bS[0].left = bS[4].left = bS[8].left = bS[12].left = 0;
    }

    /* inner edges */
    if (IS_INTRA_MB(*mb))
    {
        bS[4].top  = bS[5].top  = bS[6].top  = bS[7].top  =
        bS[8].top  = bS[9].top  = bS[10].top = bS[11].top =
        bS[12].top = bS[13].top = bS[14].top = bS[15].top = 3;

        bS[1].left  = bS[2].left  = bS[3].left  =
        bS[5].left  = bS[6].left  = bS[7].left  =
        bS[9].left  = bS[10].left = bS[11].left =
        bS[13].left = bS[14].left = bS[15].left = 3;
        nonZeroBs = HANTRO_TRUE;
    }
    else
    {
        /* 16x16 inter mb -> ref addresses or motion vectors cannot differ,
         * only check if either of the blocks contain coefficients */
        if (h264bsdNumMbPart(mb->mbType) == 1)
        {
            GetBoundaryStrengthsA(mb, bS);
        }
        /* 16x8 inter mb -> ref addresses and motion vectors can be different
         * only for the middle horizontal edge, for the other top edges it is
         * enough to check whether the blocks contain coefficients or not. The
         * same applies to all internal left edges. */
        else if (mb->mbType == P_L0_L0_16x8)
        {
            bS[4].top = mb->totalCoeff[2] || mb->totalCoeff[0] ? 2 : 0;
            bS[5].top = mb->totalCoeff[3] || mb->totalCoeff[1] ? 2 : 0;
            bS[6].top = mb->totalCoeff[6] || mb->totalCoeff[4] ? 2 : 0;
            bS[7].top = mb->totalCoeff[7] || mb->totalCoeff[5] ? 2 : 0;
            bS[12].top = mb->totalCoeff[10] || mb->totalCoeff[8] ? 2 : 0;
            bS[13].top = mb->totalCoeff[11] || mb->totalCoeff[9] ? 2 : 0;
            bS[14].top = mb->totalCoeff[14] || mb->totalCoeff[12] ? 2 : 0;
            bS[15].top = mb->totalCoeff[15] || mb->totalCoeff[13] ? 2 : 0;
            bS[8].top = InnerBoundaryStrength(mb, 8, 2);
            bS[9].top = InnerBoundaryStrength(mb, 9, 3);
            bS[10].top = InnerBoundaryStrength(mb, 12, 6);
            bS[11].top = InnerBoundaryStrength(mb, 13, 7);

            bS[1].left = mb->totalCoeff[1] || mb->totalCoeff[0] ? 2 : 0;
            bS[2].left = mb->totalCoeff[4] || mb->totalCoeff[1] ? 2 : 0;
            bS[3].left = mb->totalCoeff[5] || mb->totalCoeff[4] ? 2 : 0;
            bS[5].left = mb->totalCoeff[3] || mb->totalCoeff[2] ? 2 : 0;
            bS[6].left = mb->totalCoeff[6] || mb->totalCoeff[3] ? 2 : 0;
            bS[7].left = mb->totalCoeff[7] || mb->totalCoeff[6] ? 2 : 0;
            bS[9].left = mb->totalCoeff[9] || mb->totalCoeff[8] ? 2 : 0;
            bS[10].left = mb->totalCoeff[12] || mb->totalCoeff[9] ? 2 : 0;
            bS[11].left = mb->totalCoeff[13] || mb->totalCoeff[12] ? 2 : 0;
            bS[13].left = mb->totalCoeff[11] || mb->totalCoeff[10] ? 2 : 0;
            bS[14].left = mb->totalCoeff[14] || mb->totalCoeff[11] ? 2 : 0;
            bS[15].left = mb->totalCoeff[15] || mb->totalCoeff[14] ? 2 : 0;
        }
        /* 8x16 inter mb -> ref addresses and motion vectors can be different
         * only for the middle vertical edge, for the other left edges it is
         * enough to check whether the blocks contain coefficients or not. The
         * same applies to all internal top edges. */
        else if (mb->mbType == P_L0_L0_8x16)
        {
            bS[4].top = mb->totalCoeff[2] || mb->totalCoeff[0] ? 2 : 0;
            bS[5].top = mb->totalCoeff[3] || mb->totalCoeff[1] ? 2 : 0;
            bS[6].top = mb->totalCoeff[6] || mb->totalCoeff[4] ? 2 : 0;
            bS[7].top = mb->totalCoeff[7] || mb->totalCoeff[5] ? 2 : 0;
            bS[8].top = mb->totalCoeff[8] || mb->totalCoeff[2] ? 2 : 0;
            bS[9].top = mb->totalCoeff[9] || mb->totalCoeff[3] ? 2 : 0;
            bS[10].top = mb->totalCoeff[12] || mb->totalCoeff[6] ? 2 : 0;
            bS[11].top = mb->totalCoeff[13] || mb->totalCoeff[7] ? 2 : 0;
            bS[12].top = mb->totalCoeff[10] || mb->totalCoeff[8] ? 2 : 0;
            bS[13].top = mb->totalCoeff[11] || mb->totalCoeff[9] ? 2 : 0;
            bS[14].top = mb->totalCoeff[14] || mb->totalCoeff[12] ? 2 : 0;
            bS[15].top = mb->totalCoeff[15] || mb->totalCoeff[13] ? 2 : 0;

            bS[1].left = mb->totalCoeff[1] || mb->totalCoeff[0] ? 2 : 0;
            bS[3].left = mb->totalCoeff[5] || mb->totalCoeff[4] ? 2 : 0;
            bS[5].left = mb->totalCoeff[3] || mb->totalCoeff[2] ? 2 : 0;
            bS[7].left = mb->totalCoeff[7] || mb->totalCoeff[6] ? 2 : 0;
            bS[9].left = mb->totalCoeff[9] || mb->totalCoeff[8] ? 2 : 0;
            bS[11].left = mb->totalCoeff[13] || mb->totalCoeff[12] ? 2 : 0;
            bS[13].left = mb->totalCoeff[11] || mb->totalCoeff[10] ? 2 : 0;
            bS[15].left = mb->totalCoeff[15] || mb->totalCoeff[14] ? 2 : 0;
            bS[2].left = InnerBoundaryStrength(mb, 4, 1);
            bS[6].left = InnerBoundaryStrength(mb, 6, 3);
            bS[10].left = InnerBoundaryStrength(mb, 12, 9);
            bS[14].left = InnerBoundaryStrength(mb, 14, 11);
        }
        else
        {
            bS[4].top = InnerBoundaryStrength(mb, mb4x4Index[4], mb4x4Index[0]);
            bS[5].top = InnerBoundaryStrength(mb, mb4x4Index[5], mb4x4Index[1]);
            bS[6].top = InnerBoundaryStrength(mb, mb4x4Index[6], mb4x4Index[2]);
            bS[7].top = InnerBoundaryStrength(mb, mb4x4Index[7], mb4x4Index[3]);
            bS[8].top = InnerBoundaryStrength(mb, mb4x4Index[8], mb4x4Index[4]);
            bS[9].top = InnerBoundaryStrength(mb, mb4x4Index[9], mb4x4Index[5]);
            bS[10].top = InnerBoundaryStrength(mb, mb4x4Index[10], mb4x4Index[6]);
            bS[11].top = InnerBoundaryStrength(mb, mb4x4Index[11], mb4x4Index[7]);
            bS[12].top = InnerBoundaryStrength(mb, mb4x4Index[12], mb4x4Index[8]);
            bS[13].top = InnerBoundaryStrength(mb, mb4x4Index[13], mb4x4Index[9]);
            bS[14].top = InnerBoundaryStrength(mb, mb4x4Index[14], mb4x4Index[10]);
            bS[15].top = InnerBoundaryStrength(mb, mb4x4Index[15], mb4x4Index[11]);

            bS[1].left = InnerBoundaryStrength(mb, mb4x4Index[1], mb4x4Index[0]);
            bS[2].left = InnerBoundaryStrength(mb, mb4x4Index[2], mb4x4Index[1]);
            bS[3].left = InnerBoundaryStrength(mb, mb4x4Index[3], mb4x4Index[2]);
            bS[5].left = InnerBoundaryStrength(mb, mb4x4Index[5], mb4x4Index[4]);
            bS[6].left = InnerBoundaryStrength(mb, mb4x4Index[6], mb4x4Index[5]);
            bS[7].left = InnerBoundaryStrength(mb, mb4x4Index[7], mb4x4Index[6]);
            bS[9].left = InnerBoundaryStrength(mb, mb4x4Index[9], mb4x4Index[8]);
            bS[10].left = InnerBoundaryStrength(mb, mb4x4Index[10], mb4x4Index[9]);
            bS[11].left = InnerBoundaryStrength(mb, mb4x4Index[11], mb4x4Index[10]);
            bS[13].left = InnerBoundaryStrength(mb, mb4x4Index[13], mb4x4Index[12]);
            bS[14].left = InnerBoundaryStrength(mb, mb4x4Index[14], mb4x4Index[13]);
            bS[15].left = InnerBoundaryStrength(mb, mb4x4Index[15], mb4x4Index[14]);
        }
        if (!nonZeroBs &&
            (bS[4].top || bS[5].top || bS[6].top || bS[7].top ||
             bS[8].top || bS[9].top || bS[10].top || bS[11].top ||
             bS[12].top || bS[13].top || bS[14].top || bS[15].top ||
             bS[1].left || bS[2].left || bS[3].left ||
             bS[5].left || bS[6].left || bS[7].left ||
             bS[9].left || bS[10].left || bS[11].left ||
             bS[13].left || bS[14].left || bS[15].left))
            nonZeroBs = HANTRO_TRUE;
    }

    return(nonZeroBs);

}

/*------------------------------------------------------------------------------

    Function: GetEdgeThresholds

        Functional description:
            Compute alpha, beta and tc0 thresholds for the top, left and
            inner edges of the macroblock of rec, from its qp and the qps of
            its top and left neighbours, mapped to chroma qps if chroma. An
            MB edge uses the average qp of its two macroblocks, which is the
            macroblock's own when they are equal: the same values as the
            original GetLumaEdgeThresholds and GetChromaEdgeThresholds.

------------------------------------------------------------------------------*/
void GetEdgeThresholds(
  edgeThreshold_t *thresholds,
  const deblockRec_t *rec,
  u32 qpTop,
  u32 qpLeft,
  u32 chroma)
{

/* Variables */

    u32 i, indexA, indexB;
    u32 qp = rec->qp;
    u32 qpAv[3];

/* Code */

    ASSERT(thresholds);
    ASSERT(rec);

    if (chroma)
    {
        qp = h264bsdQpC[CLIP3(0, 51, (i32)qp + rec->chromaQpOff)];
        qpTop = h264bsdQpC[CLIP3(0, 51, (i32)qpTop + rec->chromaQpOff)];
        qpLeft = h264bsdQpC[CLIP3(0, 51, (i32)qpLeft + rec->chromaQpOff)];
    }
    qpAv[TOP] = (qp + qpTop + 1) >> 1;
    qpAv[LEFT] = (qp + qpLeft + 1) >> 1;
    qpAv[INNER] = qp;

    for (i = 0; i < 3; i++)
    {
        indexA = (u32)CLIP3(0, 51, (i32)qpAv[i] + rec->offA);
        indexB = (u32)CLIP3(0, 51, (i32)qpAv[i] + rec->offB);

        thresholds[i].alpha = alphas[indexA];
        thresholds[i].beta = betas[indexB];
        thresholds[i].tc0 = tc0[indexA];
    }

}

/*------------------------------------------------------------------------------

    Function: FilterLuma

        Functional description:
            Function to filter all luma edges of a macroblock

------------------------------------------------------------------------------*/
void FilterLuma(
  u8 *data,
  bS_t *bS,
  edgeThreshold_t *thresholds,
  u32 width)
{

/* Variables */

    u32 vblock;
    bS_t *tmp;
    u8 *ptr;
    u32 offset;

/* Code */

    ASSERT(data);
    ASSERT(bS);
    ASSERT(thresholds);

    ptr = data;
    tmp = bS;

    offset  = TOP;

    /* loop block rows, perform filtering for all vertical edges of the block
     * row first, then filter each horizontal edge of the row */
    for (vblock = 4; vblock--;)
    {
        /* only perform filtering if bS is non-zero, first of the four
         * FilterVerLumaEdge handles the left edge of the macroblock, others
         * filter inner edges */
        if (tmp[0].left)
            FilterVerLumaEdge(ptr, tmp[0].left, thresholds + LEFT, width);
        if (tmp[1].left)
            FilterVerLumaEdge(ptr+4, tmp[1].left, thresholds + INNER, width);
        if (tmp[2].left)
            FilterVerLumaEdge(ptr+8, tmp[2].left, thresholds + INNER, width);
        if (tmp[3].left)
            FilterVerLumaEdge(ptr+12, tmp[3].left, thresholds + INNER, width);

        /* if bS is equal for all horizontal edges of the row -> perform
         * filtering with FilterHorLuma, otherwise use FilterHorLumaEdge for
         * each edge separately. offset variable indicates top macroblock edge
         * on the first loop round, inner edge for the other rounds */
        if (tmp[0].top == tmp[1].top && tmp[1].top == tmp[2].top &&
            tmp[2].top == tmp[3].top)
        {
            if(tmp[0].top)
                FilterHorLuma(ptr, tmp[0].top, thresholds + offset, (i32)width);
        }
        else
        {
            if(tmp[0].top)
                FilterHorLumaEdge(ptr, tmp[0].top, thresholds+offset,
                    (i32)width);
            if(tmp[1].top)
                FilterHorLumaEdge(ptr+4, tmp[1].top, thresholds+offset,
                    (i32)width);
            if(tmp[2].top)
                FilterHorLumaEdge(ptr+8, tmp[2].top, thresholds+offset,
                    (i32)width);
            if(tmp[3].top)
                FilterHorLumaEdge(ptr+12, tmp[3].top, thresholds+offset,
                    (i32)width);
        }

        /* four pixel rows ahead, i.e. next row of 4x4-blocks */
        ptr += width*4;
        tmp += 4;
        offset = INNER;
    }
}

/*------------------------------------------------------------------------------

    Function: FilterChroma

        Functional description:
            Function to filter all chroma edges of a macroblock

------------------------------------------------------------------------------*/
void FilterChroma(
  u8 *dataCb,
  u8 *dataCr,
  bS_t *bS,
  edgeThreshold_t *thresholds,
  u32 width)
{

/* Variables */

    u32 vblock;
    bS_t *tmp;
    u32 offset;

/* Code */

    ASSERT(dataCb);
    ASSERT(dataCr);
    ASSERT(bS);
    ASSERT(thresholds);

    tmp = bS;
    offset = TOP;

    /* loop block rows, perform filtering for all vertical edges of the block
     * row first, then filter each horizontal edge of the row */
    for (vblock = 0; vblock < 2; vblock++)
    {
        /* only perform filtering if bS is non-zero, first two of the four
         * FilterVerChromaEdge calls handle the left edge of the macroblock,
         * others filter the inner edge. Note that as chroma uses bS values
         * determined for luma edges, each bS is used only for 2 pixels of
         * a 4-pixel edge */
        if (tmp[0].left)
        {
            FilterVerChromaEdge(dataCb, tmp[0].left, thresholds + LEFT, width);
            FilterVerChromaEdge(dataCr, tmp[0].left, thresholds + LEFT, width);
        }
        if (tmp[4].left)
        {
            FilterVerChromaEdge(dataCb+2*width, tmp[4].left, thresholds + LEFT,
                width);
            FilterVerChromaEdge(dataCr+2*width, tmp[4].left, thresholds + LEFT,
                width);
        }
        if (tmp[2].left)
        {
            FilterVerChromaEdge(dataCb+4, tmp[2].left, thresholds + INNER,
                width);
            FilterVerChromaEdge(dataCr+4, tmp[2].left, thresholds + INNER,
                width);
        }
        if (tmp[6].left)
        {
            FilterVerChromaEdge(dataCb+2*width+4, tmp[6].left,
                thresholds + INNER, width);
            FilterVerChromaEdge(dataCr+2*width+4, tmp[6].left,
                thresholds + INNER, width);
        }

        /* if bS is equal for all horizontal edges of the row -> perform
         * filtering with FilterHorChroma, otherwise use FilterHorChromaEdge
         * for each edge separately. offset variable indicates top macroblock
         * edge on the first loop round, inner edge for the second */
        if (tmp[0].top == tmp[1].top && tmp[1].top == tmp[2].top &&
            tmp[2].top == tmp[3].top)
        {
            if(tmp[0].top)
            {
                FilterHorChroma(dataCb, tmp[0].top, thresholds+offset,
                    (i32)width);
                FilterHorChroma(dataCr, tmp[0].top, thresholds+offset,
                    (i32)width);
            }
        }
        else
        {
            if (tmp[0].top)
            {
                FilterHorChromaEdge(dataCb, tmp[0].top, thresholds+offset,
                    (i32)width);
                FilterHorChromaEdge(dataCr, tmp[0].top, thresholds+offset,
                    (i32)width);
            }
            if (tmp[1].top)
            {
                FilterHorChromaEdge(dataCb+2, tmp[1].top, thresholds+offset,
                    (i32)width);
                FilterHorChromaEdge(dataCr+2, tmp[1].top, thresholds+offset,
                    (i32)width);
            }
            if (tmp[2].top)
            {
                FilterHorChromaEdge(dataCb+4, tmp[2].top, thresholds+offset,
                    (i32)width);
                FilterHorChromaEdge(dataCr+4, tmp[2].top, thresholds+offset,
                    (i32)width);
            }
            if (tmp[3].top)
            {
                FilterHorChromaEdge(dataCb+6, tmp[3].top, thresholds+offset,
                    (i32)width);
                FilterHorChromaEdge(dataCr+6, tmp[3].top, thresholds+offset,
                    (i32)width);
            }
        }

        tmp += 8;
        dataCb += width*4;
        dataCr += width*4;
        offset = INNER;
    }
}

#else /* H264DEC_OMXDL */

/*------------------------------------------------------------------------------

    Function: h264bsdFilterPicture

        Functional description:
          Perform deblocking filtering for a picture. Filter does not copy
          the original picture anywhere but filtering is performed directly
          on the original image. Parameters controlling the filtering process
          are computed based on information in macroblock structures of the
          filtered macroblock, macroblock above and macroblock on the left of
          the filtered one.

        Inputs:
          image         pointer to image to be filtered
          mb            pointer to macroblock data structure of the top-left
                        macroblock of the picture

        Outputs:
          image         filtered image stored here

        Returns:
          none

------------------------------------------------------------------------------*/

/*lint --e{550} Symbol not accessed */
void h264bsdFilterPicture(
  image_t *image,
  mbStorage_t *mb, u32 picSliceBase)
{

/* Variables */

    u32 flags;
    u32 picSizeInMbs, mbRow, mbCol;
    u32 picWidthInMbs;
    u8 *data;
    mbStorage_t *pMb;
    u8 bS[2][16];
    u8 thresholdLuma[2][16];
    u8 thresholdChroma[2][8];
    u8 alpha[2][2];
    u8 beta[2][2];
    OMXResult res;

/* Code */

    ASSERT(image);
    ASSERT(mb);
    ASSERT(image->data);
    ASSERT(image->width);
    ASSERT(image->height);

    picWidthInMbs = image->width;
    data = image->data;
    picSizeInMbs = picWidthInMbs * image->height;

    pMb = mb;

    for (mbRow = 0, mbCol = 0; mbRow < image->height; pMb++)
    {
        flags = GetMbFilteringFlags(pMb, picSliceBase);

        if (flags)
        {
            /* GetBoundaryStrengths function returns non-zero value if any of
             * the bS values for the macroblock being processed was non-zero */
            if (GetBoundaryStrengths(pMb, bS, flags))
            {

                /* Luma */
                GetLumaEdgeThresholds(pMb,alpha,beta,thresholdLuma,bS,flags);
                data = image->data + mbRow * picWidthInMbs * 256 + mbCol * 16;

                res = omxVCM4P10_FilterDeblockingLuma_VerEdge_I( data,
                                                (OMX_S32)(picWidthInMbs*16),
                                                (const OMX_U8*)alpha,
                                                (const OMX_U8*)beta,
                                                (const OMX_U8*)thresholdLuma,
                                                (const OMX_U8*)bS );

                res = omxVCM4P10_FilterDeblockingLuma_HorEdge_I( data,
                                                (OMX_S32)(picWidthInMbs*16),
                                                (const OMX_U8*)alpha+2,
                                                (const OMX_U8*)beta+2,
                                                (const OMX_U8*)thresholdLuma+16,
                                                (const OMX_U8*)bS+16 );
                /* Cb */
                GetChromaEdgeThresholds(pMb, alpha, beta, thresholdChroma,
                                        bS, flags, pMb->chromaQpIndexOffset);
                data = image->data + picSizeInMbs * 256 +
                    mbRow * picWidthInMbs * 64 + mbCol * 8;

                res = omxVCM4P10_FilterDeblockingChroma_VerEdge_I( data,
                                              (OMX_S32)(picWidthInMbs*8),
                                              (const OMX_U8*)alpha,
                                              (const OMX_U8*)beta,
                                              (const OMX_U8*)thresholdChroma,
                                              (const OMX_U8*)bS );
                res = omxVCM4P10_FilterDeblockingChroma_HorEdge_I( data,
                                              (OMX_S32)(picWidthInMbs*8),
                                              (const OMX_U8*)alpha+2,
                                              (const OMX_U8*)beta+2,
                                              (const OMX_U8*)thresholdChroma+8,
                                              (const OMX_U8*)bS+16 );
                /* Cr */
                data += (picSizeInMbs * 64);
                res = omxVCM4P10_FilterDeblockingChroma_VerEdge_I( data,
                                              (OMX_S32)(picWidthInMbs*8),
                                              (const OMX_U8*)alpha,
                                              (const OMX_U8*)beta,
                                              (const OMX_U8*)thresholdChroma,
                                              (const OMX_U8*)bS );
                res = omxVCM4P10_FilterDeblockingChroma_HorEdge_I( data,
                                              (OMX_S32)(picWidthInMbs*8),
                                              (const OMX_U8*)alpha+2,
                                              (const OMX_U8*)beta+2,
                                              (const OMX_U8*)thresholdChroma+8,
                                              (const OMX_U8*)bS+16 );
            }
        }

        mbCol++;
        if (mbCol == picWidthInMbs)
        {
            mbCol = 0;
            mbRow++;
        }
    }

}

/*------------------------------------------------------------------------------

    Function: GetBoundaryStrengths

        Functional description:
            Function to calculate boundary strengths for all edges of a
            macroblock. Function returns HANTRO_TRUE if any of the bS values for
            the macroblock had non-zero value, HANTRO_FALSE otherwise.

------------------------------------------------------------------------------*/
u32 GetBoundaryStrengths(mbStorage_t *mb, u8 (*bS)[16], u32 flags)
{

/* Variables */

    /* this flag is set HANTRO_TRUE as soon as any boundary strength value is
     * non-zero */
    u32 nonZeroBs = HANTRO_FALSE;
    u32 *pTmp;
    u32 tmp1, tmp2, isIntraMb;

/* Code */

    ASSERT(mb);
    ASSERT(bS);
    ASSERT(flags);

    isIntraMb = IS_INTRA_MB(*mb);

    /* top edges */
    pTmp = (u32*)&bS[1][0];
    if (flags & FILTER_TOP_EDGE)
    {
        if (isIntraMb || IS_INTRA_MB(*mb->mbB))
        {
            *pTmp = 0x04040404;
            nonZeroBs = HANTRO_TRUE;
        }
        else
        {
            *pTmp = EdgeBoundaryStrengthTop(mb, mb->mbB);
            if (*pTmp)
                nonZeroBs = HANTRO_TRUE;
        }
    }
    else
    {
        *pTmp = 0;
    }

    /* left edges */
    pTmp = (u32*)&bS[0][0];
    if (flags & FILTER_LEFT_EDGE)
    {
        if (isIntraMb || IS_INTRA_MB(*mb->mbA))
        {
            /*bS[0][0] = bS[0][1] = bS[0][2] = bS[0][3] = 4;*/
            *pTmp = 0x04040404;
            nonZeroBs = HANTRO_TRUE;
        }
        else
        {
            *pTmp = EdgeBoundaryStrengthLeft(mb, mb->mbA);
            if (!nonZeroBs && *pTmp)
                nonZeroBs = HANTRO_TRUE;
        }
    }
    else
    {
        *pTmp = 0;
    }

    /* inner edges */
    if (isIntraMb)
    {
        pTmp++;
        *pTmp++ = 0x03030303;
        *pTmp++ = 0x03030303;
        *pTmp++ = 0x03030303;
        pTmp++;
        *pTmp++ = 0x03030303;
        *pTmp++ = 0x03030303;
        *pTmp = 0x03030303;

        nonZeroBs = HANTRO_TRUE;
    }
    else
    {
        pTmp = (u32*)mb->totalCoeff;

        /* 16x16 inter mb -> ref addresses or motion vectors cannot differ,
         * only check if either of the blocks contain coefficients */
        if (h264bsdNumMbPart(mb->mbType) == 1)
        {
            tmp1 = *pTmp++;
            tmp2 = *pTmp++;
            bS[1][4]  = (tmp1 & 0x00FF00FF) ? 2 : 0; /* [2]  || [0] */
            bS[1][5]  = (tmp1 & 0xFF00FF00) ? 2 : 0; /* [3]  || [1] */
            bS[0][4]  = (tmp1 & 0x0000FFFF) ? 2 : 0; /* [1]  || [0] */
            bS[0][5]  = (tmp1 & 0xFFFF0000) ? 2 : 0; /* [3]  || [2] */

            tmp1 = *pTmp++;
            bS[1][6]  = (tmp2 & 0x00FF00FF) ? 2 : 0; /* [6]  || [4] */
            bS[1][7]  = (tmp2 & 0xFF00FF00) ? 2 : 0; /* [7]  || [5] */
            bS[0][12] = (tmp2 & 0x0000FFFF) ? 2 : 0; /* [5]  || [4] */
            bS[0][13] = (tmp2 & 0xFFFF0000) ? 2 : 0; /* [7]  || [6] */
            tmp2 = *pTmp;
            bS[1][12] = (tmp1 & 0x00FF00FF) ? 2 : 0; /* [10] || [8] */
            bS[1][13] = (tmp1 & 0xFF00FF00) ? 2 : 0; /* [11] || [9] */
            bS[0][6]  = (tmp1 & 0x0000FFFF) ? 2 : 0; /* [9]  || [8] */
            bS[0][7]  = (tmp1 & 0xFFFF0000) ? 2 : 0; /* [11] || [10] */

            bS[1][14] = (tmp2 & 0x00FF00FF) ? 2 : 0; /* [14] || [12] */
            bS[1][15] = (tmp2 & 0xFF00FF00) ? 2 : 0; /* [15] || [13] */
            bS[0][14] = (tmp2 & 0x0000FFFF) ? 2 : 0; /* [13] || [12] */
            bS[0][15] = (tmp2 & 0xFFFF0000) ? 2 : 0; /* [15] || [14] */

            {
            u32 tmp3, tmp4;

            tmp1 = mb->totalCoeff[8];
            tmp2 = mb->totalCoeff[2];
            tmp3 = mb->totalCoeff[9];
            tmp4 = mb->totalCoeff[3];

            bS[1][8] = tmp1 || tmp2 ? 2 : 0;
            tmp1 = mb->totalCoeff[12];
            tmp2 = mb->totalCoeff[6];
            bS[1][9] = tmp3 || tmp4 ? 2 : 0;
            tmp3 = mb->totalCoeff[13];
            tmp4 = mb->totalCoeff[7];
            bS[1][10] = tmp1 || tmp2 ? 2 : 0;
            tmp1 = mb->totalCoeff[4];
            tmp2 = mb->totalCoeff[1];
            bS[1][11] = tmp3 || tmp4 ? 2 : 0;
            tmp3 = mb->totalCoeff[6];
            tmp4 = mb->totalCoeff[3];
            bS[0][8] = tmp1 || tmp2 ? 2 : 0;
            tmp1 = mb->totalCoeff[12];
            tmp2 = mb->totalCoeff[9];
            bS[0][9] = tmp3 || tmp4 ? 2 : 0;
            tmp3 = mb->totalCoeff[14];
            tmp4 = mb->totalCoeff[11];
            bS[0][10] = tmp1 || tmp2 ? 2 : 0;
            bS[0][11] = tmp3 || tmp4 ? 2 : 0;
            }
        }

        /* 16x8 inter mb -> ref addresses and motion vectors can be different
         * only for the middle horizontal edge, for the other top edges it is
         * enough to check whether the blocks contain coefficients or not. The
         * same applies to all internal left edges. */
        else if (mb->mbType == P_L0_L0_16x8)
        {
            tmp1 = *pTmp++;
            tmp2 = *pTmp++;
            bS[1][4]  = (tmp1 & 0x00FF00FF) ? 2 : 0; /* [2]  || [0] */
            bS[1][5]  = (tmp1 & 0xFF00FF00) ? 2 : 0; /* [3]  || [1] */
            bS[0][4]  = (tmp1 & 0x0000FFFF) ? 2 : 0; /* [1]  || [0] */
            bS[0][5]  = (tmp1 & 0xFFFF0000) ? 2 : 0; /* [3]  || [2] */
            tmp1 = *pTmp++;
            bS[1][6]  = (tmp2 & 0x00FF00FF) ? 2 : 0; /* [6]  || [4] */
            bS[1][7]  = (tmp2 & 0xFF00FF00) ? 2 : 0; /* [7]  || [5] */
            bS[0][12] = (tmp2 & 0x0000FFFF) ? 2 : 0; /* [5]  || [4] */
            bS[0][13] = (tmp2 & 0xFFFF0000) ? 2 : 0; /* [7]  || [6] */
            tmp2 = *pTmp;
            bS[1][12] = (tmp1 & 0x00FF00FF) ? 2 : 0; /* [10] || [8] */
            bS[1][13] = (tmp1 & 0xFF00FF00) ? 2 : 0; /* [11] || [9] */
            bS[0][6]  = (tmp1 & 0x0000FFFF) ? 2 : 0; /* [9]  || [8] */
            bS[0][7]  = (tmp1 & 0xFFFF0000) ? 2 : 0; /* [11] || [10] */

            bS[1][14] = (tmp2 & 0x00FF00FF) ? 2 : 0; /* [14] || [12] */
            bS[1][15] = (tmp2 & 0xFF00FF00) ? 2 : 0; /* [15] || [13] */
            bS[0][14] = (tmp2 & 0x0000FFFF) ? 2 : 0; /* [13] || [12] */
            bS[0][15] = (tmp2 & 0xFFFF0000) ? 2 : 0; /* [15] || [14] */

            bS[1][8] = (u8)InnerBoundaryStrength(mb, 8, 2);
            bS[1][9] = (u8)InnerBoundaryStrength(mb, 9, 3);
            bS[1][10] = (u8)InnerBoundaryStrength(mb, 12, 6);
            bS[1][11] = (u8)InnerBoundaryStrength(mb, 13, 7);

            {
            u32 tmp3, tmp4;

            tmp1 = mb->totalCoeff[4];
            tmp2 = mb->totalCoeff[1];
            tmp3 = mb->totalCoeff[6];
            tmp4 = mb->totalCoeff[3];
            bS[0][8] = tmp1 || tmp2 ? 2 : 0;
            tmp1 = mb->totalCoeff[12];
            tmp2 = mb->totalCoeff[9];
            bS[0][9] = tmp3 || tmp4 ? 2 : 0;
            tmp3 = mb->totalCoeff[14];
            tmp4 = mb->totalCoeff[11];
            bS[0][10] = tmp1 || tmp2 ? 2 : 0;
            bS[0][11] = tmp3 || tmp4 ? 2 : 0;
            }
        }
        /* 8x16 inter mb -> ref addresses and motion vectors can be different
         * only for the middle vertical edge, for the other left edges it is
         * enough to check whether the blocks contain coefficients or not. The
         * same applies to all internal top edges. */
        else if (mb->mbType == P_L0_L0_8x16)
        {
            tmp1 = *pTmp++;
            tmp2 = *pTmp++;
            bS[1][4]  = (tmp1 & 0x00FF00FF) ? 2 : 0; /* [2]  || [0] */
            bS[1][5]  = (tmp1 & 0xFF00FF00) ? 2 : 0; /* [3]  || [1] */
            bS[0][4]  = (tmp1 & 0x0000FFFF) ? 2 : 0; /* [1]  || [0] */
            bS[0][5]  = (tmp1 & 0xFFFF0000) ? 2 : 0; /* [3]  || [2] */
            tmp1 = *pTmp++;
            bS[1][6]  = (tmp2 & 0x00FF00FF) ? 2 : 0; /* [6]  || [4] */
            bS[1][7]  = (tmp2 & 0xFF00FF00) ? 2 : 0; /* [7]  || [5] */
            bS[0][12] = (tmp2 & 0x0000FFFF) ? 2 : 0; /* [5]  || [4] */
            bS[0][13] = (tmp2 & 0xFFFF0000) ? 2 : 0; /* [7]  || [6] */
            tmp2 = *pTmp;
            bS[1][12] = (tmp1 & 0x00FF00FF) ? 2 : 0; /* [10] || [8] */
            bS[1][13] = (tmp1 & 0xFF00FF00) ? 2 : 0; /* [11] || [9] */
            bS[0][6]  = (tmp1 & 0x0000FFFF) ? 2 : 0; /* [9]  || [8] */
            bS[0][7]  = (tmp1 & 0xFFFF0000) ? 2 : 0; /* [11] || [10] */

            bS[1][14] = (tmp2 & 0x00FF00FF) ? 2 : 0; /* [14] || [12] */
            bS[1][15] = (tmp2 & 0xFF00FF00) ? 2 : 0; /* [15] || [13] */
            bS[0][14] = (tmp2 & 0x0000FFFF) ? 2 : 0; /* [13] || [12] */
            bS[0][15] = (tmp2 & 0xFFFF0000) ? 2 : 0; /* [15] || [14] */

            bS[0][8] = (u8)InnerBoundaryStrength(mb, 4, 1);
            bS[0][9] = (u8)InnerBoundaryStrength(mb, 6, 3);
            bS[0][10] = (u8)InnerBoundaryStrength(mb, 12, 9);
            bS[0][11] = (u8)InnerBoundaryStrength(mb, 14, 11);

            {
            u32 tmp3, tmp4;

            tmp1 = mb->totalCoeff[8];
            tmp2 = mb->totalCoeff[2];
            tmp3 = mb->totalCoeff[9];
            tmp4 = mb->totalCoeff[3];
            bS[1][8] = tmp1 || tmp2 ? 2 : 0;
            tmp1 = mb->totalCoeff[12];
            tmp2 = mb->totalCoeff[6];
            bS[1][9] = tmp3 || tmp4 ? 2 : 0;
            tmp3 = mb->totalCoeff[13];
            tmp4 = mb->totalCoeff[7];
            bS[1][10] = tmp1 || tmp2 ? 2 : 0;
            bS[1][11] = tmp3 || tmp4 ? 2 : 0;
            }
        }
        else
        {
            tmp1 = *pTmp++;
            bS[1][4] = (tmp1 & 0x00FF00FF) ? 2 : (u8)InnerBoundaryStrength2(mb, 2, 0);
            bS[1][5] = (tmp1 & 0xFF00FF00) ? 2 : (u8)InnerBoundaryStrength2(mb, 3, 1);
            bS[0][4] = (tmp1 & 0x0000FFFF) ? 2 : (u8)InnerBoundaryStrength2(mb, 1, 0);
            bS[0][5] = (tmp1 & 0xFFFF0000) ? 2 : (u8)InnerBoundaryStrength2(mb, 3, 2);
            tmp1 = *pTmp++;
            bS[1][6]  = (tmp1 & 0x00FF00FF) ? 2 : (u8)InnerBoundaryStrength2(mb, 6, 4);
            bS[1][7]  = (tmp1 & 0xFF00FF00) ? 2 : (u8)InnerBoundaryStrength2(mb, 7, 5);
            bS[0][12] = (tmp1 & 0x0000FFFF) ? 2 : (u8)InnerBoundaryStrength2(mb, 5, 4);
            bS[0][13] = (tmp1 & 0xFFFF0000) ? 2 : (u8)InnerBoundaryStrength2(mb, 7, 6);
            tmp1 = *pTmp++;
            bS[1][12] = (tmp1 & 0x00FF00FF) ? 2 : (u8)InnerBoundaryStrength2(mb, 10, 8);
            bS[1][13] = (tmp1 & 0xFF00FF00) ? 2 : (u8)InnerBoundaryStrength2(mb, 11, 9);
            bS[0][6]  = (tmp1 & 0x0000FFFF) ? 2 : (u8)InnerBoundaryStrength2(mb, 9, 8);
            bS[0][7]  = (tmp1 & 0xFFFF0000) ? 2 : (u8)InnerBoundaryStrength2(mb, 11, 10);
            tmp1 = *pTmp;
            bS[1][14] = (tmp1 & 0x00FF00FF) ? 2 : (u8)InnerBoundaryStrength2(mb, 14, 12);
            bS[1][15] = (tmp1 & 0xFF00FF00) ? 2 : (u8)InnerBoundaryStrength2(mb, 15, 13);
            bS[0][14] = (tmp1 & 0x0000FFFF) ? 2 : (u8)InnerBoundaryStrength2(mb, 13, 12);
            bS[0][15] = (tmp1 & 0xFFFF0000) ? 2 : (u8)InnerBoundaryStrength2(mb, 15, 14);

            bS[1][8] = (u8)InnerBoundaryStrength(mb, 8, 2);
            bS[1][9] = (u8)InnerBoundaryStrength(mb, 9, 3);
            bS[1][10] = (u8)InnerBoundaryStrength(mb, 12, 6);
            bS[1][11] = (u8)InnerBoundaryStrength(mb, 13, 7);

            bS[0][8] = (u8)InnerBoundaryStrength(mb, 4, 1);
            bS[0][9] = (u8)InnerBoundaryStrength(mb, 6, 3);
            bS[0][10] = (u8)InnerBoundaryStrength(mb, 12, 9);
            bS[0][11] = (u8)InnerBoundaryStrength(mb, 14, 11);
        }
        pTmp = (u32*)&bS[0][0];
        if (!nonZeroBs && (pTmp[1] || pTmp[2] || pTmp[3] ||
                           pTmp[5] || pTmp[6] || pTmp[7]) )
        {
            nonZeroBs = HANTRO_TRUE;
        }
    }

    return(nonZeroBs);

}

/*------------------------------------------------------------------------------

    Function: GetLumaEdgeThresholds

        Functional description:
            Compute alpha, beta and tc0 thresholds for inner, left and top
            luma edges of a macroblock.

------------------------------------------------------------------------------*/
void GetLumaEdgeThresholds(
    mbStorage_t *mb,
    u8 (*alpha)[2],
    u8 (*beta)[2],
    u8 (*threshold)[16],
    u8 (*bs)[16],
    u32 filteringFlags )
{

/* Variables */

    u32 indexA, indexB;
    u32 qpAv, qp, qpTmp;
    u32 i;

/* Code */

    ASSERT(threshold);
    ASSERT(bs);
    ASSERT(beta);
    ASSERT(alpha);
    ASSERT(mb);

    qp = mb->qpY;

    indexA = (u32)CLIP3(0, 51, (i32)qp + mb->filterOffsetA);
    indexB = (u32)CLIP3(0, 51, (i32)qp + mb->filterOffsetB);

    /* Internal edge values */
    alpha[0][1] = alphas[indexA];
    alpha[1][1] = alphas[indexA];
    alpha[1][0] = alphas[indexA];
    alpha[0][0] = alphas[indexA];
    beta[0][1] = betas[indexB];
    beta[1][1] = betas[indexB];
    beta[1][0] = betas[indexB];
    beta[0][0] = betas[indexB];

    /* vertical scan order */
    for (i = 0; i < 2; i++)
    {
        u32 t1, t2;

        t1 = bs[i][0];
        t2 = bs[i][1];
        threshold[i][0]  = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][2];
        threshold[i][1]  = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][3];
        threshold[i][2]  = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][4];
        threshold[i][3]  = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][5];
        threshold[i][4]  = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][6];
        threshold[i][5]  = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][7];
        threshold[i][6]  = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][8];
        threshold[i][7]  = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][9];
        threshold[i][8]  = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][10];
        threshold[i][9]  = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][11];
        threshold[i][10] = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][12];
        threshold[i][11] = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][13];
        threshold[i][12] = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][14];
        threshold[i][13] = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][15];
        threshold[i][14] = (t1) ? tc0[indexA][t1] : 0;
        threshold[i][15] = (t2) ? tc0[indexA][t2] : 0;
    }

    if (filteringFlags & FILTER_TOP_EDGE)
    {
        qpTmp = mb->mbB->qpY;
        if (qpTmp != qp)
        {
            u32 t1, t2, t3, t4;
            qpAv = (qp + qpTmp + 1) >> 1;

            indexA = (u32)CLIP3(0, 51, (i32)qpAv + mb->filterOffsetA);
            indexB = (u32)CLIP3(0, 51, (i32)qpAv + mb->filterOffsetB);

            alpha[1][0] = alphas[indexA];
            beta[1][0] = betas[indexB];
            t1 = bs[1][0];
            t2 = bs[1][1];
            t3 = bs[1][2];
            t4 = bs[1][3];
            threshold[1][0] = (t1 && (t1 < 4)) ? tc0[indexA][t1] : 0;
            threshold[1][1] = (t2 && (t2 < 4)) ? tc0[indexA][t2] : 0;
            threshold[1][2] = (t3 && (t3 < 4)) ? tc0[indexA][t3] : 0;
            threshold[1][3] = (t4 && (t4 < 4)) ? tc0[indexA][t4] : 0;
        }
    }
    if (filteringFlags & FILTER_LEFT_EDGE)
    {
        qpTmp = mb->mbA->qpY;
        if (qpTmp != qp)
        {
            qpAv = (qp + qpTmp + 1) >> 1;

            indexA = (u32)CLIP3(0, 51, (i32)qpAv + mb->filterOffsetA);
            indexB = (u32)CLIP3(0, 51, (i32)qpAv + mb->filterOffsetB);

            alpha[0][0] = alphas[indexA];
            beta[0][0] = betas[indexB];
            threshold[0][0] = (bs[0][0] && (bs[0][0] < 4)) ? tc0[indexA][bs[0][0]] : 0;
            threshold[0][1] = (bs[0][1] && (bs[0][1] < 4)) ? tc0[indexA][bs[0][1]] : 0;
            threshold[0][2] = (bs[0][2] && (bs[0][2] < 4)) ? tc0[indexA][bs[0][2]] : 0;
            threshold[0][3] = (bs[0][3] && (bs[0][3] < 4)) ? tc0[indexA][bs[0][3]] : 0;
        }
    }

}

/*------------------------------------------------------------------------------

    Function: GetChromaEdgeThresholds

        Functional description:
            Compute alpha, beta and tc0 thresholds for inner, left and top
            chroma edges of a macroblock.

------------------------------------------------------------------------------*/
void GetChromaEdgeThresholds(
    mbStorage_t *mb,
    u8 (*alpha)[2],
    u8 (*beta)[2],
    u8 (*threshold)[8],
    u8 (*bs)[16],
    u32 filteringFlags,
    i32 chromaQpIndexOffset)
{

/* Variables */

    u32 indexA, indexB;
    u32 qpAv, qp, qpTmp;
    u32 i;

/* Code */

    ASSERT(threshold);
    ASSERT(bs);
    ASSERT(beta);
    ASSERT(alpha);
    ASSERT(mb);
    ASSERT(mb);

    qp = mb->qpY;
    qp = h264bsdQpC[CLIP3(0, 51, (i32)qp + chromaQpIndexOffset)];

    indexA = (u32)CLIP3(0, 51, (i32)qp + mb->filterOffsetA);
    indexB = (u32)CLIP3(0, 51, (i32)qp + mb->filterOffsetB);

    alpha[0][1] = alphas[indexA];
    alpha[1][1] = alphas[indexA];
    alpha[1][0] = alphas[indexA];
    alpha[0][0] = alphas[indexA];
    beta[0][1] = betas[indexB];
    beta[1][1] = betas[indexB];
    beta[1][0] = betas[indexB];
    beta[0][0] = betas[indexB];

    for (i = 0; i < 2; i++)
    {
        u32 t1, t2;

        t1 = bs[i][0];
        t2 = bs[i][1];
        threshold[i][0]  = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][2];
        threshold[i][1]  = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][3];
        threshold[i][2]  = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][8];
        threshold[i][3]  = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][9];
        threshold[i][4]  = (t1) ? tc0[indexA][t1] : 0;
        t1 = bs[i][10];
        threshold[i][5]  = (t2) ? tc0[indexA][t2] : 0;
        t2 = bs[i][11];
        threshold[i][6]  = (t1) ? tc0[indexA][t1] : 0;
        threshold[i][7]  = (t2) ? tc0[indexA][t2] : 0;
    }

    if (filteringFlags & FILTER_TOP_EDGE)
    {
        qpTmp = mb->mbB->qpY;
        if (qpTmp != mb->qpY)
        {
            u32 t1, t2, t3, t4;
            qpTmp = h264bsdQpC[CLIP3(0, 51, (i32)qpTmp + chromaQpIndexOffset)];
            qpAv = (qp + qpTmp + 1) >> 1;

            indexA = (u32)CLIP3(0, 51, (i32)qpAv + mb->filterOffsetA);
            indexB = (u32)CLIP3(0, 51, (i32)qpAv + mb->filterOffsetB);

            alpha[1][0] = alphas[indexA];
            beta[1][0] = betas[indexB];

            t1 = bs[1][0];
            t2 = bs[1][1];
            t3 = bs[1][2];
            t4 = bs[1][3];
            threshold[1][0] = (t1) ? tc0[indexA][t1] : 0;
            threshold[1][1] = (t2) ? tc0[indexA][t2] : 0;
            threshold[1][2] = (t3) ? tc0[indexA][t3] : 0;
            threshold[1][3] = (t4) ? tc0[indexA][t4] : 0;
        }
    }
    if (filteringFlags & FILTER_LEFT_EDGE)
    {
        qpTmp = mb->mbA->qpY;
        if (qpTmp != mb->qpY)
        {

            qpTmp = h264bsdQpC[CLIP3(0, 51, (i32)qpTmp + chromaQpIndexOffset)];
            qpAv = (qp + qpTmp + 1) >> 1;

            indexA = (u32)CLIP3(0, 51, (i32)qpAv + mb->filterOffsetA);
            indexB = (u32)CLIP3(0, 51, (i32)qpAv + mb->filterOffsetB);

            alpha[0][0] = alphas[indexA];
            beta[0][0] = betas[indexB];
            threshold[0][0] = (bs[0][0]) ? tc0[indexA][bs[0][0]] : 0;
            threshold[0][1] = (bs[0][1]) ? tc0[indexA][bs[0][1]] : 0;
            threshold[0][2] = (bs[0][2]) ? tc0[indexA][bs[0][2]] : 0;
            threshold[0][3] = (bs[0][3]) ? tc0[indexA][bs[0][3]] : 0;
        }
    }

}

#endif /* H264DEC_OMXDL */

/*lint +e701 +e702 */

