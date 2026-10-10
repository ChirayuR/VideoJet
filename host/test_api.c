/* test_api STREAMS_DIR: unit checks of the public API that the stream gate cannot reach. Plain asserts. */
#undef NDEBUG /* the checks are the asserts: they stay live whatever CFLAGS say (-DNDEBUG would pass vacuously) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "videojet.h"
#include "vj_hash.h"

#define LEN(a) (sizeof(a))

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    size_t got = b ? fread(b, 1, *len, f) : 0;
    assert(b && got == *len);
    fclose(f);
    return b;
}

static void test_au_length(void)
{
    /* SPS, PPS, IDR slice (first_mb_in_slice 0: top bit of the byte after the NAL header), then a P slice (mb 0). */
    static const uint8_t two[] = {0, 0, 1, 0x67, 0xAA, 0, 0, 1, 0x68, 0xBB, 0, 0, 1, 0x65, 0x88, 0x01,
                                  0, 0, 1, 0x41, 0x9A, 0x02};
    assert(vj_au_length(two, LEN(two)) == 16);                 /* boundary at the P slice's start code */
    assert(vj_au_length(two + 16, LEN(two) - 16) == LEN(two) - 16);
    /* Second slice of the same picture (first_mb_in_slice != 0: top bit clear), then a new picture. */
    static const uint8_t slices[] = {0, 0, 1, 0x65, 0x88, 0, 0, 1, 0x65, 0x08, 0, 0, 1, 0x41, 0x9A};
    assert(vj_au_length(slices, LEN(slices)) == 10);
    /* A 4-byte start code: the boundary owns its leading zero. */
    static const uint8_t four[] = {0, 0, 1, 0x65, 0x88, 0x11, 0, 0, 0, 1, 0x41, 0x9A};
    assert(vj_au_length(four, LEN(four)) == 6);
    /* SEI after a slice starts the next AU. */
    static const uint8_t sei[] = {0, 0, 1, 0x65, 0x88, 0, 0, 1, 0x06, 0x05};
    assert(vj_au_length(sei, LEN(sei)) == 5);
    /* AUD after a slice starts the next AU; an AUD at the head of an AU is part of it (no slice seen yet). */
    static const uint8_t aud[] = {0, 0, 1, 0x65, 0x88, 0, 0, 1, 0x09, 0xF0};
    assert(vj_au_length(aud, LEN(aud)) == 5);
    static const uint8_t aud2[] = {0, 0, 1, 0x09, 0xF0, 0, 0, 1, 0x65, 0x88, 0, 0, 1, 0x09, 0xF0, 0, 0, 1, 0x41, 0x9A};
    assert(vj_au_length(aud2, LEN(aud2)) == 10);
    /* No start code, NULL, empty. */
    static const uint8_t junk[] = {1, 2, 3, 4};
    assert(vj_au_length(junk, LEN(junk)) == 0);
    assert(vj_au_length(NULL, 10) == 0);
    assert(vj_au_length(two, 0) == 0);
}

static void test_args(void)
{
    static uint8_t b[] = {0, 0, 1, 0x67};
    vj_frame_t f;
    vj_decoder_t *d = vj_decoder_create();
    assert(d);
    assert(vj_decode_au(NULL, b, LEN(b), &f) == VJ_ERR_ARG);
    assert(vj_decode_au(d, NULL, LEN(b), &f) == VJ_ERR_ARG);
    assert(vj_decode_au(d, b, 0, &f) == VJ_ERR_ARG);
    assert(vj_decode_au(d, b, LEN(b), NULL) == VJ_ERR_ARG);
    vj_decoder_destroy(d);
    vj_decoder_destroy(NULL); /* allowed */
}

/* The first AU of aux_card is SPS + PPS + SEI + IDR. Its prefix up to the IDR slice holds no slice: VJ_NO_FRAME.
 * Then the whole first AU: VJ_FRAME, an IDR, 800x480, full frame shown, clean. */
