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
          h264bsdGetBits
          h264bsdStrmFill
          h264bsdIsByteAligned

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "h264bsd_util.h"
#include "h264bsd_stream.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------

    Function: h264bsdGetBits

        Functional description:
            Read and remove bits from the stream buffer.

        Input:
            pStrmData   pointer to stream data structure
            numBits     number of bits to read

        Output:
            none

        Returns:
            bits read from stream
            END_OF_STREAM if not enough bits left

------------------------------------------------------------------------------*/

u32 h264bsdGetBits(strmData_t *pStrmData, u32 numBits)
{

    u32 out;

    ASSERT(pStrmData);
    ASSERT(numBits < 32);

    out = h264bsdShowBits32(pStrmData) >> (32 - numBits);

    if (h264bsdFlushBits(pStrmData, numBits) == HANTRO_OK)
    {
        return(out);
    }
    else
    {
        return(END_OF_STREAM);
    }

}

/*------------------------------------------------------------------------------

    Function: h264bsdStrmFill

        Functional description:
            Load the window (h264bsd_stream.h) for strmBuffReadBits:
            cache[0] = stream word strmBuffReadBits/32, cache[1] = the word
            after it, the first byte in the MSB. Bytes past the end of the
            buffer read as 0, as h264bsdShowBits32 returns them.

        Input:
            pStrmData   pointer to stream data structure

        Output:
            pStrmData   cache[] loaded

        Returns:
            none

------------------------------------------------------------------------------*/

static u32 StrmWord(const strmData_t *pStrmData, u32 pos)
{

    const u8 *pStrm = pStrmData->pStrmBuffStart;
    u32 i, out = 0;

    if (pos + 4 <= pStrmData->strmBuffSize)
        return(((u32)pStrm[pos] << 24) | ((u32)pStrm[pos+1] << 16) |
               ((u32)pStrm[pos+2] << 8) | (u32)pStrm[pos+3]);

    for (i = 0; i < 4; i++)
        out = (out << 8) |
              (pos + i < pStrmData->strmBuffSize ? (u32)pStrm[pos+i] : 0);
    return(out);

}

void h264bsdStrmFill(strmData_t *pStrmData)
{

    u32 pos = (pStrmData->strmBuffReadBits >> 5) << 2;

    ASSERT(pStrmData);
    ASSERT(pStrmData->pStrmBuffStart);

    pStrmData->cache[0] = StrmWord(pStrmData, pos);
    pStrmData->cache[1] = StrmWord(pStrmData, pos + 4);

}

/*------------------------------------------------------------------------------

    Function: h264bsdIsByteAligned

        Functional description:
            Check if current stream position is byte aligned.

        Inputs:
            pStrmData   pointer to stream data structure

        Outputs:
            none

        Returns:
            TRUE        stream is byte aligned
            FALSE       stream is not byte aligned

------------------------------------------------------------------------------*/

u32 h264bsdIsByteAligned(strmData_t *pStrmData)
{

/* Variables */

/* Code */

    if (!(pStrmData->strmBuffReadBits & 0x7))
        return(HANTRO_TRUE);
    else
        return(HANTRO_FALSE);

}

