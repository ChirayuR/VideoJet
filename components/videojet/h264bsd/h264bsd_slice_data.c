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
#include "h264bsd_inter_prediction.h"
#include "vj_prof.h"

#if defined(VJ_PARSE_PIPELINE) && !defined(VJ_DEBLOCK_WORKER)
#error "VJ_PARSE_PIPELINE needs VJ_DEBLOCK_WORKER (one core-1 task)"
#endif

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

#ifdef VJ_PARSE_PIPELINE
static const u8 vjZero24[24];
static const u8 vjSixteen24[24] =
    {16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16};

#endif

#ifdef VJ_POISON
/* test hook (make check-poison): every slot starts as some other macroblock's bytes, here random "plausible" ones, so
 * that a reader of stale slot contents changes pixels instead of passing by luck. vjPoisonSkip re-poisons a slot
 * between the macroblocks of a skip run, leaving what the run's reconstruction may read (mbType, mbPred). */
static __thread u32 vjp_seed = VJ_POISON;
static u32 vjp_rnd(void)
{
    vjp_seed = vjp_seed * 1664525u + 1013904223u;
    return vjp_seed >> 8;
}
static void vjPoisonX(vjSlot_t *s, u32 keepPred)
{
    macroblockLayer_t *m = &s->layer;
    u32 i, j;

    if (!keepPred)
    {
        m->mbType = (mbType_e)(vjp_rnd() % 32);
        for (i = 0; i < 16; i++)
        {
            m->mbPred.prevIntra4x4PredModeFlag[i] = vjp_rnd() & 1;
            m->mbPred.remIntra4x4PredMode[i] = vjp_rnd() & 7;
        }
        m->mbPred.intraChromaPredMode = vjp_rnd() & 3;
        for (i = 0; i < 4; i++)
        {
            m->mbPred.refIdxL0[i] = vjp_rnd() & 1;
            m->mbPred.mvdL0[i].hor = (i16)((i32)(vjp_rnd() % 9) - 4);
            m->mbPred.mvdL0[i].ver = (i16)((i32)(vjp_rnd() % 9) - 4);
        }
    }
    m->codedBlockPattern = vjp_rnd() % 48;
    m->mbQpDelta = (i32)(vjp_rnd() % 7) - 3;
    for (i = 0; i < 4; i++)
    {
        m->subMbPred.subMbType[i] = (subMbType_e)(vjp_rnd() & 3);
        m->subMbPred.refIdxL0[i] = vjp_rnd() & 1;
        for (j = 0; j < 4; j++)
        {
            m->subMbPred.mvdL0[i][j].hor = (i16)((i32)(vjp_rnd() % 9) - 4);
            m->subMbPred.mvdL0[i][j].ver = (i16)((i32)(vjp_rnd() % 9) - 4);
        }
    }
    for (i = 0; i < 27; i++)
        m->residual.totalCoeff[i] = (i16)(vjp_rnd() % 17);
    for (i = 0; i < 26; i++)
        for (j = 0; j < 16; j++)
            m->residual.level[i][j] = (i32)(vjp_rnd() % 5) - 2;
    for (i = 0; i < 24; i++)
        m->residual.coeffMap[i] = vjp_rnd() & 0xFFFF;
    if (!keepPred)
    {
        s->kind = vjp_rnd() % 3;
        s->count = vjp_rnd();
        s->status = vjp_rnd();
        s->last = vjp_rnd() & 1;
    }
}
#define vjPoison(s) vjPoisonX((s), 0)
#define vjPoisonSkip(s) vjPoisonX((s), 1)
#endif