static void test_frames(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/search/aux_card.h264", dir);
    size_t len;
    uint8_t *s = slurp(path, &len);
    size_t au = vj_au_length(s, len);
    assert(au > 0 && au < len);
    size_t idr = 0; /* offset of the IDR slice's start code within the first AU */
    for (size_t i = 0; i + 3 < au; i++)
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1 && (s[i + 3] & 0x1f) == 5) {
            idr = i;
            break;
        }
    assert(idr > 0);
    vj_frame_t f;
    vj_decoder_t *d = vj_decoder_create();
    assert(vj_decode_au(d, s, idr, &f) == VJ_NO_FRAME);
    assert(vj_decode_au(d, s + idr, au - idr, &f) == VJ_FRAME);
    assert(f.idr && f.width == 800 && f.height == 480 && f.error_mbs == 0 && f.changed_mb == NULL);
    assert(f.crop_x == 0 && f.crop_y == 0 && f.crop_w == 800 && f.crop_h == 480);
    assert(f.y_stride == 800 && f.uv_stride == 400 && f.u == f.y + 800 * 480 && f.v == f.u + 400 * 240);
    assert(!f.full_range && f.matrix == 5); /* gen_streams.py: --range tv --colormatrix bt470bg */
    vj_decoder_destroy(d);

    /* h264bsd strips emulation-prevention bytes from the AU in place, so an AU decodes once: reload the stream. */
    free(s);
    s = slurp(path, &len);

    /* An AU cut inside its slice is an error; the next AU still decodes (h264bsd conceals the cut picture and the
     * API drops it, so each call returns at most one picture). */
    d = vj_decoder_create();
    size_t au2 = vj_au_length(s + au, len - au);
    assert(vj_decode_au(d, s, au, &f) == VJ_FRAME);
    assert(vj_decode_au(d, s + au, au2 / 2, &f) == VJ_ERR_STREAM);
    assert(vj_decode_au(d, s + au + au2, vj_au_length(s + au + au2, len - au - au2), &f) == VJ_FRAME);
    vj_decoder_destroy(d);
    free(s);
}

/* Make the slice header of the AU at `au` unparsable: slice_type becomes a B slice, which baseline never has. */
static void damage_header(uint8_t *au)
{
    while (!(au[0] == 0 && au[1] == 0 && au[2] == 1))
        au++;
    au[4] ^= 0x04; /* the byte after the NAL header */
}

/* Offset of the start code of slice number k (from 0) in the n-byte AU. */
static size_t slice_at(const uint8_t *au, size_t n, int k)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1 && ((au[i + 3] & 0x1f) == 1 || (au[i + 3] & 0x1f) == 5) &&
            k-- == 0)
            return i;
    assert(!"slice not found");
    return 0;
}

/* Consumers reuse one buffer for every AU, and a damaged AU is followed by more. Decode the first AUs of aux_card
 * with one slice-header bit flipped in the second, once from a fresh block per AU and once from a single buffer.
 * Everything must agree, and the AU after the damaged one must decode. */
#define NAU 8

static void decode_run(const uint8_t *s, const size_t *off, const size_t *len, int reuse, int *st, uint32_t *hash)
{
    size_t max = 0;
    for (int i = 0; i < NAU; i++)
        max = len[i] > max ? len[i] : max;
    uint8_t *one = malloc(max), *blk[NAU];
    vj_decoder_t *d = vj_decoder_create();
    for (int i = 0; i < NAU; i++) {
        blk[i] = reuse ? one : malloc(len[i]);
        memcpy(blk[i], s + off[i], len[i]); /* a fresh copy: decoding strips emulation-prevention bytes in place */
        vj_frame_t f;
        st[i] = vj_decode_au(d, blk[i], len[i], &f);
        hash[i] = st[i] == VJ_FRAME
                      ? vj_hash_i420(f.y, f.y_stride, f.u, f.v, f.uv_stride, f.crop_x, f.crop_y, f.crop_w, f.crop_h)
                      : 0;
    }
    vj_decoder_destroy(d);
    for (int i = 0; i < NAU; i++)
        if (!reuse)
            free(blk[i]);
    free(one);
}

static void test_reuse(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/search/aux_card.h264", dir);
    size_t total, off[NAU], len[NAU], o = 0;
    uint8_t *s = slurp(path, &total);
    for (int i = 0; i < NAU; i++) {
        off[i] = o;
        len[i] = vj_au_length(s + o, total - o);
        assert(len[i]);
        o += len[i];
    }
    damage_header(s + off[1]);
    int st_a[NAU], st_b[NAU];
    uint32_t h_a[NAU], h_b[NAU];
    decode_run(s, off, len, 0, st_a, h_a);
    decode_run(s, off, len, 1, st_b, h_b);
    assert(st_a[0] == VJ_FRAME && st_a[1] == VJ_ERR_STREAM); /* the damage is felt */
    assert(st_a[2] == VJ_FRAME); /* and the AU after it is decoded, not swallowed */
    for (int i = 0; i < NAU; i++)
        assert(st_a[i] == st_b[i] && h_a[i] == h_b[i]);
    free(s);
}

