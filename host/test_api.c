/* test_api STREAMS_DIR VECTORS_DIR: unit checks of the public API that the stream gate cannot reach. Plain asserts. */
#undef NDEBUG /* the checks are the asserts: they stay live whatever CFLAGS say (-DNDEBUG would pass vacuously) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "videojet.h"
#include "vj_hash.h"
#include "h264bsd_storage.h" /* test_twice moves the slice ids to the restart point */

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
    assert(f.idr && f.width == 800 && f.height == 480 && f.error_mbs == 0 && f.changed_mb != NULL);
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

/* A byte stream that lost the FIRST slice of picture N+1 (aux_map_dark, 4 slices per picture). vj_au_length() sees a
 * boundary only at a slice with first_mb_in_slice 0, so N+1's other slices ride along with AU N: one call that
 * completes a picture and leaves the next one unfinished. That is an error, not a frame: the next call conceals and
 * drops the unfinished picture, and every picture after it would decode on a lost reference with nothing to say so. */
static void test_lost_slice(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/heldout/aux_map_dark.h264", dir);
    size_t total;
    uint8_t *s = slurp(path, &total);
    size_t n0 = vj_au_length(s, total), n1 = vj_au_length(s + n0, total - n0);
    size_t n2 = vj_au_length(s + n0 + n1, total - n0 - n1);
    size_t cut = slice_at(s + n0 + n1, n2, 1); /* AU2's slice 1 starts here: everything before it is the lost slice 0 */
    memmove(s + n0 + n1, s + n0 + n1 + cut, total - n0 - n1 - cut);
    total -= cut;
    size_t merged = vj_au_length(s + n0, total - n0);
    assert(merged == n1 + n2 - cut); /* the splitter itself hands over AU1 plus the rest of AU2 */
    size_t n3 = vj_au_length(s + n0 + merged, total - n0 - merged);
    vj_frame_t f;
    vj_decoder_t *d = vj_decoder_create();
    assert(vj_decode_au(d, s, n0, &f) == VJ_FRAME);
    assert(vj_decode_au(d, s + n0, merged, &f) == VJ_ERR_STREAM); /* picture 1 is done, picture 2 is left unfinished */
    assert(vj_decode_au(d, s + n0 + merged, n3, &f) == VJ_FRAME); /* and the next AU still decodes, not swallowed */
    vj_decoder_destroy(d);
    free(s);
}

/* A call never continues the last one. Hand over [AU1][AU2 cut after 2 of its 4 slices][AU3] as one call: an error
 * (AU3's first slice conceals the cut picture, which makes a second picture) that leaves h264bsd expecting a re-call at
 * AU3's address. Then decode AU4 where AU3 began, or in a fresh block: it must decode either way. */
