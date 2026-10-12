/* test_alpha0: why the deblock may skip a macroblock whose three thresholds (top, left, inner) all have alpha or beta 0
 * (EdgesInert in h264bsd_deblocking.c): its edge filters could not change a pixel.
 *  1. Edge filters: every (indexA, indexB) pair with alpha[indexA] == 0 or beta[indexB] == 0, every bS 1..4, each of the six
 *     filters (vertical and horizontal luma edge, 16-wide horizontal luma, and the same three for chroma), on random
 *     samples of five kinds: no byte of the sample plane changes.
 *  2. Macroblock, as the decoder calls it: random qps, filter offsets and chroma qp offset, thresholds from GetEdgeThresholds,
 *     random bS; whenever EdgesInert holds FilterLuma / FilterChroma change nothing.
 *  3. Macroblock, each of the three thresholds inert or not at random (so an EdgesInert that ignored one of them fails):
 *     when EdgesInert holds, nothing changes.
 *  4. EdgesInert is exactly "all three of (alpha == 0 or beta == 0)": its truth table over 4 alphas x 4 betas per slot (a weaker
 *     test would be exact too, but skip less).
 *  5. The harness is not blind: the other pairs, and the macroblocks where EdgesInert does not hold, do change bytes
 *     (every non-inert pair does, for some filter and bS), so a pass is not an empty test.
 * It includes h264bsd_deblocking.c (the filters are static) and links the rest of the library. Prints the case counts;
 * exit 0 when everything holds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264bsd_deblocking.c"

static unsigned long long rs = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 11);
}

static u8 clip8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* A w x h plane of one of five kinds of content, picked so that the filters' tests (alpha, beta, the strong filter's) pass
 * about as often as they fail: random bytes, smooth, 4x4 blocks with steps, steps of up to 24 across line and column s, and
 * the extremes (clipping) */
static void fill(u8 *p, int w, int h, int s, unsigned kind)
{
    int x, y, v0 = (int)(rnd() % 256), d = (int)(rnd() % 49) - 24, e = (int)(rnd() % 49) - 24;
    int blk[16][16];

    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            blk[y][x] = (y ? blk[y - 1][x] : x ? blk[0][x - 1] : v0) + (int)(rnd() % 21) - 10;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            int v;

            switch (kind % 5)
            {
            case 0: v = (int)(rnd() % 256); break;
            case 1: v = v0 + (int)(rnd() % 5) - 2; break;
            case 2: v = blk[(y / 4) & 15][(x / 4) & 15] + (int)(rnd() % 3) - 1; break;
            case 3: v = v0 + (x >= s ? d : 0) + (y >= s ? e : 0) + (int)(rnd() % 3); break;
            default: v = (rnd() & 1 ? 255 : 0) + (int)(rnd() % 7) - 3; break;
            }
            p[y * w + x] = clip8(v);
        }
}

/* the six edge filters on a 32 x 32 plane, the edge at (8, 8); each returns after its call */
#define PW 32
enum { F_VLUMA, F_HLUMA_EDGE, F_HLUMA, F_VCHROMA, F_HCHROMA_EDGE, F_HCHROMA, NF };
static const char *const fname[NF] = {"FilterVerLumaEdge", "FilterHorLumaEdge", "FilterHorLuma",
    "FilterVerChromaEdge", "FilterHorChromaEdge", "FilterHorChroma"};

static void run_filter(int f, u8 *at, u32 bS, edgeThreshold_t *t)
{
    switch (f)
    {
    case F_VLUMA: FilterVerLumaEdge(at, bS, t, PW); break;
    case F_HLUMA_EDGE: FilterHorLumaEdge(at, bS, t, PW); break;
    case F_HLUMA: FilterHorLuma(at, bS, t, PW); break;
    case F_VCHROMA: FilterVerChromaEdge(at, bS, t, PW); break;
    case F_HCHROMA_EDGE: FilterHorChromaEdge(at, bS, t, PW); break;
    default: FilterHorChroma(at, bS, t, PW); break;
    }
}

static unsigned long long bad;

