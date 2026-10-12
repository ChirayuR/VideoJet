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
          SkipNonZero
          MoveDown
          ExtractNalUnit

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include <string.h>
#include "h264bsd_byte_stream.h"
#include "h264bsd_util.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

#define BYTE_STREAM_ERROR  0xFFFFFFFF

/* a 32-bit load through a u8 pointer, only ever at a 4-byte aligned address */
typedef u32 __attribute__((may_alias)) AliasU32;

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

static const u8 *SkipNonZero(const u8 *p, const u8 *end);
static u8 *MoveDown(u8 *dst, const u8 *src, const u8 *end);

/*------------------------------------------------------------------------------

    Function name: SkipNonZero

        Functional description:
            Finds the first zero byte in [p, end) four bytes at a time: after
            a scalar head up to a word boundary, a word holds a zero byte iff
            (w - 0x01010101) & ~w & 0x80808080 is not 0 (exact as a yes/no
            test, whatever the byte order). Reads only inside [p, end), and
            only aligned words.

        Returns:
            pointer to the first zero byte, or end if there is none

------------------------------------------------------------------------------*/

static const u8 *SkipNonZero(const u8 *p, const u8 *end)
{

    while (p < end && ((uintptr_t)p & 3))
    {
        if (*p == 0)
            return(p);
        p++;
    }
    while ((size_t)(end - p) >= 4)
    {
        u32 w = *(const AliasU32 *)p;

        /* a zero byte in the word is the rare case: keep the fall-through
         * path free of taken branches */
        if (__builtin_expect(((w - 0x01010101U) & ~w & 0x80808080U) != 0, 0))
            break;
        p += 4;
    }
    /* a word with a zero byte, or the tail */
    while (p < end && *p)
        p++;

    return(p);

}

/*------------------------------------------------------------------------------

    Function name: MoveDown

        Functional description:
            Moves [src, end) down to dst (dst <= src, the ranges may overlap).

        Returns:
            dst + the number of bytes moved

------------------------------------------------------------------------------*/

static u8 *MoveDown(u8 *dst, const u8 *src, const u8 *end)
{

    if (dst != src)
        memmove(dst, src, (size_t)(end - src));

    return(dst + (end - src));

}

/*------------------------------------------------------------------------------

    Function name: ExtractNalUnit

        Functional description:
            Extracts one NAL unit from the byte stream buffer. Removes
            emulation prevention bytes if present. The original stream buffer
            is used directly and is therefore modified if emulation prevention
            bytes are present in the stream.

            Stream buffer is assumed to contain either exactly one NAL unit
            and nothing else, or one or more NAL units embedded in byte
            stream format described in the Annex B of the standard. Function
            detects which one is used based on the first bytes in the buffer.

        Inputs:
            pByteStream     pointer to byte stream buffer
            len             length of the stream buffer (in bytes)

        Outputs:
            pStrmData       stream information is stored here
            readBytes       number of bytes "consumed" from the stream buffer

        Returns:
            HANTRO_OK       success
            HANTRO_NOK      error in byte stream

------------------------------------------------------------------------------*/

