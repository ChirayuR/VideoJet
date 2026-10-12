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

#ifndef H264SWDEC_DEBLOCKING_H
#define H264SWDEC_DEBLOCKING_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_image.h"
#include "h264bsd_macroblock_layer.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

/* What the deblock pass needs of one macroblock (h264bsdDeblockRecord): the
 * bS of its 32 edge segments, 2 bits each, top edges of 4x4 blocks 0-15 in
 * bs[0] and left edges in bs[1] (code 3: bS 3 on an inner edge, 4 on an MB
 * edge, which is never 3), both 0 when it filters nothing; and the inputs of
 * its edge thresholds */
typedef struct
{
    u32 bs[2];
    u8 qp;
    i8 offA;
    i8 offB;
    i8 chromaQpOff;
} deblockRec_t;

/* the deblock worker's state (h264bsd_deblocking.c, VJ_DEBLOCK_WORKER) */
typedef struct dbw dbw_t;

#ifdef VJ_DEBLOCK_WORKER
/* With the worker, a picture's deblock records live in a ring of VJ_REC_ROWS macroblock rows, not in a picture-sized
 * array: the records are read only by the worker, a row or two behind the decoding task, and by the final pass. Rows the
 * ring no longer holds are remade from the storage by that pass. At least 4: row r is filtered once row r + 1 is decoded,
 * and the decoding task may write row r + VJ_REC_ROWS only after the worker is done with row r + 1. */
#ifndef VJ_REC_ROWS
#define VJ_REC_ROWS 4
#endif
#if VJ_REC_ROWS < 4
#error "VJ_REC_ROWS must be at least 4"
#endif
#endif

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

void h264bsdDeblockRecord(mbStorage_t *mb, deblockRec_t *rec,
  u32 picSliceBase, u32 justDecoded);

void h264bsdFilterPicture(
  image_t *image,
  mbStorage_t *mb, u32 picSliceBase,
  u8 *changed, deblockRec_t *rec, u32 recRows, u32 recValid, u32 rowFrom);

#ifdef VJ_DEBLOCK_WORKER
dbw_t *h264bsdDbwCreate(void);
void h264bsdDbwDestroy(dbw_t *d);
void h264bsdDbwSetStep(dbw_t *d, int (*step)(void *, int), void *arg);
void h264bsdDbwKick(dbw_t *d);
void h264bsdDbwWake(dbw_t *d);
void h264bsdDbwStats(dbw_t *d, unsigned *kicks, unsigned *sleeps, unsigned *wakes);
u32 h264bsdDbwParallel(dbw_t *d);
u32 h264bsdDbwBegin(dbw_t *d, image_t *image, deblockRec_t *rec, u32 recRows,
  u8 *changed, u32 numSliceGroups, u32 redundantPicCntPresent);
void h264bsdDbwSliceStart(dbw_t *d, u32 firstMb, u32 isP, u32 recNext);
u32 h264bsdDbwRowEnd(dbw_t *d);
u32 h264bsdDbwActive(const dbw_t *d);
u32 h264bsdDbwBusy(dbw_t *d);
void h264bsdDbwAbortError(dbw_t *d);
void h264bsdDbwAbortKeep(dbw_t *d);
u32 h264bsdDbwResume(dbw_t *d);
u32 h264bsdDbwFinishable(dbw_t *d);
u32 h264bsdDbwRecCovers(dbw_t *d, u32 height, u32 from);
void h264bsdDbwJoin(dbw_t *d);
void h264bsdDbwFinish(dbw_t *d);
void h264bsdDbwRingStats(dbw_t *d, unsigned *waits, unsigned *lost,
  unsigned *remade);
#endif

#endif /* #ifdef H264SWDEC_DEBLOCKING_H */

