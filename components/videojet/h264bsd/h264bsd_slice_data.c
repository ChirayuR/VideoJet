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
          vjParseBegin
          vjParseNext
          h264bsdDecodeSliceData
          SetMbParams
          h264bsdMarkSliceCorrupted

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "h264bsd_slice_data.h"
#include "h264bsd_util.h"
#include "h264bsd_vlc.h"
#include "vj_prof.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

static void SetMbParams(mbStorage_t *pMb, sliceHeader_t *pSlice, u32 sliceId,
    i32 chromaQpIndexOffset);

/*------------------------------------------------------------------------------

   5.0  Function name: vjParseBegin, vjParseNext

        Functional description:
            The slice's bitstream side, split out of h264bsdDecodeSliceData
            (VideoJet): skip_run, macroblock_layer, more_rbsp_data and the
            next-address bookkeeping of the macroblock loop, one record at a
            time. A record is a parsed macroblock (kind MB, layer filled), a
            run of skipped macroblocks (SKIPRUN, count; layer.mbType = P_Skip,
            layer.mbPred zero) or an error (ERR, status). last: no stream
            data follows the record, so the slice ends with it (for a
            SKIPRUN, with its final macroblock).

            The parser is a function of the bitstream, the slice header and
            its own state: it reads no mbStorage the reconstruction writes
            (nC comes from the shadow of the neighbours' totalCoeff, vjNbr),
            and writes only the slot and its state. So the reconstruction
            side may take the records later, in order, and an error record is
            reached exactly where the sequential decoder returned it.

            Shadow rule (one slice group, macroblocks visited first, first+1,
            ...): the left neighbour is available iff the column is not 0 and
            the macroblock before was parsed in this slice, the upper iff the
            one a row before was; the shadow is cleared at the slice start, so
            an entry is set only for a macroblock of this slice. With slice
            groups the neighbours' totalCoeff are read from the storage, as
            before (the sequential path only).

------------------------------------------------------------------------------*/

static const u8 vjZero24[24];
static const u8 vjSixteen24[24] =
    {16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16};

void vjParseBegin(vjParse_t *p, storage_t *pStorage, const strmData_t *pStrm,
    const sliceHeader_t *pSliceHeader)
{
    u32 i, w;

    w = pStorage->activeSps->picWidthInMbs;
    p->strm = *pStrm;
    p->sliceType = pSliceHeader->sliceType;
    p->numRefIdxActive = pSliceHeader->numRefIdxL0Active;
    p->picSizeInMbs = pStorage->picSizeInMbs;
    p->width = w;
    p->addr = pSliceHeader->firstMbInSlice;
    p->prevSkipped = HANTRO_FALSE;
    p->fin = HANTRO_FALSE;
    p->useStorage = pStorage->activePps->numSliceGroups != 1;
#ifdef VJ_NBR_STORAGE /* test: the slice-group path for every slice */
    p->useStorage = HANTRO_TRUE;
#endif
    p->mb = pStorage->mb;
    p->sliceGroupMap = pStorage->sliceGroupMap;
    p->sliceId = pStorage->slice->sliceId;
    p->nbr = pStorage->vjNbr;
    p->col = p->addr % w;
    p->idx = p->addr % (w + 1);
    for (i = 0; i <= w; i++)
        p->nbr[i].st = VJ_NBR_NONE;
}

static const u8 *NbrTc(const vjNbr_t *n)
{
    switch (n->st)
    {
        case VJ_NBR_TC:   return n->tc;
        case VJ_NBR_ZERO: return vjZero24;
        case VJ_NBR_PCM:  return vjSixteen24;
        default:          return NULL;
    }
}

/* the next macroblock address of the slice group, 0 at its end */
static u32 ParseNextAddr(const vjParse_t *p, u32 addr)
{
    if (p->useStorage)
        return h264bsdNextMbAddress(p->sliceGroupMap, p->picSizeInMbs, addr);
    return (addr + 1 == p->picSizeInMbs) ? 0 : addr + 1;
}

/* the macroblock at addr was parsed (or skipped): its shadow entry, and move on one macroblock */
static void ParseAdvance(vjParse_t *p, u32 st, const i16 *tc)
{
    if (!p->useStorage)
    {
        vjNbr_t *n = p->nbr + p->idx;
        u32 i;

        n->st = (u8)st;
        if (st == VJ_NBR_TC)
            for (i = 0; i < 24; i++)
                n->tc[i] = (u8)tc[i];
        if (++p->idx > p->width)
            p->idx = 0;
        if (++p->col == p->width)
            p->col = 0;
    }
    p->addr = ParseNextAddr(p, p->addr);
}