u32 h264bsdExtractNalUnit(u8 *pByteStream, u32 len, strmData_t *pStrmData,
    u32 *readBytes)
{

/* Variables */

    u32 byteCount, initByteCount;
    u32 zeroCount;
    u8  byte;
    u32 hasEmulation = HANTRO_FALSE;
    u32 invalidStream = HANTRO_FALSE;
    const u8 *readPtr, *end;
    u8 *dst;
    const u8 *src;

/* Code */

    ASSERT(pByteStream);
    ASSERT(len);
    ASSERT(len < BYTE_STREAM_ERROR);
    ASSERT(pStrmData);

    end = pByteStream + len;

    /* byte stream format if starts with 0x000001 or 0x000000 */
    if (len > 3 && pByteStream[0] == 0x00 && pByteStream[1] == 0x00 &&
        (pByteStream[2]&0xFE) == 0x00)
    {
        /* search for NAL unit start point, i.e. point after first start code
         * prefix in the stream (a start code prefix is seldom more than a few
         * bytes in, so this loop stays bytewise) */
        zeroCount = byteCount = 2;
        readPtr = pByteStream + 2;
        /*lint -e(716) while(1) used consciously */
        while (1)
        {
            byte = *readPtr++;
            byteCount++;

            if (byteCount == len)
            {
                /* no start code prefix found -> error */
                *readBytes = len;
                return(HANTRO_NOK);
            }

            if (!byte)
                zeroCount++;
            else if ((byte == 0x01) && (zeroCount >= 2))
                break;
            else
                zeroCount = 0;
        }

        initByteCount = byteCount;

        /* determine size of the NAL unit. Search for next start code prefix
         * or end of stream and ignore possible trailing zero bytes.
         *
         * Only a run of two or more zero bytes can matter, and what matters
         * is the byte that ends it (zeroCount = the run's length, counted
         * from the start of the NAL unit):
         *   - 0x01: the next start code prefix, the NAL unit ends before
         *     the run;
         *   - 0x03 after exactly two zeros: an emulation prevention byte;
         *   - anything else after three or more zeros: invalid stream.
         * Bytes that are not zero are skipped a word at a time. */
        zeroCount = 0;
        /*lint -e(716) while(1) used consciously */
        while (1)
        {
            while (readPtr < end && *readPtr == 0)
            {
                zeroCount++;
                readPtr++;
            }

            if (readPtr == end)
            {
                /* end of stream: trailing zeros are not part of the NAL unit */
                pStrmData->strmBuffSize = len - initByteCount - zeroCount;
                break;
            }

            /* a byte that is not zero: it ends the run */
            if (zeroCount >= 2)
            {
                if (*readPtr == 0x01)
                {
                    byteCount = (u32)(readPtr - pByteStream);
                    pStrmData->strmBuffSize =
                        byteCount - initByteCount - zeroCount;
                    zeroCount -= MIN(zeroCount, 3);
                    break;
                }
                if (zeroCount == 2)
                {
                    if (*readPtr == 0x03)
                        hasEmulation = HANTRO_TRUE;
                }
                else
                    invalidStream = HANTRO_TRUE;
            }
            zeroCount = 0;
            readPtr = SkipNonZero(readPtr + 1, end);
        }
    }
    /* separate NAL units as input -> just set stream params */
    else
    {
        initByteCount = 0;
        zeroCount = 0;
        pStrmData->strmBuffSize = len;
        hasEmulation = HANTRO_TRUE;
    }

    pStrmData->pStrmBuffStart    = pByteStream + initByteCount;
    pStrmData->strmBuffReadBits  = 0;

    /* return number of bytes "consumed" */
    *readBytes = pStrmData->strmBuffSize + initByteCount + zeroCount;

    if (invalidStream)
    {
        return(HANTRO_NOK);
    }

    /* remove emulation prevention bytes before rbsp processing */
    if (hasEmulation)
    {
        /* [src, readPtr) has been read and is not moved down to dst yet: the
         * bytes between emulation prevention bytes move in one piece */
        dst = pStrmData->pStrmBuffStart;
        src = dst;
        end = src + pStrmData->strmBuffSize;
        readPtr = src;
        zeroCount = 0;
        /*lint -e(716) while(1) used consciously */
        while (1)
        {
            while (readPtr < end && *readPtr == 0)
            {
                /* NAL unit shall not contain byte sequence 0x000000 */
                if (zeroCount == 2)
                    goto nalError;
                zeroCount++;
                readPtr++;
            }

            if (readPtr == end)
                break;

            /* a byte that is not zero */
            if (zeroCount == 2)
            {
                if (*readPtr == 0x03)
                {
                    /* emulation prevention byte shall be followed by one of
                     * the following bytes: 0x00, 0x01, 0x02, 0x03. This
                     * implies that emulation prevention 0x03 byte shall not
                     * be the last byte of the stream. */
                    if (readPtr + 1 == end || readPtr[1] > 0x03)
                        goto nalError;

                    /* do not write emulation prevention byte */
                    dst = MoveDown(dst, src, readPtr);
                    src = ++readPtr;
                    zeroCount = 0;
                    continue;
                }
                /* NAL unit shall not contain byte sequences 0x000001 or
                 * 0x000002 */
                if (*readPtr <= 0x02)
                    goto nalError;
            }
            zeroCount = 0;
            readPtr = SkipNonZero(readPtr + 1, end);
        }

        /* (end - dst) indicates number of "removed" emulation prevention
         * bytes -> subtract from stream buffer size */
        dst = MoveDown(dst, src, end);
        pStrmData->strmBuffSize -= (u32)(end - dst);
    }

    h264bsdStrmFill(pStrmData);

    return(HANTRO_OK);

nalError:
    /* the bytes before the offending one are in place, as if moved one by
     * one */
    MoveDown(dst, src, readPtr);
    return(HANTRO_NOK);

}
