/* test_nal: h264bsdExtractNalUnit (h264bsd_byte_stream.c, word-at-a-time) against the original byte-at-a-time version
 * below. Usage: test_nal [RANDOM_BUFFERS] (default 10,000,000; 1,000,000 when built with a sanitizer).
 *
 * Everything the function can show a caller must be identical:
 *   - the returned status and *readBytes;
 *   - every field of strmData_t (the pointer as an offset), cache[] too: it holds the window on the extracted bytes;
 *   - every byte of the buffer afterwards. The function works in place, so that is the extracted bytes, the stale tail
 *     past them, and what an error leaves behind (the bytes before the offending one are moved down already).
 * The struct and *readBytes are poisoned first, so "not written" is compared as well.
 * Each case runs at all four alignments of the buffer start (0..3) on a buffer that ends exactly at the end of its
 * allocation, so ASan/valgrind flag any read or write outside [buf, buf + len).
 *
 * Inputs:
 *   1. exhaustive: every buffer of length 1..8 over {00 01 02 03 04 FF}; length 9..10 over {00 01 03 04};
 *      length 11..13 over {00 01 03} (and length 0, once);
 *   2. RANDOM_BUFFERS random buffers of length 1..300 from six generators (biased to 00 and 03, NAL-like with start codes,
 *      emulation bytes and trailing zeros, sparse zeros, runs of zeros, plain random, mutated NAL-like), many with
 *      the start code at the front so the byte stream branch runs;
 *   3. 4000 long buffers (1000..70000 bytes) from the same generators.
 * Prints the case count and how many cases ended each way; exit 0 when every case matches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "h264bsd_byte_stream.h"
#include "h264bsd_util.h"

/* The original: h264bsdExtractNalUnit from h264bsd_byte_stream.c at 4f9a759, before the word-at-a-time rewrite; only the
 * name carries the Old suffix. */
/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 * Modified for VideoJet: see h264bsd/README.md, Local changes. Copied from
 * h264bsd_byte_stream.c at 4f9a759 into host/test_nal.c as its oracle, name suffixed.
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
#define BYTE_STREAM_ERROR  0xFFFFFFFF

