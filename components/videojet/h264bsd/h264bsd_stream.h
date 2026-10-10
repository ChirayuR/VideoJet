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

#ifndef H264SWDEC_STREAM_H
#define H264SWDEC_STREAM_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

/* The read position is strmBuffReadBits alone. cache[] is a window on it:
 * stream words strmBuffReadBits/32 and the one after (bytes 4k..4k+3, the
 * first byte in the MSB), with bytes past strmBuffSize read as 0. Whoever
 * sets strmBuffReadBits or changes the buffer calls h264bsdStrmFill. */
typedef struct
{
    u8  *pStrmBuffStart;    /* pointer to start of stream buffer */
    u32  strmBuffSize;      /* size of stream buffer (bytes) */
    u32  strmBuffReadBits;  /* number of bits read from stream buffer */
    u32  cache[2];          /* window, see above */
} strmData_t;

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

u32 h264bsdGetBits(strmData_t *pStrmData, u32 numBits);

void h264bsdStrmFill(strmData_t *pStrmData);

u32 h264bsdIsByteAligned(strmData_t *);

/* The next 32 bits, first bit in the MSB, 0 past the end of the buffer */
static inline u32 h264bsdShowBits32(strmData_t *pStrmData)
{
    u32 bitPos = pStrmData->strmBuffReadBits & 31;

    return bitPos ? (pStrmData->cache[0] << bitPos) |
                    (pStrmData->cache[1] >> (32 - bitPos)) :
                    pStrmData->cache[0];
}

/* Remove numBits bits. Returns 0 (HANTRO_OK), or 0xFFFFFFFF (END_OF_STREAM)
 * when the position is now past the end of the buffer */
static inline u32 h264bsdFlushBits(strmData_t *pStrmData, u32 numBits)
{
    u32 readBits = pStrmData->strmBuffReadBits + numBits;
    u32 newWord = (readBits ^ pStrmData->strmBuffReadBits) >= 32;

    pStrmData->strmBuffReadBits = readBits;
    if (newWord)
        h264bsdStrmFill(pStrmData);

    return (readBits <= 8*pStrmData->strmBuffSize) ? 0 : 0xFFFFFFFFU;
}

#endif /* #ifdef H264SWDEC_STREAM_H */