void vjParseBegin(vjParse_t *p, storage_t *pStorage, const strmData_t *pStrm,
    const sliceHeader_t *pSliceHeader)
{
    u32 w;

    w = pStorage->activeSps->picWidthInMbs;
    p->strm = *pStrm;
    p->sliceType = pSliceHeader->sliceType;
    p->numRefIdxActive = pSliceHeader->numRefIdxL0Active;
    p->picSizeInMbs = pStorage->picSizeInMbs;
    p->width = w;
    p->addr = pSliceHeader->firstMbInSlice;
    p->prevSkipped = HANTRO_FALSE;
    p->fin = HANTRO_FALSE;
    p->singleGroup = pStorage->activePps->numSliceGroups == 1;
    p->mb = pStorage->mb;
    p->sliceGroupMap = pStorage->sliceGroupMap;
    p->sliceId = pStorage->slice->sliceId;
    p->ring = pStorage->vjRing;
#ifdef VJ_PARSE_PIPELINE
    {
        u32 i;

        p->useStorage = !p->singleGroup;
#ifdef VJ_NBR_STORAGE /* test: the slice-group path (neighbours from the storage) for every slice */
        p->useStorage = HANTRO_TRUE;
#endif
        p->nbr = pStorage->vjNbr;
        /* atomic: the worker's lock-free hint loads of head / tail may still run */
        __atomic_store_n(&p->head, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&p->tail, 0, __ATOMIC_RELAXED);
        p->mode = VJ_MODE_INLINE;
        p->col = p->addr % w;
        p->idx = p->addr % (w + 1);
        for (i = 0; i <= w; i++)
            p->nbr[i].st = VJ_NBR_NONE;
    }
#endif
}

#ifdef VJ_PARSE_PIPELINE
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
#endif

/* the next macroblock address of the slice group, 0 at its end */
static u32 ParseNextAddr(const vjParse_t *p, u32 addr)
{
    if (!p->singleGroup)
        return h264bsdNextMbAddress(p->sliceGroupMap, p->picSizeInMbs, addr);
    return (addr + 1 == p->picSizeInMbs) ? 0 : addr + 1;
}

void vjParseNext(vjParse_t *p, vjSlot_t *s)
{
    u32 tmp, skipRun, i;
    const vjTc_t *tcA, *tcB;
    macroblockLayer_t *mbLayer = &s->layer;

    ASSERT(!p->fin);

#ifdef VJ_POISON
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
                if (p->singleGroup)
                {
                    /* the run (skipRun <= picSize - addr, checked above) ends at addr + skipRun, or at the picture's
                     * end, where the next address is 0 */
#ifdef VJ_PARSE_PIPELINE
                    if (!p->useStorage)
                    {
                        /* every skipped macroblock is "available, totalCoeff 0" to its neighbours, but only the last
                         * width + 1 of them can be read again (nbr is a ring of that size): set those, move on */
                        u32 w1 = p->width + 1;
                        u32 m = skipRun < w1 ? skipRun : w1;
                        u32 j = (p->idx + skipRun - m) % w1;

                        for (i = 0; i < m; i++)
                        {
                            p->nbr[j].st = VJ_NBR_ZERO;
                            if (++j == w1)
                                j = 0;
                        }
                        p->idx = (p->idx + skipRun) % w1;
                        p->col = (p->col + skipRun) % p->width;
                    }
#endif
                    p->addr += skipRun;
                    if (p->addr == p->picSizeInMbs)
                        p->addr = 0;
                }
                else
                {
                    /* slice groups: the run follows the group's own order, and may leave it (the loop ends there) */
                    for (i = skipRun; i--;)
                    {
                        p->addr = ParseNextAddr(p, p->addr);
                        if (!p->addr)
                            break;
                    }
                }
                if (s->last || !p->addr)
                    p->fin = HANTRO_TRUE;
                return;
            }
        }
    }

    p->prevSkipped = HANTRO_FALSE;

#ifdef VJ_PARSE_PIPELINE
    if (p->useStorage)
    {
        mbStorage_t *pMb = p->mb + p->addr;
        mbStorage_t *mbA = pMb->mbA, *mbB = pMb->mbB;

        tcA = tcB = NULL;
        if (mbA && mbA->sliceId == p->sliceId)
        {
            VJ_COPY_TC(p->tmpA, mbA->totalCoeff);
            tcA = p->tmpA;
        }
        if (mbB && mbB->sliceId == p->sliceId)
        {
            VJ_COPY_TC(p->tmpB, mbB->totalCoeff);
            tcB = p->tmpB;
        }
    }
    else
    {
        tcA = p->col ? NbrTc(p->nbr + (p->idx ? p->idx - 1 : p->width)) : NULL;
        tcB = NbrTc(p->nbr + (p->idx == p->width ? 0 : p->idx + 1));
    }
