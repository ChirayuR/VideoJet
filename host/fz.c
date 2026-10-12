/* Damaged-stream differential (from the T3 review, host/fzdiff.sh drives it): fz STREAM SEED RATE
 * Splits STREAM into AUs (vj_au_length), damages about 1 in RATE of them with one of 9 ops, and prints one line per
 * vj_decode_au call: status and FNV-1a of the full coded frame. On stderr: changed_mb violations (an MB called
 * unchanged that differs from the previous VJ_FRAME). Same seed => same calls, so two builds can be diffed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "videojet.h"
#ifdef VJ_FZ_STORAGE
#include "h264bsd_storage.h"
#endif

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }
static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

#ifdef VJ_FZ_STORAGE
/* FNV-1a over every observable field of mb[] and the slice / picture counters, after every call: the differential then
 * compares the decoder's whole storage state, not only what it outputs. It needs only the private headers, so it also
 * builds against an older tree (the base decoder of a differential): copy this file there. */
static uint64_t storage_hash(vj_decoder_t *dec)
{
    storage_t *st = (storage_t *)(void *)dec;
    uint64_t h = 0xcbf29ce484222325ULL;
    h = fnv((const uint8_t *)&st->slice->sliceId, 4, h);
    h = fnv((const uint8_t *)&st->slice->numDecodedMbs, 4, h);
    h = fnv((const uint8_t *)&st->slice->lastMbAddr, 4, h);
    h = fnv((const uint8_t *)&st->picSliceBase, 4, h);
    h = fnv((const uint8_t *)&st->numConcealedMbs, 4, h);
    h = fnv((const uint8_t *)&st->picStarted, 4, h);
    h = fnv((const uint8_t *)&st->vjRecNext, 4, h);
    if (!st->mb)
        return h;
    for (unsigned i = 0; i < st->picSizeInMbs; i++) {
        const mbStorage_t *m = st->mb + i;
        h = fnv((const uint8_t *)&m->mbType, sizeof m->mbType, h);
        h = fnv((const uint8_t *)&m->sliceId, 4, h);
        h = fnv((const uint8_t *)&m->disableDeblockingFilterIdc, 4, h);
        h = fnv((const uint8_t *)&m->filterOffsetA, 4, h);
        h = fnv((const uint8_t *)&m->filterOffsetB, 4, h);
        h = fnv((const uint8_t *)&m->qpY, 4, h);
        h = fnv((const uint8_t *)&m->chromaQpIndexOffset, 4, h);
        h = fnv((const uint8_t *)m->totalCoeff, sizeof m->totalCoeff, h);
        h = fnv(m->intra4x4PredMode, 16, h);
        h = fnv((const uint8_t *)m->refPic, sizeof m->refPic, h);
        for (int k = 0; k < 4; k++) {
            uint8_t z = m->refAddr[k] != NULL; /* the address itself differs between runs */
            h = fnv(&z, 1, h);
        }
        h = fnv((const uint8_t *)m->mv, sizeof m->mv, h);
        h = fnv((const uint8_t *)&m->decoded, 4, h);
    }
    return h;
}
#endif

static vj_decoder_t *d;
static uint8_t *prev;
static size_t prevsz;
static unsigned long viol, frames, calls, errs, partial;