void vjParseNext(vjParse_t *p, vjSlot_t *s)
{
    u32 tmp, skipRun, i;
    const u8 *tcA, *tcB;
    macroblockLayer_t *mbLayer = &s->layer;

    ASSERT(!p->fin);

#ifdef VJ_POISON /* test: every slot starts as the previous macroblock's bytes, here random ones */
    vjPoison(s);
#endif

    if (!IS_I_SLICE(p->sliceType))
    {
        if (!p->prevSkipped)
        {
            tmp = h264bsdDecodeExpGolombUnsigned(&p->strm, &skipRun);
            if (tmp != HANTRO_OK)
                goto err;
            /* skip_run shall be less than or equal to number of
             * macroblocks left */
            if (skipRun > (p->picSizeInMbs - p->addr))
            {
                EPRINT("skip_run");
                tmp = HANTRO_NOK;
                goto err;
            }
            if (skipRun)
            {
                p->prevSkipped = HANTRO_TRUE;
                memset(&mbLayer->mbPred, 0, sizeof(mbPred_t));
                /* mark current macroblock skipped */
                mbLayer->mbType = P_Skip;
                s->kind = VJ_REC_SKIPRUN;
                s->count = skipRun;
                s->last = h264bsdMoreRbspData(&p->strm) ?
                    HANTRO_FALSE : HANTRO_TRUE;
                for (i = skipRun; i--;)
                {
                    ParseAdvance(p, VJ_NBR_ZERO, NULL);
                    if (!p->addr)
                        break;
                }
                if (s->last || !p->addr)
                    p->fin = HANTRO_TRUE;
                return;
            }
        }
    }

    p->prevSkipped = HANTRO_FALSE;

    if (p->useStorage)
    {
        mbStorage_t *pMb = p->mb + p->addr;
        mbStorage_t *mbA = pMb->mbA, *mbB = pMb->mbB;

        tcA = tcB = NULL;
        if (mbA && mbA->sliceId == p->sliceId)
        {
            for (i = 0; i < 24; i++)
                p->tmpA[i] = (u8)mbA->totalCoeff[i];
            tcA = p->tmpA;
        }
        if (mbB && mbB->sliceId == p->sliceId)
        {
            for (i = 0; i < 24; i++)
                p->tmpB[i] = (u8)mbB->totalCoeff[i];
            tcB = p->tmpB;
        }
    }
    else
    {
        tcA = p->col ? NbrTc(p->nbr + (p->idx ? p->idx - 1 : p->width)) : NULL;
        tcB = NbrTc(p->nbr + (p->idx == p->width ? 0 : p->idx + 1));
    }

    tmp = h264bsdDecodeMacroblockLayer(&p->strm, mbLayer, tcA, tcB,
        p->sliceType, p->numRefIdxActive);
    if (tmp != HANTRO_OK)
    {
        EPRINT("macroblock_layer");
        goto err;
    }

    s->kind = VJ_REC_MB;
    s->count = 1;
    /* skipRun is 0 here: more data is all that keeps the slice going */
    s->last = h264bsdMoreRbspData(&p->strm) ? HANTRO_FALSE : HANTRO_TRUE;
    if (mbLayer->mbType == I_PCM)
        i = VJ_NBR_PCM;
    else if (!mbLayer->codedBlockPattern &&
             h264bsdMbPartPredMode(mbLayer->mbType) != PRED_MODE_INTRA16x16)
        i = VJ_NBR_ZERO; /* no residual: the lazy clear left totalCoeff 0 */
    else
        i = VJ_NBR_TC;
    ParseAdvance(p, i, mbLayer->residual.totalCoeff);
    if (s->last || !p->addr)
        p->fin = HANTRO_TRUE;
    return;

err:
    s->kind = VJ_REC_ERR;
    s->status = tmp;
    s->last = HANTRO_TRUE;
    p->fin = HANTRO_TRUE;
}

/*------------------------------------------------------------------------------

   5.1  Function name: h264bsdDecodeSliceData

        Functional description:
            Decode one slice. Function decodes stream data, i.e. macroblocks
            and possible skip_run fields. h264bsdDecodeMacroblock function is
            called to handle all other macroblock related processing.
            Macroblock to slice group mapping is considered when next
            macroblock to process is determined (h264bsdNextMbAddress function)
            map

        Inputs:
            pStrmData       pointer to stream data structure
            pStorage        pointer to storage structure
            currImage       pointer to current processed picture, needed for
                            intra prediction of the macroblocks
            pSliceHeader    pointer to slice header of the current slice

        Outputs:
            currImage       processed macroblocks are written to current image
            pStorage        mbStorage structure of each processed macroblock
                            is updated here

        Returns:
            HANTRO_OK       success
            HANTRO_NOK      invalid stream data

------------------------------------------------------------------------------*/

