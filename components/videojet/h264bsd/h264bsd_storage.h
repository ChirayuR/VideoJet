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
    2. Module defines
    3. Data types
    4. Function prototypes

------------------------------------------------------------------------------*/

#ifndef H264SWDEC_STORAGE_H
#define H264SWDEC_STORAGE_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_cfg.h"
#include "h264bsd_seq_param_set.h"
#include "h264bsd_pic_param_set.h"
#include "h264bsd_macroblock_layer.h"
#include "h264bsd_nal_unit.h"
#include "h264bsd_slice_header.h"
#include "h264bsd_seq_param_set.h"
#include "h264bsd_dpb.h"
#include "h264bsd_pic_order_cnt.h"
#include "h264bsd_deblocking.h"
#include "h264bsd_parse.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/* vjRecNext once the picture's deblock records may be stale */
#define VJ_REC_OFF 0xFFFFFFFFU

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

typedef struct
{
    u32 sliceId;
    u32 numDecodedMbs;
    u32 lastMbAddr;
} sliceStorage_t;

/* structure to store parameters needed for access unit boundary checking */
typedef struct
{
    nalUnit_t nuPrev[1];
    u32 prevFrameNum;
    u32 prevIdrPicId;
    u32 prevPicOrderCntLsb;
    i32 prevDeltaPicOrderCntBottom;
    i32 prevDeltaPicOrderCnt[2];
    u32 firstCallFlag;
} aubCheck_t;

/* storage data structure, holds all data of a decoder instance */
typedef struct storage
{
    /* active paramet set ids and pointers */
    u32 oldSpsId;
    u32 activePpsId;
    u32 activeSpsId;
    picParamSet_t *activePps;
    seqParamSet_t *activeSps;
    seqParamSet_t *sps[MAX_NUM_SEQ_PARAM_SETS];
    picParamSet_t *pps[MAX_NUM_PIC_PARAM_SETS];

    /* current slice group map, recomputed for each slice */
    u32 *sliceGroupMap;

    u32 picSizeInMbs;

    /* this flag is set after all macroblocks of a picture successfully
     * decoded -> redundant slices not decoded */
    u32 skipRedundantSlices;
    u32 picStarted;

    /* flag to indicate if current access unit contains any valid slices */
    u32 validSliceInAccessUnit;

    /* store information needed for handling of slice decoding */
    sliceStorage_t slice[1];

    /* "slice id 0" of the current picture. mb[] is not cleared per picture:
     * slice ids only grow, so an MB whose sliceId is below this has not been
     * touched in this picture (see h264bsdMbDecoded) */
    u32 picSliceBase;

    /* number of concealed macroblocks in the current image */
    u32 numConcealedMbs;

    /* picId given by application */
    u32 currentPicId;

    /* macroblock specific storages, size determined by image dimensions */
    mbStorage_t *mb;

    /* deblock record of each macroblock (h264bsdDeblockRecord), made as it
     * is decoded, and the next macroblock address in raster order. The
     * records hold for the deblock pass only if the picture's macroblocks
     * were decoded once each in raster order, with no slice error and no
     * concealment: vjRecNext == picSizeInMbs. VJ_REC_OFF once anything else
     * happened in the picture.
     * vjRec is vjRecRows rows of picWidthInMbs records: the whole picture
     * (vjRecRows == the picture's height) without the deblock worker, a ring
     * of VJ_REC_ROWS rows with it (macroblock n is in slot n % (vjRecRows *
     * width); the rows the ring no longer holds are remade from the storage
     * by the pass, see h264bsd_deblocking.c). vjRecCur is the slot of
     * vjRecNext, vjRecEnd the end of the ring (VJ_DEBLOCK_WORKER only) */
    deblockRec_t *vjRec;
    u32 vjRecNext;
    u32 vjRecRows;
    deblockRec_t *vjRecCur;
    deblockRec_t *vjRecEnd;

    /* deblock worker (VJ_DEBLOCK_WORKER; NULL when off): vjDbwRowEnd is the
     * vjRecNext at which it is told that another row is complete, 0 when it
     * takes no part in the picture. Always present, so the decode loop has
     * one layout */
    dbw_t *vjDbw;
    u32 vjDbwRowEnd;

    /* flag to store noOutputReordering flag set by the application */
    u32 noReordering;

    /* DPB */
    dpbStorage_t dpb[1];

    /* structure to store picture order count related information */
    pocStorage_t poc[1];

    /* access unit boundary checking related data */
    aubCheck_t aub[1];

    /* current processed image */
    image_t currImage[1];

    /* last valid NAL unit header is stored here */
    nalUnit_t prevNalUnit[1];

    /* slice header, second structure used as a temporary storage while
     * decoding slice header, first one stores last successfully decoded
     * slice header */
    sliceHeader_t sliceHeader[2];

    /* fields to store old stream buffer pointers, needed when only part of
     * a stream buffer is processed by h264bsdDecode function */
    u32 prevBufNotFinished;
    u8 *prevBufPointer;
    u32 prevBytesConsumed;
    strmData_t strm[1];

    /* the parser's records (VJ_RING_DEPTH slots, 16-aligned), its state, and
     * the shadow of the neighbours' totalCoeff (width + 1 entries) */
    vjSlot_t *vjRing;
    vjParse_t vjParse[1];
    vjNbr_t *vjNbr;

    u32 pendingActivation; /* Activate parameter sets after returning
                              HEADERS_RDY to the user */
    u32 intraConcealmentFlag; /* 0 gray picture for corrupted intra
                                 1 previous frame used if available */
    u32* conversionBuffer; // used to perform yuv conversion
    size_t conversionBufferSize;
} storage_t;

/* decoded count of mb in the current picture: 0 for an MB no slice of this
 * picture has touched yet, as the old per-picture clear left it. Use it
 * wherever mb may be untouched; once SetMbParams ran on mb, read it raw */
static inline u32 h264bsdMbDecoded(const mbStorage_t *mb, u32 picSliceBase)
{
    return mb->sliceId >= picSliceBase ? mb->decoded : 0;
}

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

void h264bsdInitStorage(storage_t *pStorage);
void h264bsdResetStorage(storage_t *pStorage);
u32 h264bsdIsStartOfPicture(storage_t *pStorage);
u32 h264bsdIsEndOfPicture(storage_t *pStorage);
u32 h264bsdStoreSeqParamSet(storage_t *pStorage, seqParamSet_t *pSeqParamSet);
u32 h264bsdStorePicParamSet(storage_t *pStorage, picParamSet_t *pPicParamSet);
u32 h264bsdActivateParamSets(storage_t *pStorage, u32 ppsId, u32 isIdr);
void h264bsdComputeSliceGroupMap(storage_t *pStorage,
    u32 sliceGroupChangeCycle);

u32 h264bsdCheckAccessUnitBoundary(
  strmData_t *strm,
  nalUnit_t *nuNext,
  storage_t *storage,
  u32 *accessUnitBoundaryFlag);

u32 h264bsdValidParamSets(storage_t *pStorage);

#endif /* #ifdef H264SWDEC_STORAGE_H */