/* The call after a damaged AU conceals the half-parsed picture and must still decode its own AU. Here that AU is the
 * recovery IDR (AA sends one after a keyframe request): no error path may swallow it. */
static void test_recovery(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/search/aux_card.h264", dir);
    size_t len;
    uint8_t *s = slurp(path, &len);
    size_t n0 = vj_au_length(s, len), n1 = vj_au_length(s + n0, len - n0);
    uint8_t *idr = malloc(n0);
    memcpy(idr, s, n0); /* decoding rewrites an AU in place: keep a copy to send again */
    damage_header(s + n0);
    vj_frame_t f;
    vj_decoder_t *d = vj_decoder_create();
    assert(vj_decode_au(d, s, n0, &f) == VJ_FRAME);
    assert(vj_decode_au(d, s + n0, n1, &f) == VJ_ERR_STREAM);
    assert(vj_decode_au(d, idr, n0, &f) == VJ_FRAME && f.idr && f.error_mbs == 0);
    vj_decoder_destroy(d);
    free(idr);
    free(s);
}

/* Two more contract edges, on aux_map_dark (4 slices per picture): an AU that stops after 2 of its 4 slices is an error
 * and the next AU still decodes; two AUs handed over as one call are an error (one picture per call). */
static void test_contract(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/heldout/aux_map_dark.h264", dir);
    size_t len;
    uint8_t *s = slurp(path, &len);
    size_t n0 = vj_au_length(s, len), n1 = vj_au_length(s + n0, len - n0);
    size_t n2 = vj_au_length(s + n0 + n1, len - n0 - n1);
    vj_frame_t f;
    vj_decoder_t *d = vj_decoder_create();
    assert(vj_decode_au(d, s, n0, &f) == VJ_FRAME);
    assert(vj_decode_au(d, s + n0, slice_at(s + n0, n1, 2), &f) == VJ_ERR_STREAM); /* slices 0 and 1 only */
    assert(vj_decode_au(d, s + n0 + n1, n2, &f) == VJ_FRAME);
    vj_decoder_destroy(d);

    free(s);
    s = slurp(path, &len); /* decoding rewrote the AUs: start from the file again */
    d = vj_decoder_create();
    assert(vj_decode_au(d, s, n0, &f) == VJ_FRAME);
    assert(vj_decode_au(d, s + n0, n1 + n2, &f) == VJ_ERR_STREAM); /* two pictures in one call */
    vj_decoder_destroy(d);
    free(s);
}

/* A call never continues the last one. Hand over [AU1][AU2 cut after 2 of its 4 slices][AU3] as one call: an error
 * (AU3's first slice conceals the cut picture, which makes a second picture) that leaves h264bsd expecting a re-call at
 * AU3's address. Then decode AU4 where AU3 began, or in a fresh block: it must decode either way. */
static int resume_run(const uint8_t *s, const size_t *off, const size_t *n, int same_address)
{
    size_t cut = slice_at(s + off[2], n[2], 2), at = n[1] + cut;
    uint8_t *big = malloc(at + n[3]), *a0 = malloc(n[0]), *a4 = malloc(n[4]);
    memcpy(a0, s + off[0], n[0]);
    memcpy(big, s + off[1], n[1]);
    memcpy(big + n[1], s + off[2], cut);
    memcpy(big + at, s + off[3], n[3]);
    vj_frame_t f;
    vj_decoder_t *d = vj_decoder_create();
    assert(vj_decode_au(d, a0, n[0], &f) == VJ_FRAME);
    assert(vj_decode_au(d, big, at + n[3], &f) == VJ_ERR_STREAM);
    uint8_t *au4 = same_address ? big + at : a4;
    memcpy(au4, s + off[4], n[4]);
    int st = vj_decode_au(d, au4, n[4], &f);
    vj_decoder_destroy(d);
    free(a4);
    free(a0);
    free(big);
    return st;
}

static void test_resume(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/heldout/aux_map_dark.h264", dir);
    size_t total, off[5], n[5], o = 0;
    uint8_t *s = slurp(path, &total);
    for (int i = 0; i < 5; i++) {
        off[i] = o;
        n[i] = vj_au_length(s + o, total - o);
        o += n[i];
    }
    assert(resume_run(s, off, n, 0) == VJ_FRAME);
    assert(resume_run(s, off, n, 1) == VJ_FRAME);
    free(s);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../streams";
    test_au_length();
    test_args();
    test_frames(dir);
    test_reuse(dir);
    test_recovery(dir);
    test_contract(dir);
    test_resume(dir);
    puts("test_api: all passed");
    return 0;
}