u32 h264bsdDecodeSliceData(strmData_t *pStrmData, storage_t *pStorage,
    image_t *currImage, sliceHeader_t *pSliceHeader)
{

/* Variables */

    u8 mbData[384 + 15 + 32];
    u8 *data;
    u32 tmp;
    u32 skipLeft;
    u32 currMbAddr;
    u32 moreMbs;
    u32 mbCount;
    i32 qpY;
    vjParse_t *parse;
    vjSlot_t *slot;
    macroblockLayer_t *mbLayer;

/* Code */

    ASSERT(pStrmData);
    ASSERT(pSliceHeader);
    ASSERT(pStorage);
    ASSERT(pSliceHeader->firstMbInSlice < pStorage->picSizeInMbs);

    /* ensure 16-byte alignment */
    data = (u8*)ALIGN(mbData, 16);

    parse = pStorage->vjParse;
    slot = pStorage->vjRing;
    mbLayer = &slot->layer;

    currMbAddr = pSliceHeader->firstMbInSlice;
    skipLeft = 0;

    /* increment slice index, will be one for decoding of the first slice of
     * the picture */
    pStorage->slice->sliceId++;
    vjParseBegin(parse, pStorage, pStrmData, pSliceHeader);

    /* lastMbAddr stores address of the macroblock that was last successfully
     * decoded, needed for error handling */
    pStorage->slice->lastMbAddr = 0;

    mbCount = 0;
    /* initial quantization parameter for the slice is obtained as the sum of
     * initial QP for the picture and sliceQpDelta for the current slice */
    qpY = (i32)pStorage->activePps->picInitQp + pSliceHeader->sliceQpDelta;
    do
    {
        /* first touch of this MB in the picture (its sliceId is older): drop
         * an older picture's decoded count, the per-picture clear done
         * lazily. SetMbParams and decoded++ write both lines anyway. */
        if (pStorage->mb[currMbAddr].sliceId < pStorage->picSliceBase)
            pStorage->mb[currMbAddr].decoded = 0;

        /* primary picture and already decoded macroblock -> error */
        if (!pSliceHeader->redundantPicCnt && pStorage->mb[currMbAddr].decoded)
        {
            EPRINT("Primary and already decoded");
            return(HANTRO_NOK);
        }

        SetMbParams(pStorage->mb + currMbAddr, pSliceHeader,
            pStorage->slice->sliceId, pStorage->activePps->chromaQpIndexOffset);

        /* the next record: a macroblock, a run of skipped ones, or the error
         * the sequential decoder returned here (after the checks above) */
        if (!skipLeft)
        {
            { VJP_BEGIN(VJP_PARSE);
            vjParseNext(parse, slot);
            VJP_END(VJP_PARSE); }
            if (slot->kind == VJ_REC_ERR)
                return(slot->status);
            if (slot->kind == VJ_REC_SKIPRUN)
                skipLeft = slot->count;
        }

        if (skipLeft)
        {
            DEBUG(("Skipping macroblock %d\n", currMbAddr));
            skipLeft--;
        }

        { VJP_BEGIN(VJP_RECON);
        tmp = h264bsdDecodeMacroblock(pStorage->mb + currMbAddr, mbLayer,
            currImage, pStorage->dpb, &qpY, currMbAddr,
            pStorage->activePps->constrainedIntraPredFlag, data);
        VJP_END(VJP_RECON); }
        if (tmp != HANTRO_OK)
        {
            EPRINT("MACRO_BLOCK");
            return(tmp);
        }

        /* deblock record, made while this MB's and its left and top
         * neighbours' storage is in cache. While the picture's MBs come
         * once each in raster order those neighbours are final and nothing
         * rewrites them or this MB before the deblock pass; the first MB
         * out of that order turns the picture's records off */
        if (currMbAddr == pStorage->vjRecNext)
        {
            h264bsdDeblockRecord(pStorage->mb + currMbAddr,
                pStorage->vjRec + currMbAddr, pStorage->picSliceBase, 1);
            pStorage->vjRecNext++;
#ifdef VJ_DEBLOCK_WORKER
            /* a row is complete: the deblock worker may filter those behind it */
            if (pStorage->vjRecNext == pStorage->vjDbwRowEnd)
                pStorage->vjDbwRowEnd = h264bsdDbwRowEnd(pStorage->vjDbw);
#endif
        }
        else
            pStorage->vjRecNext = VJ_REC_OFF;

        /* increment macroblock count only for macroblocks that were decoded
         * for the first time (redundant slices) */
        if (pStorage->mb[currMbAddr].decoded == 1)
            mbCount++;

        /* keep on processing as long as there is stream data left or
         * processing of macroblocks to be skipped based on the last skipRun is
         * not finished */
        moreMbs = (skipLeft || !slot->last) ? HANTRO_TRUE : HANTRO_FALSE;

        /* lastMbAddr is only updated for intra slices (all macroblocks of
         * inter slices will be lost in case of an error) */
        if (IS_I_SLICE(pSliceHeader->sliceType))
            pStorage->slice->lastMbAddr = currMbAddr;

        currMbAddr = h264bsdNextMbAddress(pStorage->sliceGroupMap,
            pStorage->picSizeInMbs, currMbAddr);
        /* data left in the buffer but no more macroblocks for current slice
         * group -> error */
        if (moreMbs && !currMbAddr)
        {
            EPRINT("Next mb address");
            return(HANTRO_NOK);
        }

    } while (moreMbs);

    if ((pStorage->slice->numDecodedMbs + mbCount) > pStorage->picSizeInMbs)
    {
        EPRINT("Num decoded mbs");
        return(HANTRO_NOK);
    }

    pStorage->slice->numDecodedMbs += mbCount;

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

   5.2  Function: SetMbParams

        Functional description:
            Set macroblock parameters that remain constant for this slice

        Inputs:
            pSlice      pointer to current slice header
            sliceId     id of the current slice
            chromaQpIndexOffset

        Outputs:
            pMb         pointer to macroblock structure which is updated

        Returns:
            none

------------------------------------------------------------------------------*/

void SetMbParams(mbStorage_t *pMb, sliceHeader_t *pSlice, u32 sliceId,
    i32 chromaQpIndexOffset)
{

/* Variables */
    u32 tmp1;
    i32 tmp2, tmp3;

/* Code */

    tmp1 = pSlice->disableDeblockingFilterIdc;
    tmp2 = pSlice->sliceAlphaC0Offset;
    tmp3 = pSlice->sliceBetaOffset;
    pMb->sliceId = sliceId;
    pMb->disableDeblockingFilterIdc = tmp1;
    pMb->filterOffsetA = tmp2;
    pMb->filterOffsetB = tmp3;
    pMb->chromaQpIndexOffset = chromaQpIndexOffset;

}

/*------------------------------------------------------------------------------

   5.3  Function name: h264bsdMarkSliceCorrupted

        Functional description:
            Mark macroblocks of the slice corrupted. If lastMbAddr in the slice
            storage is set -> picWidhtInMbs (or at least 10) macroblocks back
            from  the lastMbAddr are marked corrupted. However, if lastMbAddr
            is not set -> all macroblocks of the slice are marked.

        Inputs:
            pStorage        pointer to storage structure
            firstMbInSlice  address of the first macroblock in the slice, this
                            identifies the slice to be marked corrupted

        Outputs:
            pStorage        mbStorage for the corrupted macroblocks updated

        Returns:
            none

------------------------------------------------------------------------------*/

void h264bsdMarkSliceCorrupted(storage_t *pStorage, u32 firstMbInSlice)
{

/* Variables */

    u32 tmp, i;
    u32 sliceId;
    u32 currMbAddr;

/* Code */

    ASSERT(pStorage);
    ASSERT(firstMbInSlice < pStorage->picSizeInMbs);

    currMbAddr = firstMbInSlice;

    sliceId = pStorage->slice->sliceId;

    /* DecodeSliceData sets lastMbAddr for I slices -> if it was set, go back
     * MAX(picWidthInMbs, 10) macroblocks and start marking from there */
    if (pStorage->slice->lastMbAddr)
    {
        ASSERT(pStorage->mb[pStorage->slice->lastMbAddr].sliceId == sliceId);
        i = pStorage->slice->lastMbAddr - 1;
        tmp = 0;
        while (i > currMbAddr)
        {
            if (pStorage->mb[i].sliceId == sliceId)
            {
                tmp++;
                if (tmp >= MAX(pStorage->activeSps->picWidthInMbs, 10))
                    break;
            }
            i--;
        }
        currMbAddr = i;
    }

    do
    {

        if ( (pStorage->mb[currMbAddr].sliceId == sliceId) &&
             (pStorage->mb[currMbAddr].decoded) )
        {
            pStorage->mb[currMbAddr].decoded--;
        }
        else
        {
            break;
        }

        currMbAddr = h264bsdNextMbAddress(pStorage->sliceGroupMap,
            pStorage->picSizeInMbs, currMbAddr);

    } while (currMbAddr);

}