int main(void)
{
    static u8 buf[PW * PW], ref[PW * PW];
    unsigned long long inertCases = 0, otherCases = 0, otherChanged = 0, mbInert[2] = {0, 0}, mbOther[2] = {0, 0},
        mbOtherChanged[2] = {0, 0}, trInert[2] = {0, 0}, trOther[2] = {0, 0}, trOtherChanged[2] = {0, 0};
    unsigned long long perFilterChanged[NF] = {0};
    unsigned ia, ib, nInertPairs = 0, nBlindPairs = 0;
    int f;

    /* 1. edge filters, every pair */
    for (ia = 0; ia < 52; ia++)
        for (ib = 0; ib < 52; ib++)
        {
            int inert = !alphas[ia] || !betas[ib];
            int pairChanged = 0;
            edgeThreshold_t t;
            u32 bS;
            unsigned k;

            t.tc0 = tc0[ia];
            t.alpha = alphas[ia];
            t.beta = betas[ib];
            nInertPairs += inert;
            for (bS = 1; bS <= 4; bS++)
                for (k = 0; k < 20; k++)
                {
                    fill(buf, PW, PW, 8, k);
                    for (f = 0; f < NF; f++)
                    {
                        int changed;

                        /* FilterHorLumaEdge and FilterHorChromaEdge are only called with bS < 4 */
                        if (bS == 4 && (f == F_HLUMA_EDGE || f == F_HCHROMA_EDGE))
                            continue;
                        memcpy(ref, buf, sizeof buf);
                        run_filter(f, ref + 8 * PW + 8, bS, &t);
                        changed = memcmp(ref, buf, sizeof buf) != 0;
                        if (inert)
                        {
                            inertCases++;
                            if (changed && bad++ < 8)
                                printf("CHANGED %s indexA %u indexB %u bS %u\n", fname[f], ia, ib, (unsigned)bS);
                        }
                        else
                        {
                            otherCases++;
                            otherChanged += changed;
                            perFilterChanged[f] += changed;
                            pairChanged |= changed;
                        }
                    }
                }
            if (!inert && !pairChanged)
            {
                nBlindPairs++;
                if (bad++ < 8)
                    printf("BLIND: no filter changed a byte for indexA %u indexB %u\n", ia, ib);
            }
        }
    for (f = 0; f < NF; f++)
        if (!perFilterChanged[f] && bad++ < 8)
            printf("BLIND: %s never changed a byte\n", fname[f]);

    /* the predicate's truth table */
    {
        static const u8 av[4] = {0, 1, 4, 255}, bv[4] = {0, 1, 2, 18};
        unsigned c, n = 0;

        for (c = 0; c < 4096; c++)
        {
            edgeThreshold_t t[3];
            int i;
            u32 want = 1;

            for (i = 0; i < 3; i++)
            {
                unsigned sel = (c >> (4 * i)) & 15;

                t[i].tc0 = tc0[0];
                t[i].alpha = av[sel & 3];
                t[i].beta = bv[sel >> 2];
                want &= t[i].alpha == 0 || t[i].beta == 0;
            }
            n++;
            if (EdgesInert(t) != want && bad++ < 8)
                printf("EdgesInert wrong for %u: alphas %u %u %u betas %u %u %u\n", c, (unsigned)t[0].alpha, (unsigned)t[1].alpha,
                    (unsigned)t[2].alpha, (unsigned)t[0].beta, (unsigned)t[1].beta, (unsigned)t[2].beta);
        }
        printf("test_alpha0: EdgesInert truth table: %u combinations\n", n);
    }

    /* 2. and 3. whole macroblocks. Luma: a 48 x 48 plane, the MB at (16, 16); chroma: two 32 x 32 planes, the MB at (8, 8) */
    {
        static u8 luma[48 * 48], lref[48 * 48], cb[32 * 32], cr[32 * 32], cbref[32 * 32], crref[32 * 32];
        unsigned it;

        for (it = 0; it < 60000; it++)
        {
            edgeThreshold_t t[3];
            bS_t bS[16];
            int i, tr = it & 1, chroma = (it >> 1) & 1, inert, changed;

            if (!tr)
            {
                /* the decoder's thresholds: qps clustered around a random level, so all three are inert about a third of the time */
                deblockRec_t rec;
                u32 lo = rnd() % 40, qp, qpTop, qpLeft;

                qp = lo + rnd() % 14;
                qpTop = lo + rnd() % 14;
                qpLeft = lo + rnd() % 14;
                memset(&rec, 0, sizeof rec);
                rec.qp = (u8)qp;
                rec.offA = (i8)(2 * ((int)(rnd() % 13) - 6));
                rec.offB = (i8)(2 * ((int)(rnd() % 13) - 6));
                rec.chromaQpOff = (i8)((int)(rnd() % 25) - 12);
                GetEdgeThresholds(t, &rec, qpTop, qpLeft, chroma);
            }
            else
            {
                /* each slot its own pair, inert or not at random */
                for (i = 0; i < 3; i++)
                {
                    unsigned a = rnd() % 52, b = rnd() % 52;

                    if (rnd() & 1)
                    {
                        if (rnd() & 1)
                            a = rnd() % 16;
                        else
                            b = rnd() % 16;
                    }
                    else
                    {
                        a = 16 + rnd() % 36;
                        b = 16 + rnd() % 36;
                    }
                    t[i].tc0 = tc0[a];
                    t[i].alpha = alphas[a];
                    t[i].beta = betas[b];
                }
            }
            inert = EdgesInert(t);

            /* bS the way the records give them: inner 0..3, an MB edge 0..3 per segment or 4 for the whole top edge; mostly
             * non-zero and often equal along a row, so the 16- and 8-wide horizontal filters run too */
            for (i = 0; i < 16; i++)
            {
                bS[i].top = rnd() % 5 ? rnd() % 4 : 0;
                bS[i].left = rnd() % 5 ? rnd() % 4 : 0;
            }
            for (i = 0; i < 4; i++)
                if (rnd() % 3 == 0)
                    bS[4 * i + 1].top = bS[4 * i + 2].top = bS[4 * i + 3].top = bS[4 * i].top;
            if (rnd() % 4 == 0)
                bS[0].top = bS[1].top = bS[2].top = bS[3].top = 4;
            for (i = 0; i < 4; i++)
                if (rnd() % 4 == 0)
                    bS[4 * i].left = 4;

            if (!chroma || tr)
            {
                fill(luma, 48, 48, 16 + 4 * (int)(rnd() % 4), rnd());
                memcpy(lref, luma, sizeof luma);
                FilterLuma(lref + 16 * 48 + 16, bS, t, 48);
                changed = memcmp(lref, luma, sizeof luma) != 0;
                if (inert)
                {
                    (tr ? trInert : mbInert)[0]++;
                    if (changed && bad++ < 8)
                        printf("CHANGED FilterLuma (%s)\n", tr ? "slots" : "records");
                }
                else
                {
                    (tr ? trOther : mbOther)[0]++;
                    (tr ? trOtherChanged : mbOtherChanged)[0] += changed;
                }
            }
            if (chroma || tr)
            {
                fill(cb, 32, 32, 8 + 4 * (int)(rnd() % 2), rnd());
                fill(cr, 32, 32, 8 + 4 * (int)(rnd() % 2), rnd());
                memcpy(cbref, cb, sizeof cb);
                memcpy(crref, cr, sizeof cr);
                FilterChroma(cbref + 8 * 32 + 8, crref + 8 * 32 + 8, bS, t, 32);
                changed = memcmp(cbref, cb, sizeof cb) != 0 || memcmp(crref, cr, sizeof cr) != 0;
                if (inert)
                {
                    (tr ? trInert : mbInert)[1]++;
                    if (changed && bad++ < 8)
                        printf("CHANGED FilterChroma (%s)\n", tr ? "slots" : "records");
                }
                else
                {
                    (tr ? trOther : mbOther)[1]++;
                    (tr ? trOtherChanged : mbOtherChanged)[1] += changed;
                }
            }
        }
    }
    for (f = 0; f < 2; f++)
        if (!mbInert[f] || !mbOtherChanged[f] || !trInert[f] || !trOtherChanged[f])
        {
            if (bad++ < 8)
                printf("BLIND: macroblock %s: inert %llu / %llu, other changed %llu / %llu\n", f ? "chroma" : "luma",
                    mbInert[f], trInert[f], mbOtherChanged[f], trOtherChanged[f]);
        }
    printf("test_alpha0: %u inert (indexA, indexB) pairs: %llu edge-filter runs, 0 expected changes; %llu runs of the other %u pairs, "
           "%llu changed bytes (%u pairs never did)\n",
        nInertPairs, inertCases, otherCases, 52 * 52 - nInertPairs, otherChanged, nBlindPairs);
    printf("test_alpha0: macroblocks, decoder thresholds: luma %llu inert + %llu other (%llu changed), chroma %llu inert + %llu other "
           "(%llu changed); random slots: luma %llu inert + %llu other (%llu changed), chroma %llu inert + %llu other (%llu changed)\n",
        mbInert[0], mbOther[0], mbOtherChanged[0], mbInert[1], mbOther[1], mbOtherChanged[1], trInert[0], trOther[0],
        trOtherChanged[0], trInert[1], trOther[1], trOtherChanged[1]);
    printf("test_alpha0: %llu failures\n", bad);
    return bad != 0;
}
