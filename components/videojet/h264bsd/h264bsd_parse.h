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
#define VJ_RING_DEPTH 1
#endif

enum { VJ_REC_MB = 0, VJ_REC_SKIPRUN = 1, VJ_REC_ERR = 2 };

typedef struct
{
    macroblockLayer_t layer;    /* 16-aligned: the PIE inverse transform stores into it */
    u32 kind;                   /* VJ_REC_* */
    u32 count;                  /* SKIPRUN: macroblocks skipped */
    u32 status;                 /* ERR: what the sequential decoder returned */
    u32 last;                   /* the slice's stream data ends after this record */
} vjSlot_t;

/* What a neighbour macroblock's totalCoeff[0..23] is, as the parser needs it for nC (CAVLC) */
enum { VJ_NBR_NONE = 0, VJ_NBR_TC = 1, VJ_NBR_ZERO = 2, VJ_NBR_PCM = 3 };

typedef struct
{
    u8 st;
    u8 tc[24];
} vjNbr_t;

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
    /* neighbour totalCoeff: a ring of width + 1 entries (shadow, one slice group) ... */
    vjNbr_t *nbr;
    u32 col;                    /* column of addr */
    u32 idx;                    /* addr's entry in nbr */
    /* ... or, with slice groups, read from the macroblock storage as the sequential decoder did */
    u32 useStorage;
    struct mbStorage *mb;
    u32 *sliceGroupMap;
    u32 sliceId;
    u8 tmpA[24], tmpB[24];
} vjParse_t;

#endif /* #ifndef H264SWDEC_PARSE_H */
