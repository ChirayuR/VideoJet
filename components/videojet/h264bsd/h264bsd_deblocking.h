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

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

void h264bsdDeblockRecord(mbStorage_t *mb, deblockRec_t *rec,
  u32 picSliceBase);

void h264bsdFilterPicture(
  image_t *image,
  mbStorage_t *mb, u32 picSliceBase,
  u8 *changed, deblockRec_t *rec, u32 recValid);

#endif /* #ifdef H264SWDEC_DEBLOCKING_H */