static int resume_run(const uint8_t *s, const size_t *off, const size_t *n, int same_address)
{
    size_t cut = slice_at(s + off[2], n[2], 2), at = n[1] + cut;
    size_t tail = n[3] > n[4] ? n[3] : n[4]; /* AU3 is copied to big + at, and AU4 too when same_address */
    uint8_t *big = malloc(at + tail), *a0 = malloc(n[0]), *a4 = malloc(n[4]);
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

/* mb[] is not cleared per picture (slice ids only grow, h264bsdResetStorage). Decode aux_map_dark (4 slices per
 * picture) twice through one decoder, IDR to end: every frame of both passes hashes to the golden. Pass 2 starts with
 * the ids 600 below the half-range restart, so it also crosses the restart (the real clear, then ids from 0). */
static void test_twice(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/heldout/aux_map_dark.golden", dir);
    FILE *g = fopen(path, "r");
    assert(g);
    unsigned gold[1000], ng = 0;
    while (ng < 1000 && fscanf(g, "%x", &gold[ng]) == 1)
        ng++;
    fclose(g);
    assert(ng > 0 && ng < 1000);
    snprintf(path, sizeof path, "%s/heldout/aux_map_dark.h264", dir);
    vj_decoder_t *d = vj_decoder_create();
    storage_t *st = (storage_t *)(void *)d;
    for (int pass = 0; pass < 2; pass++) {
        size_t len;
        uint8_t *s = slurp(path, &len); /* decoding rewrites the AUs in place: a fresh copy per pass */
        unsigned n = 0;
        for (size_t o = 0; o < len;) {
            size_t au = vj_au_length(s + o, len - o);
            assert(au);
            vj_frame_t f;
            vj_status_t r = vj_decode_au(d, s + o, au, &f);
            o += au;
            assert(r == VJ_FRAME || r == VJ_NO_FRAME);
            if (r == VJ_FRAME) {
                assert(n < ng);
                assert(vj_hash_i420(f.y, f.y_stride, f.u, f.v, f.uv_stride, f.crop_x, f.crop_y, f.crop_w, f.crop_h) ==
                       gold[n]);
                n++;
            }
        }
        assert(n == ng);
        free(s);
        if (pass == 0)
            st->picSliceBase = st->slice->sliceId = 0x80000000u - 600; /* as if a reset had just made it so */
    }
    assert(st->picSliceBase < 0x80000000u - 600); /* the restart ran */
    vj_decoder_destroy(d);
}

/* Concealment next to older pictures' MBs. aux_map_dark's IDR loses slices [lo, hi) of its 4 and is concealed from its
 * decoded neighbours (intra, no reference); AU 1 decodes on it. Concealment reads only this picture's MBs, so AU 1 must
 * hash the same on a fresh decoder and on a warm one, whose mb[] holds older pictures. Losing the bottom starts the
 * concealment at MB 0; losing the top starts it mid-picture, with the rows above it concealed upwards. */
static uint32_t cut_idr(vj_decoder_t *d, const char *path, int lo, int hi)
{
    size_t len;
    uint8_t *s = slurp(path, &len);
    size_t n0 = vj_au_length(s, len), n1 = vj_au_length(s + n0, len - n0);
    size_t a = slice_at(s, n0, lo), z = hi < 4 ? slice_at(s, n0, hi) : n0;
    memmove(s + a, s + z, len - z); /* AU 0 keeps SPS, PPS, SEI and the other slices; AU 1 follows it */
    n0 -= z - a;
    vj_frame_t f;
    assert(vj_decode_au(d, s, n0, &f) == VJ_ERR_STREAM);
    assert(vj_decode_au(d, s + n0, n1, &f) == VJ_FRAME);
    uint32_t h = vj_hash_i420(f.y, f.y_stride, f.u, f.v, f.uv_stride, f.crop_x, f.crop_y, f.crop_w, f.crop_h);
    free(s);
    return h;
}

static void test_conceal_history(const char *dir)
{
    static const int cut[][2] = {{2, 4}, {0, 2}, {0, 1}};
    char path[512];
    snprintf(path, sizeof path, "%s/heldout/aux_map_dark.h264", dir);
    size_t len;
    uint8_t *s = slurp(path, &len);
    vj_frame_t f;
    vj_decoder_t *warm = vj_decoder_create();
    for (size_t o = 0, au; o < len; o += au) {
        au = vj_au_length(s + o, len - o);
        assert(au && vj_decode_au(warm, s + o, au, &f) >= 0);
    }
    free(s);
    for (int k = 0; k < 3; k++) { /* each cut's pictures leave warm's mb[] old for the next IDR */
        vj_decoder_t *fresh = vj_decoder_create();
        assert(cut_idr(fresh, path, cut[k][0], cut[k][1]) == cut_idr(warm, path, cut[k][0], cut[k][1]));
        vj_decoder_destroy(fresh);
    }
    vj_decoder_destroy(warm);
}

/* Redundant slices (host/vectors, made by gen.py there): failed redundant slices can leave MBs untouched in a complete
 * picture (attack) or keep a concealed MB's corrupted-slice id (attack2), and idc 2 deblocking compares those ids. In
 * elide, a redundant slice re-decodes as a skip an MB that its primary slice changed: skip elision must still count it
 * changed, or the next picture keeps that MB's old pixels. In slerr, a slice fails after writing an MB and the same
 * picture continues: elision must stop for the rest of the picture and the picture must count as all changed.
 * Expected: the per-picture-clear decoder (m3-speed 2063e44), one entry per vj_decode_au call ("-" = VJ_ERR_STREAM);
 * every frame hashes the same with DPB memory filled 0x00, 0xA5, 0x5A or a byte pattern, so none reads uninitialised
 * memory. */
static void test_vectors(const char *vec)
{
    static const char *const want[][2] = {
        {"attack.h264", "ed86bcae ed86bcae - - - - c946cd05 c946cd05 "},
        {"attack2.h264", "ed86bcae ed86bcae - - 2c2cceb1 "},
        {"elide.h264", "ed86bcae ed86bcae - ed86bcae ed86bcae - 45efe711 45efe711 "},
        {"slerr.h264", "ed86bcae ed86bcae - ed86bcae ed86bcae - ed86bcae - 8a96d75d 8a96d75d 8a96d75d "},
    };
    for (int k = 0; k < (int)(sizeof want / sizeof *want); k++) {
        char path[512], got[256] = "";
        snprintf(path, sizeof path, "%s/%s", vec, want[k][0]);
        size_t len;
        uint8_t *s = slurp(path, &len);
        vj_decoder_t *d = vj_decoder_create();
        for (size_t o = 0, au; o < len; o += au) {
            au = vj_au_length(s + o, len - o);
            assert(au && strlen(got) < sizeof got - 10);
            vj_frame_t f;
            vj_status_t r = vj_decode_au(d, s + o, au, &f);
            if (r == VJ_FRAME)
                sprintf(got + strlen(got), "%08x ", (unsigned)vj_hash_i420(f.y, f.y_stride, f.u, f.v, f.uv_stride,
                                                                          f.crop_x, f.crop_y, f.crop_w, f.crop_h));
            else
                strcat(got, r == VJ_ERR_STREAM ? "- " : "? ");
        }
        if (strcmp(got, want[k][1]))
            fprintf(stderr, "%s: got  %s\n%*swant %s\n", want[k][0], got, (int)strlen(want[k][0]) + 2, "", want[k][1]);
        assert(!strcmp(got, want[k][1]));
        vj_decoder_destroy(d);
        free(s);
    }
}

/* f.changed_mb against the pixels: every macroblock it calls unchanged equals the same macroblock of *prev, the last
 * frame returned (NULL: none yet). Then *prev becomes f. Returns how many macroblocks it calls changed. */
static unsigned check_changed(const vj_frame_t *f, uint8_t **prev)
{
    unsigned w = f->width / 16, h = f->height / 16, n = 0;
    size_t ysz = (size_t)f->width * f->height;
    assert(f->changed_mb);
    for (unsigned i = 0; i < w * h; i++) {
        unsigned x = i % w, y = i / w;
        n += f->changed_mb[i];
        if (f->changed_mb[i])
            continue;
        assert(*prev); /* the first frame has every byte at 1 */
        for (unsigned r = 0; r < 16; r++)
            assert(!memcmp(f->y + (y * 16 + r) * f->y_stride + x * 16, *prev + (y * 16 + r) * f->width + x * 16, 16));
        for (unsigned r = 0; r < 8; r++) {
            size_t o = (y * 8 + r) * (size_t)f->uv_stride + x * 8;
            assert(!memcmp(f->u + o, *prev + ysz + o, 8) && !memcmp(f->v + o, *prev + ysz + ysz / 4 + o, 8));
        }
    }
    if (!*prev)
        *prev = malloc(ysz * 3 / 2);
    memcpy(*prev, f->y, ysz);
    memcpy(*prev + ysz, f->u, ysz / 4);
    memcpy(*prev + ysz + ysz / 4, f->v, ysz / 4);
    return n;
}

/* Skip elision and changed_mb on aux_card (a near-static card: most P macroblocks are elided): every frame hashes to
 * the golden, changed_mb holds against the pixels, the IDR is all changed and P frames are not. Every 7th frame the
 * picture numbers jump to 2 below the end of the range, so the renumbering restart runs about 40 times. */
static void test_changed_mb(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/search/aux_card.golden", dir);
    FILE *g = fopen(path, "r");
    assert(g);
    unsigned gold[400], ng = 0;
    while (ng < 400 && fscanf(g, "%x", &gold[ng]) == 1)
        ng++;
    fclose(g);
    snprintf(path, sizeof path, "%s/search/aux_card.h264", dir);
    size_t len;
    uint8_t *s = slurp(path, &len), *prev = NULL;
    vj_decoder_t *d = vj_decoder_create();
    dpbStorage_t *dpb = ((storage_t *)(void *)d)->dpb;
    unsigned n = 0, partial = 0, restarts = 0;
    for (size_t o = 0, au; o < len; o += au) {
        au = vj_au_length(s + o, len - o);
        vj_frame_t f;
        u32 before = dpb->vjPicIdx;
        vj_status_t r = vj_decode_au(d, s + o, au, &f);
        assert(r == VJ_FRAME || r == VJ_NO_FRAME);
        restarts += dpb->vjPicIdx < before;
        if (r != VJ_FRAME)
            continue;
        assert(n < ng && vj_hash_i420(f.y, f.y_stride, f.u, f.v, f.uv_stride, f.crop_x, f.crop_y, f.crop_w,
                                      f.crop_h) == gold[n]);
        unsigned c = check_changed(&f, &prev);
        assert(n || (f.idr && c == 1500u));
        partial += c < 1500u;
        if (++n % 7 == 0)
            dpb->vjPicIdx = VJ_IDX_NEVER - 2;
    }
    assert(n == ng && partial > ng / 2 && restarts >= 40);
    vj_decoder_destroy(d);
    free(prev);
    free(s);
}

/* Concealment marks every macroblock changed. aux_map_dark (4 slices per picture): picture 1 cut after 2 slices, with
 * an AUD behind them in the same call, is concealed and returned (error_mbs > 0). Picture 2, cut the same way without
 * the AUD, is an error; the next call conceals and drops it, and its own frame must be all changed against the last
 * one returned. */
static void test_changed_conceal(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/heldout/aux_map_dark.h264", dir);
    size_t total;
    uint8_t *s = slurp(path, &total), *prev = NULL;
    size_t n0 = vj_au_length(s, total), n1 = vj_au_length(s + n0, total - n0);
    size_t n2 = vj_au_length(s + n0 + n1, total - n0 - n1), cut = slice_at(s + n0, n1, 2);
    size_t n3 = vj_au_length(s + n0 + n1 + n2, total - n0 - n1 - n2);
    uint8_t *a1 = malloc(cut + 5);
    memcpy(a1, s + n0, cut);
    memcpy(a1 + cut, (const uint8_t[]){0, 0, 1, 0x09, 0xF0}, 5); /* access unit delimiter */
    vj_frame_t f;
    vj_decoder_t *d = vj_decoder_create();
    assert(vj_decode_au(d, s, n0, &f) == VJ_FRAME && check_changed(&f, &prev) == 1500u);
    assert(vj_decode_au(d, a1, cut + 5, &f) == VJ_FRAME && f.error_mbs > 0 && check_changed(&f, &prev) == 1500u);
    assert(vj_decode_au(d, s + n0 + n1, slice_at(s + n0 + n1, n2, 2), &f) == VJ_ERR_STREAM);
    assert(vj_decode_au(d, s + n0 + n1 + n2, n3, &f) == VJ_FRAME && f.error_mbs == 0);
    assert(check_changed(&f, &prev) == 1500u);
    vj_decoder_destroy(d);
    free(a1);
    free(prev);
    free(s);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../streams";
    const char *vec = argc > 2 ? argv[2] : "vectors";
    test_au_length();
    test_args();
    test_frames(dir);
    test_reuse(dir);
    test_recovery(dir);
    test_contract(dir);
    test_lost_slice(dir);
    test_resume(dir);
    test_twice(dir);
    test_conceal_history(dir);
    test_vectors(vec);
    test_changed_mb(dir);
    test_changed_conceal(dir);
    puts("test_api: all passed");
    return 0;
}
