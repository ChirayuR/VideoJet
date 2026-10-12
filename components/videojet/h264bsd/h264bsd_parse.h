/*
 * Modified for VideoJet: new file, see h264bsd/README.md, Local changes (M3 speed: parse ring).
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

/* The slice parser's records and state. The parser (vjParseNext, h264bsd_slice_data.c) reads the bitstream and turns a
 * slice into records: one per parsed macroblock, one per run of skipped macroblocks, one for an error. It writes nothing
 * the reconstruction side or the deblocking reads (only its slot and its own state), so the reconstruction can consume
 * the records in order, on this thread or later on another one. */

#ifndef H264SWDEC_PARSE_H
#define H264SWDEC_PARSE_H

#include "basetype.h"
#include "h264bsd_stream.h"
#include "h264bsd_macroblock_layer.h"

/* slots in the ring (1 = parse and reconstruct alternate on one thread) */
#ifndef VJ_RING_DEPTH
#ifdef VJ_PARSE_PIPELINE
#define VJ_RING_DEPTH 6
#else
#define VJ_RING_DEPTH 1
#endif
#endif

/* parser ownership (vjParse_t.pstate): the worker task parses only while ARMED */
enum { VJ_PS_IDLE = 0, VJ_PS_ARMED = 1, VJ_PS_PARSING = 2, VJ_PS_DONE = 3 };
enum { VJ_MODE_INLINE = 0, VJ_MODE_PIPE = 1 };

enum { VJ_REC_MB = 0, VJ_REC_SKIPRUN = 1, VJ_REC_ERR = 2 };

typedef struct
{
    macroblockLayer_t layer;    /* 16-aligned: the PIE inverse transform stores into it */
    u32 kind;                   /* VJ_REC_* */
    u32 count;                  /* SKIPRUN: macroblocks skipped */
    u32 status;                 /* ERR: what the sequential decoder returned */
    u32 last;                   /* the slice's stream data ends after this record */
} vjSlot_t;

/* What a neighbour macroblock's totalCoeff[0..23] is, as the parser's shadow keeps it (VJ_PARSE_PIPELINE) */
enum { VJ_NBR_NONE = 0, VJ_NBR_TC = 1, VJ_NBR_ZERO = 2, VJ_NBR_PCM = 3 };

typedef struct
{
    u8 st;
    u8 tc[24];  /* only the entries a neighbour can supply to nC are set (vjCopyTc) */
} vjNbr_t;

/* The totalCoeff entries a neighbour macroblock can supply to nC: blocks 5 7 13 15 (left, luma), 10 11 14 15 (above,
 * luma), 17 19 21 23 (left, chroma) and 18 19 22 23 (above, chroma): the targets of the MB_A / MB_B rows of the
 * neighbour tables (test_nbr checks the set). Nothing else of the 24 is ever read. */
#define VJ_COPY_TC(dst, src) \
    do { \
        (dst)[5] = (u8)(src)[5];   (dst)[7] = (u8)(src)[7];   (dst)[10] = (u8)(src)[10]; \
        (dst)[11] = (u8)(src)[11]; (dst)[13] = (u8)(src)[13]; (dst)[14] = (u8)(src)[14]; \
        (dst)[15] = (u8)(src)[15]; (dst)[17] = (u8)(src)[17]; (dst)[18] = (u8)(src)[18]; \
        (dst)[19] = (u8)(src)[19]; (dst)[21] = (u8)(src)[21]; (dst)[22] = (u8)(src)[22]; \
        (dst)[23] = (u8)(src)[23]; \
    } while (0)

struct dbw; /* the worker, h264bsd_deblocking.h */

typedef struct vjParse
{
    strmData_t strm;            /* the parser's own read position */
    u32 sliceType;
    u32 numRefIdxActive;
    u32 picSizeInMbs;
    u32 width;                  /* picture width in macroblocks */
    u32 addr;                   /* address of the macroblock the next record starts at */
    u32 prevSkipped;            /* a skip run just ended: no skip_run field before the next macroblock */
    u32 fin;                    /* nothing more to parse: the last record was made, or the slice ran off its group */
    u32 singleGroup;            /* one slice group: the next address is addr + 1 */
    u32 sliceId;
    /* the neighbours' totalCoeff for nC come from the storage (mbA / mbB, available when they carry this slice's id) ... */
    struct mbStorage *mb;
    u32 *sliceGroupMap;
#ifdef VJ_PARSE_PIPELINE
    /* ... or, when the pipeline is on, from a shadow of the macroblocks parsed in this slice: a ring of width + 1 entries.
     * The storage is the reconstruction side's: the parser may not read it. Slice groups (and the VJ_NBR_STORAGE test)
     * read the storage as before, which only the sequential path may do. */
    vjNbr_t *nbr;
    u32 col;                    /* column of addr */
    u32 idx;                    /* addr's entry in nbr */
    u32 useStorage;
    u8 tmpA[24], tmpB[24];
    /* the ring: head = records published, tail = records consumed (free running, slot = count % VJ_RING_DEPTH).
     * Pipelined: the worker writes the slot, then head (release); the owner reads head (acquire), uses the slot in
     * place, then writes tail (release). pstate hands the whole context to the worker (ARMED) and back (IDLE). */
    u32 head, tail;
    u32 pstate;
    u32 mode;
    u32 sleeping;               /* the worker sleeps on a full ring: free a slot, then wake it (ParseConsume) */
    u32 holdoff;                /* slices left that parse inline after a take-back (the worker did not run) */
    struct dbw *dbw;            /* the worker to wake */
#endif
    vjSlot_t *ring;
} vjParse_t;

#endif /* #ifndef H264SWDEC_PARSE_H */