u32 h264bsdExtractNalUnitOld(u8 *pByteStream, u32 len, strmData_t *pStrmData,
    u32 *readBytes)
{

/* Variables */

    u32 i, tmp;
    u32 byteCount,initByteCount;
    u32 zeroCount;
    u8  byte;
    u32 hasEmulation = HANTRO_FALSE;
    u32 invalidStream = HANTRO_FALSE;
    u8 *readPtr, *writePtr;

/* Code */

    ASSERT(pByteStream);
    ASSERT(len);
    ASSERT(len < BYTE_STREAM_ERROR);
    ASSERT(pStrmData);

    /* byte stream format if starts with 0x000001 or 0x000000 */
    if (len > 3 && pByteStream[0] == 0x00 && pByteStream[1] == 0x00 &&
        (pByteStream[2]&0xFE) == 0x00)
    {
        /* search for NAL unit start point, i.e. point after first start code
         * prefix in the stream */
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
         * or end of stream and ignore possible trailing zero bytes */
        zeroCount = 0;
        /*lint -e(716) while(1) used consciously */
        while (1)
        {
            byte = *readPtr++;
            byteCount++;
            if (!byte)
                zeroCount++;

            if ( (byte == 0x03) && (zeroCount == 2) )
            {
                hasEmulation = HANTRO_TRUE;
            }

            if ( (byte == 0x01) && (zeroCount >= 2 ) )
            {
                pStrmData->strmBuffSize =
                    byteCount - initByteCount - zeroCount - 1;
                zeroCount -= MIN(zeroCount, 3);
                break;
            }
            else if (byte)
            {
                if (zeroCount >= 3)
                    invalidStream = HANTRO_TRUE;
                zeroCount = 0;
            }

            if (byteCount == len)
            {
                pStrmData->strmBuffSize = byteCount - initByteCount - zeroCount;
                break;
            }

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
        tmp = pStrmData->strmBuffSize;
        readPtr = writePtr = pStrmData->pStrmBuffStart;
        zeroCount = 0;
        for (i = tmp; i--;)
        {
            if ((zeroCount == 2) && (*readPtr == 0x03))
            {
                /* emulation prevention byte shall be followed by one of the
                 * following bytes: 0x00, 0x01, 0x02, 0x03. This implies that
                 * emulation prevention 0x03 byte shall not be the last byte
                 * of the stream. */
                if ( (i == 0) || (*(readPtr+1) > 0x03) )
                    return(HANTRO_NOK);

                /* do not write emulation prevention byte */
                readPtr++;
                zeroCount = 0;
            }
            else
            {
                /* NAL unit shall not contain byte sequences 0x000000,
                 * 0x000001 or 0x000002 */
                if ( (zeroCount == 2) && (*readPtr <= 0x02) )
                    return(HANTRO_NOK);

                if (*readPtr == 0)
                    zeroCount++;
                else
                    zeroCount = 0;

                *writePtr++ = *readPtr++;
            }
        }

        /* (readPtr - writePtr) indicates number of "removed" emulation
         * prevention bytes -> subtract from stream buffer size */
        pStrmData->strmBuffSize -= (u32)(readPtr - writePtr);
    }

    h264bsdStrmFill(pStrmData);

    return(HANTRO_OK);

}


/* ---- the test ---- */

#define MAXLEN 300

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

/* exact-size allocations for len <= MAXLEN: the buffer starts al bytes in (al = alignment) and ends where the
 * allocation ends */
static u8 *bufA[MAXLEN + 1][4], *bufB[MAXLEN + 1][4];

static unsigned long cases, nOk, nOkMoved, nNokNoStart, nNokKept, nNokMoved;

static void hexdump(const char *tag, const u8 *p, u32 n)
{
    printf("  %s:", tag);
    for (u32 i = 0; i < n; i++)
        printf(" %02x", p[i]);
    printf("\n");
}

/* a strmData_t with the pointer turned into an offset into the buffer (or left as is when it is the poison) */
static strmData_t canon(const strmData_t *s, const u8 *base, u32 len)
{
    strmData_t c = *s;
    if (s->pStrmBuffStart >= base && s->pStrmBuffStart <= base + len)
        c.pStrmBuffStart = (u8 *)(uintptr_t)(s->pStrmBuffStart - base);
    return c;
}

/* old and new on the same input at alignment al; returns 0 on a match */
static int check(const u8 *src, u32 len, unsigned al, u8 *a, u8 *b)
{
    strmData_t sa, sb, ca, cb;
    u32 ra = 0xDEADBEEFu, rb = 0xDEADBEEFu, st_a, st_b;

    memcpy(a, src, len);
    memcpy(b, src, len);
    memset(&sa, 0xA5, sizeof sa);
    memset(&sb, 0xA5, sizeof sb);
    st_a = h264bsdExtractNalUnitOld(a, len, &sa, &ra);
    st_b = h264bsdExtractNalUnit(b, len, &sb, &rb);
    ca = canon(&sa, a, len);
    cb = canon(&sb, b, len);
    cases++;
    if (st_a != st_b || ra != rb || memcmp(&ca, &cb, sizeof ca) || memcmp(a, b, len))
    {
        printf("MISMATCH len %u alignment %u\n", len, al);
        hexdump("input", src, len);
        printf("  old: status %u readBytes %u size %u readBits %u cache %08x %08x start+%ld\n", st_a, ra, sa.strmBuffSize,
               sa.strmBuffReadBits, sa.cache[0], sa.cache[1], (long)(uintptr_t)ca.pStrmBuffStart);
        printf("  new: status %u readBytes %u size %u readBits %u cache %08x %08x start+%ld\n", st_b, rb, sb.strmBuffSize,
               sb.strmBuffReadBits, sb.cache[0], sb.cache[1], (long)(uintptr_t)cb.pStrmBuffStart);
        hexdump("old buf", a, len);
        hexdump("new buf", b, len);
        return 1;
    }
    int moved = memcmp(a, src, len) != 0;
    if (st_a == HANTRO_OK)
        moved ? nOkMoved++ : nOk++;
    else if (sa.strmBuffSize == 0xA5A5A5A5u) /* the struct was not written: no start code */
        nNokNoStart++;
    else
        moved ? nNokMoved++ : nNokKept++;
    return 0;
}

static int all_alignments(const u8 *src, u32 len)
{
    for (unsigned al = 0; al < 4; al++)
        if (check(src, len, al, bufA[len][al] + al, bufB[len][al] + al))
            return 1;
    return 0;
}

static int exhaustive(const u8 *alpha, unsigned n, unsigned lo, unsigned hi)
{
    u8 buf[16];
    for (unsigned len = lo; len <= hi; len++)
    {
        unsigned long total = 1;
        for (unsigned i = 0; i < len; i++)
            total *= n;
        for (unsigned long idx = 0; idx < total; idx++)
        {
            unsigned long v = idx;
            for (unsigned i = 0; i < len; i++, v /= n)
                buf[i] = alpha[v % n];
            if (all_alignments(buf, len))
                return 1;
        }
    }
    return 0;
}

/* ---- random buffers ---- */

static u8 nzbyte(void) /* 1..255 */
{
    return (u8)(1 + rnd() % 255);
}

static u8 anybyte(unsigned p0, unsigned p3, unsigned plow) /* 00 with p0 %, 03 with p3 %, 01/02 with plow % each */
{
    unsigned r = rnd() % 100;
    if (r < p0)
        return 0;
    r -= p0;
    if (r < p3)
        return 3;
    r -= p3;
    if (r < plow)
        return 1;
    r -= plow;
    if (r < plow)
        return 2;
    return (u8)rnd();
}

static unsigned put_start(u8 *b, unsigned at, unsigned max)
{
    static const u8 sc[8][6] = {{0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1}, {0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 1}};
    static const u8 sl[8] = {3, 4, 5, 6, 3, 4, 4, 3};
    unsigned k = rnd() % 8;
    for (unsigned i = 0; i < sl[k] && at < max; i++)
        b[at++] = sc[k][i];
    return at;
}

/* a pattern around zero runs */
static unsigned put_pattern(u8 *b, unsigned at, unsigned max)
{
    unsigned k = rnd() % 14;
    u8 t[8];
    unsigned n = 0;
    switch (k)
    {
    case 0: case 1: case 2: t[n++] = 0; t[n++] = 0; t[n++] = 3; t[n++] = (u8)(rnd() % 4); break;   /* emulation, valid */
    case 3: t[n++] = 0; t[n++] = 0; t[n++] = 3; t[n++] = (u8)(4 + rnd() % 252); break;             /* emulation, next > 3 */
    case 4: t[n++] = 0; t[n++] = 0; t[n++] = 3; break;                                             /* emulation, last? */
    case 5: t[n++] = 0; t[n++] = 0; t[n++] = 0; t[n++] = nzbyte(); break;
    case 6: t[n++] = 0; t[n++] = 0; t[n++] = 1; break;
    case 7: t[n++] = 0; t[n++] = 0; t[n++] = 2; break;
    case 8: t[n++] = 0; break;
    case 9: t[n++] = 0; t[n++] = 0; t[n++] = (u8)(4 + rnd() % 252); break;
    case 10: t[n++] = 0; t[n++] = 0; t[n++] = 0; t[n++] = 0; t[n++] = nzbyte(); break;
    case 11: t[n++] = 0; t[n++] = nzbyte(); break;
    case 12: t[n++] = 0; t[n++] = 0; t[n++] = 0; t[n++] = 3; break;
    default: t[n++] = 3; break;
    }
    for (unsigned i = 0; i < n && at < max; i++)
        b[at++] = t[i];
    return at;
}

/* fill b[0..len) from generator kind (len given); returns len */
static void gen(u8 *b, unsigned len, int kind)
{
    unsigned at = 0, k, p0, p3, pl;
    switch (kind)
    {
    case 0: /* small alphabet, random weights */
        p0 = rnd() % 70;
        p3 = rnd() % 30;
        pl = rnd() % 12;
        for (at = 0; at < len; at++)
            b[at] = anybyte(p0, p3, pl);
        break;
    case 1: /* NAL-like: start code, payload with patterns, trailing zeros / next start code */
    case 5:
        if (rnd() % 6)
            at = put_start(b, 0, len);
        else
            for (k = rnd() % 5; k && at < len; k--)
                b[at++] = (u8)rnd();
        while (at < len)
        {
            if (rnd() % 24 == 0)
                at = put_pattern(b, at, len);
            else
                b[at++] = nzbyte();
            if (rnd() % 200 == 0)
                at = put_start(b, at, len);
        }
        if (rnd() % 3 == 0)
            for (k = 1 + rnd() % 6, at = len; k-- && at > 0;)
                b[--at] = 0;   /* trailing zeros */
        if (kind == 5)
            for (k = 1 + rnd() % 3; k--;)
                b[rnd() % len] = (u8)(rnd() % 3 == 0 ? rnd() : rnd() % 5);
        break;
    case 2: /* sparse zeros */
        p0 = 1 + rnd() % 8; /* 1..8 % */
        if (rnd() % 5)
            at = put_start(b, 0, len);
        for (; at < len; at++)
            b[at] = (rnd() % 100 < p0) ? 0 : nzbyte();
        break;
    case 3: /* runs of zeros between runs of nonzero bytes, terminated by 1, 2, 3 or more */
        if (rnd() % 5)
            at = put_start(b, 0, len);
        while (at < len)
        {
            for (k = rnd() % 14; k && at < len; k--)
                b[at++] = nzbyte();
            for (k = rnd() % 6; k && at < len; k--)
                b[at++] = 0;
            if (at < len)
                b[at++] = (u8)(rnd() % 3 == 0 ? 1 + rnd() % 4 : nzbyte());
        }
        break;
    default: /* plain random, mostly with a start code */
        if (rnd() % 5)
            at = put_start(b, 0, len);
        for (; at < len; at++)
            b[at] = (u8)rnd();
        break;
    }
}

static int random_buffers(unsigned long n)
{
    u8 src[MAXLEN];
    for (unsigned long i = 0; i < n; i++)
    {
        unsigned len = 1 + rnd() % (rnd() % 4 == 0 ? MAXLEN : 40);
        gen(src, len, rnd() % 6);
        if (all_alignments(src, len))
            return 1;
    }
    return 0;
}

static int long_buffers(unsigned long n)
{
    u8 *src = malloc(70000);
    for (unsigned long i = 0; i < n; i++)
    {
        unsigned len = 1000 + rnd() % 69001;
        gen(src, len, rnd() % 6);
        for (unsigned al = 0; al < 4; al++)
        {
            u8 *a = malloc(al + len), *b = malloc(al + len);
            int bad = check(src, len, al, a + al, b + al);
            free(a);
            free(b);
            if (bad)
                return 1;
        }
    }
    free(src);
    return 0;
}

/* a few inputs with the outcome known from the standard's rules: old and new must both give it */
static int named(void)
{
    static const struct
    {
        const char *what;
        u8 in[16];
        u32 len, status, readBytes, size;
        u8 out[8];
    } t[] = {
        {"3-byte start code, 2 bytes", {0, 0, 1, 0x67, 0x42}, 5, HANTRO_OK, 5, 2, {0x67, 0x42}},
        {"4-byte start code, trailing zeros dropped", {0, 0, 0, 1, 0x67, 0x42, 0, 0}, 8, HANTRO_OK, 8, 2, {0x67, 0x42}},
        {"next start code ends the NAL (4-byte prefix kept for the next call)", {0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68}, 10, HANTRO_OK, 5, 2, {0x67, 0x42}},
        {"5 zeros before the next start code: 2 are skipped, 3 + 01 stay", {0, 0, 1, 0x67, 0, 0, 0, 0, 0, 1, 0x68}, 11, HANTRO_OK, 6, 1, {0x67}},
        {"emulation byte removed", {0, 0, 1, 0x65, 0, 0, 3, 1, 0x80}, 9, HANTRO_OK, 9, 5, {0x65, 0, 0, 1, 0x80}},
        {"no start code before the end", {0, 0, 0, 1}, 4, HANTRO_NOK, 4, 0, {0}},
        {"emulation byte last", {0, 0, 1, 0x65, 0, 0, 3}, 7, HANTRO_NOK, 7, 0, {0}},
        {"emulation byte then > 3", {0, 0, 1, 0x65, 0, 0, 3, 4}, 8, HANTRO_NOK, 8, 0, {0}},
        {"000000 inside a NAL", {0, 0, 1, 0x65, 0, 0, 0, 5}, 8, HANTRO_NOK, 8, 0, {0}},
        {"raw NAL (no start code), emulation removed", {0x65, 0, 0, 3, 0, 0x80}, 6, HANTRO_OK, 6, 5, {0x65, 0, 0, 0, 0x80}},
        {"raw NAL keeps trailing zeros", {0x65, 0x80, 0, 0}, 4, HANTRO_OK, 4, 4, {0x65, 0x80, 0, 0}},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
    {
        for (int which = 0; which < 2; which++)
        {
            u8 buf[16];
            strmData_t s;
            u32 rb = 0, st;
            memcpy(buf, t[i].in, t[i].len);
            st = which ? h264bsdExtractNalUnit(buf, t[i].len, &s, &rb) : h264bsdExtractNalUnitOld(buf, t[i].len, &s, &rb);
            if (st != t[i].status || rb != t[i].readBytes ||
                (st == HANTRO_OK && (s.strmBuffSize != t[i].size || memcmp(s.pStrmBuffStart, t[i].out, t[i].size))))
            {
                printf("NAMED FAIL (%s): %s: status %u readBytes %u size %u\n", which ? "new" : "old", t[i].what, st, rb,
                       s.strmBuffSize);
                return 1;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    unsigned long nrandom = argc > 1 ? strtoul(argv[1], 0, 10) : 1000000UL;
#else
    unsigned long nrandom = argc > 1 ? strtoul(argv[1], 0, 10) : 10000000UL;
#endif
    static const u8 a6[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0xFF}, a4[4] = {0x00, 0x01, 0x03, 0x04}, a3[3] = {0x00, 0x01, 0x03};
    u8 empty[1] = {0};

    for (unsigned len = 0; len <= MAXLEN; len++)
        for (unsigned al = 0; al < 4; al++)
        {
            bufA[len][al] = malloc(al + len + (len == 0));
            bufB[len][al] = malloc(al + len + (len == 0));
        }

    if (named())
        return 1;
    if (all_alignments(empty, 0)) /* len 0: not a legal call (ASSERT(len)), but identical */
        return 1;
    if (exhaustive(a6, 6, 1, 8) || exhaustive(a4, 4, 9, 10) || exhaustive(a3, 3, 11, 13))
        return 1;
    printf("exhaustive: %lu cases\n", cases);
    unsigned long ex = cases;
    if (random_buffers(nrandom))
        return 1;
    printf("random: %lu buffers x 4 alignments = %lu cases\n", nrandom, cases - ex);
    ex = cases;
    if (long_buffers(4000))
        return 1;
    printf("long: 4000 buffers x 4 alignments = %lu cases\n", cases - ex);
    printf("outcomes (of %lu cases): ok %lu, ok with emulation bytes removed %lu, no start code %lu, error with the buffer "
           "untouched %lu, error after moving bytes %lu\n", cases, nOk, nOkMoved, nNokNoStart, nNokKept, nNokMoved);
    printf("NAL PASS\n");
    return 0;
}
