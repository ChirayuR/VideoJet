/* The deblock worker's own contracts (host, worker on). Built by the Makefile as:
 *  test_join  (-DVJ_WORKER_JITTER_US: the worker dawdles after it claims a run): when vj_decode_au returns, on every path,
 *             no run is in flight. Remove the join at the end of vj_decode_au and this fails.
 *  test_stall (-DVJ_WORKER_STALL_MS: the worker is not scheduled before it claims): vj_decode_au takes the run back
 *             after VJ_WORKER_WAIT_MS and the output is the same as with no worker at all.
 *  test_starve (PIPE=1, -DVJ_PIPE_NOSTEP: the worker task never runs the parser): the decoding task takes it back after 50 ms,
 *             parses inline, and does not ask the worker again for the next slices (a dead worker costs 50 ms once, not per
 *             slice); the output is the same as with no worker at all.
 *  test_sleep (PIPE=1, -DVJ_PIPE_STALL_EVERY: the decoding task stalls now and then): the ring stays full, the worker sleeps
 *             and is woken by the decoding task freeing a slot; the output is the same as with no worker at all.
 *  test_ringwait (-DVJ_DBW_SEG_DELAY_US: the worker is slow per segment): the decoding task runs past the record ring and
 *             waits for room, over and over; the output is the same as with no worker at all.
 * Every build also checks that the ring paths ran: pictures the worker had to leave (the I pictures: their rows are never
 * posted) and final passes that made the records the ring did not hold.
 * All also run the thread-creation-failure path (no worker: the decoder filters in the pass). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "videojet.h"
#include "vj_hash.h"
#include "vj_worker.h"
#include "h264bsd_storage.h"
#ifdef VJ_PARSE_PIPELINE
extern unsigned vjParseTestSlices, vjParseTestArmed, vjParseTestTakebacks;
static unsigned g_sleeps, g_wakes; /* of every decoder's worker, summed */
#endif
static unsigned g_rwaits, g_lost, g_remade; /* record ring: waits for room, pictures the worker left, remade final passes */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    assert(fread(p, 1, *len, f) == *len);
    fclose(f);
    return p;
}

static int busy(vj_decoder_t *d)
{
    storage_t *s = (storage_t *)(void *)d;
    int b = s->vjDbw && h264bsdDbwBusy(s->vjDbw);
#ifdef VJ_PARSE_PIPELINE
    /* the parser has been handed back: no worker step in flight on the NAL buffer the caller may free */
    b |= __atomic_load_n(&s->vjParse->pstate, __ATOMIC_ACQUIRE) != VJ_PS_IDLE;
#endif
    return b;
}

/* Decode the first n AUs; hash of every returned frame in out[], status 0/1/2 (none / frame / error) in st[]. When
 * check, the worker must be idle after every call. cut > 0: every 5th AU is cut to that fraction of its size. */
static unsigned run(const char *path, int n, int check, int cut, uint32_t *out)
{
    size_t len;
    uint8_t *s = slurp(path, &len), *au = malloc(len);
    vj_decoder_t *d = vj_decoder_create();
    unsigned frames = 0;
    size_t o = 0;
    for (int i = 0; i < n && o < len; i++) {
        size_t l = vj_au_length(s + o, len - o), use = l;
        if (cut && i % 5 == 4)
            use = l * cut / 100;
        memcpy(au, s + o, use);
        vj_frame_t f;
        vj_status_t r = vj_decode_au(d, au, use, &f);
        if (check)
            assert(!busy(d));
        out[i] = r == VJ_FRAME ? vj_hash_i420(f.y, f.y_stride, f.u, f.v, f.uv_stride, f.crop_x, f.crop_y, f.crop_w, f.crop_h)
                               : (uint32_t)(r + 100);
        frames += r == VJ_FRAME;
        o += l;
    }
    {
        storage_t *st = (storage_t *)(void *)d;
        unsigned rw, lo, rm;

        if (st->vjDbw) {
            h264bsdDbwRingStats(st->vjDbw, &rw, &lo, &rm);
            g_rwaits += rw;
            g_lost += lo;
            g_remade += rm;
#ifdef VJ_PARSE_PIPELINE
            unsigned k, sl, wk;
            h264bsdDbwStats(st->vjDbw, &k, &sl, &wk);
            g_sleeps += sl;
            g_wakes += wk;
#endif
        }
    }
    vj_decoder_destroy(d);
    free(au);
    free(s);
    return frames;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../streams";
    const char *names[] = {"heldout/aux_map_dark.h264", "search/aux_card.h264", "search/aux_map.h264"};
    for (int k = 0; k < 3; k++)
        for (int cut = 0; cut <= 60; cut += 60) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, names[k]);
            enum { N = 24 };
            uint32_t with[N], without[N];
            vj_worker_test_fail_create = 0;
            unsigned a = run(path, N, 1, cut, with);
            vj_worker_test_fail_create = 1; /* thread creation fails: no worker, the pass filters */
            unsigned b = run(path, N, 1, cut, without);
            vj_worker_test_fail_create = 0;
            assert(a && a == b);
            assert(!memcmp(with, without, sizeof with));
        }
    assert(g_lost > 0 && g_remade > 0); /* the I pictures: the worker left them, the pass made their records */
#ifdef VJ_DBW_SEG_DELAY_US
    assert(g_rwaits > 0); /* the decoding thread really waited for ring room */
    printf("test_ringwait: ok (%u waits for ring room, %u pictures left, %u remade passes)\n", g_rwaits, g_lost, g_remade);
#elif defined(VJ_WORKER_STALL_MS)
    assert(vj_worker_test_takebacks > 0); /* the fallback really ran */
    printf("test_stall: ok (%u runs taken back)\n", vj_worker_test_takebacks);
#elif defined(VJ_PIPE_NOSTEP)
    assert(vjParseTestTakebacks > 0); /* the parser really was taken back */
    /* ... and not asked again at once: without the hold-off every slice with a worker would cost a take-back (6 decoders
     * here start with an armed slice) */
    assert(vjParseTestTakebacks <= vjParseTestSlices / 31 + 6);
    printf("test_starve: ok (%u take-backs in %u slices)\n", vjParseTestTakebacks, vjParseTestSlices);
#elif defined(VJ_PIPE_STALL_EVERY)
    assert(g_sleeps > 0 && g_wakes > 0); /* the worker slept on a full ring and was woken */
    /* a lost wake-up would leave the decoding task waiting for a record until its 50 ms take-back */
    assert(vjParseTestTakebacks <= vjParseTestArmed / 10);
    printf("test_sleep: ok (%u worker sleeps, %u wakes, %u slices armed, %u take-backs)\n", g_sleeps, g_wakes, vjParseTestArmed,
        vjParseTestTakebacks);
#else
#ifdef VJ_PARSE_PIPELINE
    assert(vjParseTestArmed > 0); /* the pipeline really ran: slices were handed to the worker */
#endif
    puts("test_join: ok");
#endif
    return 0;
}