#else
    {
        /* inline parse: the macroblocks before this one are reconstructed, their totalCoeff is in the storage */
        mbStorage_t *pMb = p->mb + p->addr;

        tcA = (pMb->mbA && pMb->mbA->sliceId == p->sliceId) ?
            pMb->mbA->totalCoeff : NULL;
        tcB = (pMb->mbB && pMb->mbB->sliceId == p->sliceId) ?
            pMb->mbB->totalCoeff : NULL;
    }
#endif

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
#ifdef VJ_PARSE_PIPELINE
    if (!p->useStorage)
    {
        vjNbr_t *n = p->nbr + p->idx;

        if (mbLayer->mbType == I_PCM)
            n->st = VJ_NBR_PCM;
        else if (!mbLayer->codedBlockPattern &&
                 h264bsdMbPartPredMode(mbLayer->mbType) != PRED_MODE_INTRA16x16)
            n->st = VJ_NBR_ZERO; /* no residual: the lazy clear left totalCoeff 0 */
        else
        {
            n->st = VJ_NBR_TC;
            VJ_COPY_TC(n->tc, mbLayer->residual.totalCoeff);
        }
        if (++p->idx > p->width)
            p->idx = 0;
        if (++p->col == p->width)
            p->col = 0;
    }
#endif
    p->addr = ParseNextAddr(p, p->addr);
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

   5.0b Parse pipeline: ring protocol (VJ_PARSE_PIPELINE)

        The deblock worker's task also runs the parser. Ownership of the
        parse context passes by pstate: IDLE (the decoding task's) -> ARMED
        (the worker's; release store after the context is set up) -> IDLE
        (the decoding task, by compare-and-swap from ARMED or DONE, spinning
        while the worker is PARSING one record). vjParseStep is the worker's
        side: one record per call, into slot head % depth, then head (release).
        The decoding task polls head (acquire), uses the slot in place, and
        frees it with tail (release). The parser reads no storage the
        decoding task writes, so records are a pure function of the stream:
        the schedule cannot change the output. If the worker has not
        produced a record for 50 ms (not scheduled), the decoding task takes
        the context back and parses itself, after the records already made,
        and parses the next VJ_TAKEBACK_HOLDOFF slices inline before it asks
        the worker again (a dead worker costs 50 ms once, not once per slice).

        Sleeping: a worker that finds the ring full for about 200 us calls
        the step with stalled = 1, which sets p->sleeping, issues a full
        fence and re-reads tail: a slot freed meanwhile cancels the sleep,
        otherwise the worker blocks. The decoding task frees a slot (tail,
        release), issues a full fence and reads p->sleeping: a set flag is
        cleared and wakes the worker (a notify; none in the normal case).
        Each side's store
        precedes its load of the other's variable, with a full fence between,
        so one of them sees the other: the worker never sleeps with a free
        slot unseen.

------------------------------------------------------------------------------*/

#define VJ_TAKEBACK_TICKS (50u * VJ_TICKS_PER_MS)
/* slices parsed inline after a take-back, before the worker is asked again */
#define VJ_TAKEBACK_HOLDOFF 30
/* A wait that has lasted this long is no longer a spin: the worker may share the waiting task's core (an unpinned
 * decoding task that migrated, a priority change), so the waiter sleeps a tick and lets it run (vj_yield_wait) */
#define VJ_SPIN_TICKS (500u * VJ_TICKS_PER_MS / 1000u)

#ifndef ESP_PLATFORM
/* host tests: slices seen, slices handed to the worker, slices whose parser the decoding task took back */
unsigned vjParseTestSlices, vjParseTestArmed, vjParseTestTakebacks;
#endif

#ifdef VJ_PIPE_JITTER_US
static unsigned vjJitter(void)
{
    static __thread unsigned x = 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x % VJ_PIPE_JITTER_US;
}
#define VJ_JITTER() do { if (vjJitter() < VJ_PIPE_JITTER_US / 2) usleep(vjJitter()); } while (0)
#else
#define VJ_JITTER() ((void)0)
#endif

#ifdef VJ_PARSE_PIPELINE
#include "vj_worker.h"
#ifdef VJ_PIPE_JITTER_US
#include <unistd.h>
#endif

/* the worker's side. 1: made a record. 2: armed but the ring is full. 3: (stalled only) the ring is full, the sleeping
 * flag is set, sleep until woken. 0: not armed */
int vjParseStep(void *arg, int stalled)
{
    vjParse_t *p = (vjParse_t *)arg;
    u32 h, t, exp = VJ_PS_ARMED;

#ifdef VJ_PIPE_NOSTEP /* test: a worker task that never runs the parser (starvation: the decoding task takes it back) */
    return 0;
#endif
    if (__atomic_load_n(&p->pstate, __ATOMIC_RELAXED) != VJ_PS_ARMED)
        return 0;
    h = __atomic_load_n(&p->head, __ATOMIC_RELAXED);
    t = __atomic_load_n(&p->tail, __ATOMIC_ACQUIRE);
    if (h - t >= VJ_RING_DEPTH)
    {
        if (!stalled)
            return 2;
        /* announce the sleep, then look again: the decoding task stores tail, fences, then reads the flag */
        VJ_JITTER(); /* test: widens the window between the decision and the announcement */
        __atomic_store_n(&p->sleeping, 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        t = __atomic_load_n(&p->tail, __ATOMIC_ACQUIRE);
        if (h - t >= VJ_RING_DEPTH)
            return 3;
        __atomic_store_n(&p->sleeping, 0, __ATOMIC_RELAXED);
    }
    if (!__atomic_compare_exchange_n(&p->pstate, &exp, VJ_PS_PARSING, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    /* the context may have been taken back and armed for another slice since the loads above */
    h = __atomic_load_n(&p->head, __ATOMIC_RELAXED);
    t = __atomic_load_n(&p->tail, __ATOMIC_ACQUIRE);
    if (h - t >= VJ_RING_DEPTH)
    {
        __atomic_store_n(&p->pstate, VJ_PS_ARMED, __ATOMIC_RELEASE);
        return 2;
    }
    VJ_JITTER();
    vjParseNext(p, p->ring + h % VJ_RING_DEPTH);
    __atomic_store_n(&p->head, h + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&p->pstate, p->fin ? VJ_PS_DONE : VJ_PS_ARMED,
        __ATOMIC_RELEASE);
    return 1;
}

/* the decoding task owns the context again; no worker step is in flight. A step is one record (tens of us), so
 * the wait for it is a spin; past VJ_SPIN_TICKS the worker was preempted inside the step (it shares our core and
 * has a lower priority, or a task of its core's preempted it): sleep, never spin it out of the CPU. It always ends:
 * the worker only needs to be scheduled for the rest of one record. */
static void ParseDisarm(vjParse_t *p)
{
    u32 t0 = 0, spins = 0;

    for (;;)
    {
        u32 st = __atomic_load_n(&p->pstate, __ATOMIC_ACQUIRE);

        if (st == VJ_PS_IDLE)
            return;
        if (st == VJ_PS_PARSING)
        {
            if (!(++spins & 63))
            {
                u32 now = vj_ticks();

                if (!t0)
                    t0 = now;
                else if (now - t0 > VJ_SPIN_TICKS)
                    vj_yield_wait();
            }
            vj_relax();
            continue;
        }
        if (__atomic_compare_exchange_n(&p->pstate, &st, VJ_PS_IDLE, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return;
    }
}

static void ParseArm(vjParse_t *p, dbw_t *dbw)
{
    p->mode = VJ_MODE_PIPE;
    p->dbw = dbw;
    __atomic_store_n(&p->sleeping, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&p->pstate, VJ_PS_ARMED, __ATOMIC_RELEASE);
    h264bsdDbwKick(dbw);
}
#endif /* VJ_PARSE_PIPELINE */

/* The next record, in the order the slice has them. The slot stays the caller's until ParseConsume. */
static vjSlot_t *ParseFetch(vjParse_t *p)
{
#ifndef VJ_PARSE_PIPELINE
    /* no pipeline: one slot, parsed here, used, and parsed over by the next macroblock */
    vjParseNext(p, p->ring);
    return p->ring;
#else
    u32 t = __atomic_load_n(&p->tail, __ATOMIC_RELAXED);

    if (p->mode == VJ_MODE_PIPE)
    {
        u32 spins = 0, t0 = 0, started = 0;

        for (;;)
        {
            VJ_JITTER();
            if (__atomic_load_n(&p->head, __ATOMIC_ACQUIRE) != t)
                return p->ring + t % VJ_RING_DEPTH;
            if (!(++spins & 63))
            {
                u32 now = vj_ticks();

                if (!started)
                {
                    started = 1;
                    t0 = now;
                }
                else if (now - t0 > VJ_TAKEBACK_TICKS)
                {
                    /* no record for 50 ms: the worker is not running (starved, or never scheduled). Take the context
                     * back and parse here, after the records already made */
                    ParseDisarm(p);
                    p->mode = VJ_MODE_INLINE;
                    p->holdoff = VJ_TAKEBACK_HOLDOFF;
#ifndef ESP_PLATFORM
                    __atomic_fetch_add(&vjParseTestTakebacks, 1, __ATOMIC_RELAXED);
#endif
                    vj_worker_warn("parse pipeline: the worker task did not run; the decoding task parsed");
                    break;
                }
                else if (now - t0 > VJ_SPIN_TICKS)
                    vj_yield_wait();
            }
            vj_relax();
        }
    }
    /* inline (no worker, or taken back): the records the worker made first, then parse here */
    if (__atomic_load_n(&p->head, __ATOMIC_RELAXED) == t)
    {
        vjParseNext(p, p->ring + t % VJ_RING_DEPTH);
        __atomic_store_n(&p->head, t + 1, __ATOMIC_RELAXED);
    }
    return p->ring + t % VJ_RING_DEPTH;
#endif
}

static void ParseConsume(vjParse_t *p)
{
#ifdef VJ_PARSE_PIPELINE
    VJ_JITTER(); /* test: widens the windows around the slot being freed */
    __atomic_store_n(&p->tail, __atomic_load_n(&p->tail, __ATOMIC_RELAXED) + 1,
        __ATOMIC_RELEASE);
    if (p->mode == VJ_MODE_PIPE)
    {
        VJ_JITTER();
        /* a worker that announced a sleep on the full ring is woken, once (the flag is clear in the normal case) */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&p->sleeping, __ATOMIC_RELAXED))
        {
            __atomic_store_n(&p->sleeping, 0, __ATOMIC_RELAXED);
            h264bsdDbwWake(p->dbw);
        }
    }
#else
    (void)p;
#endif
}

/* the slice is over (any way): the worker has no step in flight, the context is the caller's */
static void ParseEnd(vjParse_t *p)
{
#ifdef VJ_PARSE_PIPELINE
    ParseDisarm(p);
    p->mode = VJ_MODE_INLINE;
#else
    (void)p;
#endif
}

/*------------------------------------------------------------------------------

   5.0c Function name: DecodeSkipRun

        Functional description:
            The skip-run path (VideoJet). Decodes the skipped macroblocks
            addr, addr + 1, ... (at most n) of a skip run for as long as the
            P_Skip rule gives motion vector 0 without a median: the left or
            the upper neighbour is unavailable (another slice, the picture
            edge) or is a zero-MV skip of this slice (vjZero). Each gets
            exactly what the generic loop gives it: the lazy decoded clear and
            SetMbParams, h264bsdDecodeMacroblock for P_Skip (mv 0, refIdx 0,
            reference ref, then the skip elision or the direct copy), the
            deblock record and its row posting. totalCoeff, refPic and mv are
            written only when vjZero says they do not hold those zeros
            already, so a macroblock that was such a skip in the picture
            before costs one cache line. Stops at the first macroblock that
            does not qualify, or that the generic loop would fail on (decoded
            already), and leaves it to the generic path.
            The caller runs the rest of the loop. n > 1: the MBs of a run
            before its last, for which the rest does nothing (no record is
            fetched or consumed, and the next address is addr + 1, inside the
            picture: the parser checked that the run fits). n = 1: one MB
            inside the generic iteration, after its head (SetMbParams again
            writes the same values) and before its tail. The caller checks:
            one slice group, a primary slice, ref is
            h264bsdGetRefPicData(dpb, 0) and not NULL.

        Returns:
            the number of macroblocks decoded

------------------------------------------------------------------------------*/

static u32 DecodeSkipRun(storage_t *pStorage, image_t *currImage,
    const sliceHeader_t *pSliceHeader, u8 *ref, u32 addr, u32 n, u32 qpY)
{
    dpbStorage_t *dpb = pStorage->dpb;
    u32 sliceId = pStorage->slice->sliceId;
    u32 base = pStorage->picSliceBase;
    u32 refIdx = dpb->list[0]->vjFrameIdx;
    u32 curIdx = dpb->currentOut->vjFrameIdx;
    u32 idc = pSliceHeader->disableDeblockingFilterIdc;
    i32 offA = pSliceHeader->sliceAlphaC0Offset;
    i32 offB = pSliceHeader->sliceBetaOffset;
    i32 chromaQpOff = pStorage->activePps->chromaQpIndexOffset;
    mbStorage_t *mb = pStorage->mb + addr;
    u32 i;

    for (i = 0; i < n; i++, addr++, mb++)
    {
        mbStorage_t *a = mb->mbA, *b = mb->mbB;
        u32 last;

        /* touched in this picture and decoded: the generic loop fails */
        if (mb->sliceId >= base && mb->decoded)
            break;
        /* a neighbour available and a zero-MV skip, or one unavailable */
        if (a && a->sliceId == sliceId && !a->vjZero &&
            b && b->sliceId == sliceId && !b->vjZero)
            break;
        ASSERT(!mb->vjZero || h264bsdZeroSkipFields(mb));
        ASSERT(!a || a->sliceId != sliceId || !a->vjZero ||
            (a->mbType == P_Skip && h264bsdZeroSkipFields(a)));
        ASSERT(!b || b->sliceId != sliceId || !b->vjZero ||
            (b->mbType == P_Skip && h264bsdZeroSkipFields(b)));

        /* SetMbParams (mirrors it) */
        mb->sliceId = sliceId;
        mb->disableDeblockingFilterIdc = (u8)idc;
        mb->filterOffsetA = (i8)offA;
        mb->filterOffsetB = (i8)offB;
        mb->chromaQpIndexOffset = (i8)chromaQpOff;
        /* mirrors h264bsdDecodeMacroblock's P_Skip path and the stores of
         * MvPrediction16x16; decoded was 0 (cleared lazily or not) */
        mb->mbType = P_Skip;
        mb->decoded = 1;
        mb->qpY = (u8)qpY;
        mb->refAddr[0] = mb->refAddr[1] = mb->refAddr[2] = mb->refAddr[3] =
            ref;
        if (!mb->vjZero)
        {
            memset(mb->totalCoeff, 0, sizeof(mb->totalCoeff));
            memset(mb->refPic, 0, sizeof(mb->refPic));
            memset(mb->mv, 0, sizeof(mb->mv));
            mb->vjZero = 1;
        }

        /* mirrors h264bsdInterPrediction's skip elision, else the direct
         * copy */
        last = dpb->vjLastChanged[addr];
        if (last <= refIdx)
            dpb->vjChanged[addr] = 0;
        if (last > refIdx || last > curIdx)
        {
            u32 width = currImage->width;

            h264bsdSetCurrImageMbPointers(currImage, addr);
            h264bsdCopyMb(currImage, ref, addr % width * 16,
                addr / width * 16);
        }

        /* mirrors the generic loop's deblock record and row posting */
        if (addr == pStorage->vjRecNext)
        {
            h264bsdDeblockRecordSkip(mb, pStorage->vjRec + addr, base);
            pStorage->vjRecNext++;
#ifdef VJ_DEBLOCK_WORKER
            if (pStorage->vjRecNext == pStorage->vjDbwRowEnd)
                pStorage->vjDbwRowEnd = h264bsdDbwRowEnd(pStorage->vjDbw);
#endif
        }
        else
            pStorage->vjRecNext = VJ_REC_OFF;
    }

    /* the image's macroblock pointers end where the generic loop leaves them */
    if (i)
        h264bsdSetCurrImageMbPointers(currImage, addr - 1);
    return i;
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

static u32 DecodeSliceLoop(strmData_t *pStrmData, storage_t *pStorage,
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
    u8 *skipRef;
    u32 fast, runStop;

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
#ifdef VJ_PARSE_PIPELINE
    /* the worker task parses ahead when it exists and runs on another core, with one slice group and a primary slice.
     * Otherwise the same records are made here, one at a time. No size gate: handing over every slice measured best,
     * even the near-empty P slices of a static card (aux_card; see docs/bench-log.md). */
#ifndef ESP_PLATFORM
    vjParseTestSlices++;
#endif
    if (pStorage->vjDbw && !parse->useStorage &&
        pStorage->activePps->numSliceGroups == 1 &&
        !pSliceHeader->redundantPicCnt && h264bsdDbwParallel(pStorage->vjDbw))
    {
        /* after a take-back the worker is not asked again for a while: a dead one costs 50 ms once */
        if (parse->holdoff)
            parse->holdoff--;
        else
        {
#ifndef ESP_PLATFORM
            vjParseTestArmed++;
#endif
            ParseArm(parse, pStorage->vjDbw);
        }
    }
#endif

    /* lastMbAddr stores address of the macroblock that was last successfully
     * decoded, needed for error handling */
    pStorage->slice->lastMbAddr = 0;

    mbCount = 0;
    /* initial quantization parameter for the slice is obtained as the sum of
     * initial QP for the picture and sliceQpDelta for the current slice */
    qpY = (i32)pStorage->activePps->picInitQp + pSliceHeader->sliceQpDelta;
    /* the skip-run path's slices: P, one slice group, primary, reference 0
     * present (without it every skipped MB fails in the generic path) */
    skipRef = (IS_P_SLICE(pSliceHeader->sliceType) &&
        pStorage->activePps->numSliceGroups == 1 &&
        !pSliceHeader->redundantPicCnt) ?
        h264bsdGetRefPicData(pStorage->dpb, 0) : NULL;
    do
    {
        /* inside a skip run, before its last MB: as many MBs as the skip-run
         * path takes, then this iteration does the next one. If it stopped
         * before the run's last MB, that one does not qualify: the one-MB
         * try below would fail the same way */
        runStop = 0;
        if (skipLeft > 1 && skipRef)
        {
            u32 n;

            { VJP_BEGIN(VJP_RECON);
            n = DecodeSkipRun(pStorage, currImage, pSliceHeader, skipRef,
                currMbAddr, skipLeft - 1, (u32)qpY);
            VJP_END(VJP_RECON); }
            runStop = n < skipLeft - 1;
            currMbAddr += n;
            skipLeft -= n;
            mbCount += n;
        }

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
            slot = ParseFetch(parse);
            mbLayer = &slot->layer;
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

        /* a skipped MB (the first or the last of its run, or one the run
         * path left) goes to the skip-run path alone if its motion vector is
         * 0 there; it makes the deblock record too */
        { VJP_BEGIN(VJP_RECON);
        fast = (slot->kind == VJ_REC_SKIPRUN && skipRef && !runStop) ?
            DecodeSkipRun(pStorage, currImage, pSliceHeader, skipRef,
                currMbAddr, 1, (u32)qpY) : 0;
        tmp = fast ? HANTRO_OK :
            h264bsdDecodeMacroblock(pStorage->mb + currMbAddr, mbLayer,
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
         * out of that order turns the picture's records off. Mirrored in
         * DecodeSkipRun: change both */
        if (fast)
        {
            /* made by DecodeSkipRun */
        }
        else if (currMbAddr == pStorage->vjRecNext)
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
        if (!skipLeft)
            ParseConsume(parse);
#ifdef VJ_PIPE_STALL_EVERY /* test hook: the decoding task stalls now and then, so the ring stays full and the worker sleeps */
        {
            static unsigned vjStallN;

            if (++vjStallN % VJ_PIPE_STALL_EVERY == 0)
                vj_stall_us(1000);
        }
#endif

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

u32 h264bsdDecodeSliceData(strmData_t *pStrmData, storage_t *pStorage,
    image_t *currImage, sliceHeader_t *pSliceHeader)
{
    u32 r = DecodeSliceLoop(pStrmData, pStorage, currImage, pSliceHeader);

    /* every way out: nothing of the slice is in flight on the worker */
    ParseEnd(pStorage->vjParse);
    return r;
}

/*------------------------------------------------------------------------------

   5.2  Function: SetMbParams

        Functional description:
            Set macroblock parameters that remain constant for this slice.
            Mirrored in DecodeSkipRun (VideoJet): change both.

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