static void feed(const uint8_t *src, size_t n)
{
    if (!n) return;
    uint8_t *au = malloc(n);
    memcpy(au, src, n);
    vj_frame_t f;
    vj_status_t r = vj_decode_au(d, au, n, &f);
    calls++;
#ifdef VJ_FZ_STORAGE
    printf("H %016llx\n", (unsigned long long)storage_hash(d));
#endif
    if (r != VJ_FRAME) {
        printf("%c\n", r == VJ_NO_FRAME ? 'n' : 'E');
        errs += r != VJ_NO_FRAME;
        free(au);
        return;
    }
    frames++;
    size_t ysz = (size_t)f.width * f.height;
    uint64_t h = fnv(f.y, ysz, 0xcbf29ce484222325ULL);
    h = fnv(f.u, ysz / 4, h);
    h = fnv(f.v, ysz / 4, h);
    unsigned w = f.width / 16, hh = f.height / 16, nch = 0;
    /* the changed_mb map too: marks are part of the contract, so the diff of two builds covers them exactly */
    uint64_t hc = f.changed_mb ? fnv(f.changed_mb, (size_t)w * hh, 0xcbf29ce484222325ULL) : 0;
    printf("F %016llx %u %016llx\n", (unsigned long long)h, f.error_mbs, (unsigned long long)hc);
    int same = prev && prevsz == ysz * 3 / 2;
    for (unsigned i = 0; i < w * hh; i++) {
        unsigned x = i % w, y = i / w;
        if (!f.changed_mb || f.changed_mb[i]) { nch++; continue; }
        int diff = !same;
        for (unsigned rr = 0; !diff && rr < 16; rr++)
            diff = memcmp(f.y + (y * 16 + rr) * f.y_stride + x * 16, prev + (y * 16 + rr) * f.width + x * 16, 16) != 0;
        for (unsigned rr = 0; !diff && rr < 8; rr++) {
            size_t o = (y * 8 + rr) * (size_t)f.uv_stride + x * 8;
            diff = memcmp(f.u + o, prev + ysz + o, 8) || memcmp(f.v + o, prev + ysz + ysz / 4 + o, 8);
        }
        viol += diff;
    }
    partial += nch < w * hh;
    if (!same) { free(prev); prev = malloc(ysz * 3 / 2); prevsz = ysz * 3 / 2; }
    memcpy(prev, f.y, ysz);
    memcpy(prev + ysz, f.u, ysz / 4);
    memcpy(prev + ysz + ysz / 4, f.v, ysz / 4);
    free(au);
}

int main(int argc, char **argv)
{
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 2;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *s = malloc(len);
    if (fread(s, 1, len, fp) != (size_t)len) return 3;
    fclose(fp);
    rs = 0x9E3779B97F4A7C15ULL ^ (uint64_t)strtoull(argv[2], 0, 10) * 0x2545F4914F6CDD1DULL;
    unsigned rate = argc > 3 ? (unsigned)atoi(argv[3]) : 6;
    d = vj_decoder_create();
    uint8_t *tmp = malloc(len + 16);
    size_t o = 0;
    while (o < (size_t)len) {
        size_t au = vj_au_length(s + o, len - o);
        if (!au) break;
        const uint8_t *a = s + o;
        unsigned op = (rnd() % rate) ? 0 : 1 + rnd() % 9;
        size_t cut = au > 8 ? 5 + rnd() % (au - 5) : au;
        size_t bit = au > 6 ? (5 + rnd() % (au - 5)) * 8 + rnd() % 8 : 0;
        switch (op) {
        case 0: feed(a, au); break;
        case 1: feed(a, cut); break;                                   /* truncate */
        case 2: memcpy(tmp, a, au); tmp[bit / 8] ^= 1 << (bit % 8); feed(tmp, au); break; /* bit flip */
        case 3: break;                                                  /* drop */
        case 4: memcpy(tmp, a, au); tmp[bit / 8] ^= 1 << (bit % 8); feed(tmp, au); feed(a, au); break; /* flip+resend */
        case 5: feed(a, cut); feed(a, au); break;                       /* cut + resend */
        case 6: feed(a, au); feed(a, au); break;                        /* duplicate */
        case 7: memcpy(tmp, a, au); if (au > 12) tmp[5 + rnd() % 8] ^= 1 << (rnd() % 8); feed(tmp, au); break; /* header */
        case 8: feed(a, cut); feed(a + cut, au - cut); break;           /* split in two calls */
        case 9: memcpy(tmp, a, au); for (int k = 0; k < 4; k++) { size_t b2 = (5 + rnd() % (au > 6 ? au - 5 : 1)); if (b2 < au) tmp[b2] = (uint8_t)rnd(); } feed(tmp, au); feed(a, au); break; /* burst + resend */
        }
        o += au;
    }
    vj_decoder_destroy(d);
    free(prev); free(tmp); free(s);
    fprintf(stderr, "calls %lu frames %lu errs %lu partial %lu viol %lu\n", calls, frames, errs, partial, viol);
    return 0;
}
