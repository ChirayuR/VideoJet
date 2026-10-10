# VideoJet M2: Library API + Packaging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn VideoJet into a component a consumer can depend on: one public header, internals hidden, debug switches in Kconfig, memory placement owned by the library. Move the host decoder and the board bench onto that API, and give M3's scorer exact timing and an internal-RAM axis, without changing fps.

**Architecture:**
- `components/videojet/` becomes the one library component.
  - `include/videojet.h` is the public API: one Annex-B access unit per `vj_decode_au()` call, plus a splitter.
  - `src/` holds the API implementation, the AU splitter and the allocator.
  - The vendored h264bsd moves to the private `h264bsd/`.
- `bench/` and `host/` consume only the public header.
- Acceptance for every board-touching task is the M1 rule: same PSRAM speed class, |Δ fps| ≤ 0.6 % per stream against `docs/baseline-m1.jsonl`. (Superseded after Task 1 by ruling R1 in Global Constraints: ±5 %.)

**Tech Stack:** ESP-IDF v5.4.2 (C, Kconfig, CMake), host gcc, Python 3 (pyserial in the IDF env).

**Spec:** `docs/superpowers/specs/2026-10-10-videojet-design.md` (see "Public API (M2)"). Carry-over inputs: `docs/superpowers/m1-carryover.md`.

## Global Constraints

- **Target and toolchain.** ESP32-S3 only. ESP-IDF v5.4.2: `source "$IDF_PATH/export.sh"`.
  `IDF_PY=/path/to/idf-python-env/bin/python`.
- **Board.** Waveshare 3.49 V2 on `/dev/ttyACM0`.
  - Never reflash the `streams` partition: the corpus is frozen.
  - You cannot power-cycle the board. If it latches (no `VJ start`), report BLOCKED.
- **Correctness floor, unchanged.**
  - `make -C host check` must pass on all 11 streams.
  - Every board run must PASS `tools/bench.py`, which checks frame hashes, the full stream×run grid and firmware identity.
  - The `vj_hash_i420()` header is untouched.
- **fps acceptance (ruling R1, after Task 1).** Every task that changes code the board runs must end with a `--set search` run within
  **±5 %** per stream of `docs/baseline-m1.jsonl` in the same speed class: `tools/compare.py <label> --tol 5`.
  Report the exact Δ. Tuning index 5 is "slow"; 4 and 10 are "fast".
  - Why ±5 %: Task 1 measured that code layout alone moves fps between −4.3 % and +0.7 % with identical decoder logic.
    M2 changes no decoder algorithm, so the guard has to sit outside the layout band.
  - The original ±0.6 % is void until M3 pins code placement.
- **Vendored code** may change only where a task says so. Every change goes under "Local changes" in the vendored README.
- **Xtensa trap.** `uint32_t` is `unsigned long`. Anything whose address goes to an h264bsd function is `u32`.
- **IDF build traps.**
  - IDF's `-Werror=all` beats `-Wno-error`. Use `-Wno-error=<tag>`.
  - `CONFIGURE_DEPENDS` must sit behind `if(NOT CMAKE_BUILD_EARLY_EXPANSION)`.
- **Bench timing: keep it comparable to M1.**
  - The hash runs between frames (it flushes the D-cache).
  - `vTaskDelay(1)` runs after each frame.
  - Both stay outside the timed region, exactly as in M1.
- **Flashing.**
  - Pipe esptool/idf.py output through `2>&1 | tr '\r' '\n' | grep -v '%)' | tail -15`.
  - Flash the app only: `idf.py -C bench -p /dev/ttyACM0 flash`.
  - Rebuild before flashing so the ELF on disk matches the board. `bench.py` checks the ELF SHA.
- **Git.** Branch `m2-library-api`. Commit with:
  `git -c user.name="Chirayu" -c user.email="chirayurankawat@gmail.com" commit -m "<subject>" -m "Co-Authored-By: <model> <noreply@anthropic.com>"`.
  Stage only the files the task lists.

## File Structure (end state)

| Path | Responsibility |
|---|---|
| `components/videojet/include/videojet.h` | the public API, and nothing else |
| `components/videojet/src/videojet.c` | the API over h264bsd: create/decode/destroy, frame description, profile accessor |
| `components/videojet/src/vj_au.c` | Annex-B access-unit splitter |
| `components/videojet/src/vj_alloc.{h,c}` | library-owned placement: large → PSRAM, small → internal (Task 2) |
| `components/videojet/h264bsd/*` | the vendored decoder, private (moved in Task 2) |
| `components/videojet/{CMakeLists.txt,Kconfig,idf_component.yml,README.md}` | packaging, debug switches, consumer guide |
| `components/vj_hash/` | test-apparatus hash, unchanged |
| `host/{vjdec.c,test_api.c,Makefile,check.sh}` | host decoder on the API, API unit tests, gate |
| `bench/main/bench_main.c`, `bench/sdkconfig.profile` | board bench on the API; profile overlay (Task 2) |
| `tools/compare.py`, `tools/test_compare.py` | same-class fps comparison with a baseline |
| `tools/bench.py`, `tools/test_bench.py` | scorer, gaining the new row fields (Task 3) |

---

### Task 1: Public API, AU splitter, host and bench on the API

> **Done (19c1a23..d691aff).** Fix round 1 superseded parts of these listings:
> - `vj_decode_au` takes a writable `uint8_t *au` (ruling R2).
> - The recovery reorder is in place.
> - `test_api` has `#undef NDEBUG` plus the new contract tests.
> - `compare.py` defaults to ±5 % (ruling R1).
>
> The committed files are authoritative; do not copy the code below.

**Files:**
- Create: `components/videojet/CMakeLists.txt`, `components/videojet/include/videojet.h`, `components/videojet/src/videojet.c`, `components/videojet/src/vj_au.c`
- Create: `host/test_api.c`, `tools/compare.py`, `tools/test_compare.py`
- Modify: `host/vjdec.c` (rewrite on the API), `host/Makefile`, `bench/main/bench_main.c` (port to the API), `bench/main/CMakeLists.txt`

**Interfaces:**
- Produces the API below, verbatim. Every later task and the consumer project build on it.
- `tools/compare.py LABEL [--tol 0.6] [--results PATH] [--baseline PATH]` exits 0 only if every stream in the last record labelled LABEL is within ±tol % of the same-class baseline mean.
- The bench output format stays byte-for-byte the M1 format. `tools/bench.py` is untouched in this task.

- [ ] **Step 1: Write the public header**

`components/videojet/include/videojet.h`:
```c
#pragma once
/* VideoJet: fast H.264 constrained-baseline decoding for the ESP32-S3.
 *
 * Feed one Annex-B access unit (AU) per vj_decode_au() call. A call that completes a picture returns it fully
 * decoded and deblocked, and no decode work is left running when any call returns. Pictures come out in decode
 * order (no output reordering), which is display order for baseline streams that never reorder.
 * Byte-stream sources split their buffer with vj_au_length(). One decoder per task: calls on one decoder must not
 * overlap. Frame buffers live in PSRAM (about 1.5 x width x height bytes each, refs + 1 of them). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vj_decoder vj_decoder_t;

typedef enum {
    VJ_FRAME = 1,        /* the AU completed a picture: *out describes it */
    VJ_NO_FRAME = 0,     /* the AU held no slice (parameter sets, SEI) */
    VJ_ERR_STREAM = -1,  /* bitstream error, unsupported stream, or the AU ended inside a picture */
    VJ_ERR_NOMEM = -2,   /* a buffer could not be allocated (frames need PSRAM) */
    VJ_ERR_ARG = -3,     /* NULL decoder, AU or out; empty AU */
} vj_status_t;

typedef struct {
    const uint8_t *y, *u, *v;   /* I420 planes, valid until the next vj_decode_au() or vj_decoder_destroy() */
    uint16_t y_stride, uv_stride;
    uint16_t width, height;     /* coded size, multiples of 16 */
    uint16_t crop_x, crop_y, crop_w, crop_h; /* the picture to show (SPS cropping), else the whole frame */
    bool full_range;            /* VUI video_full_range_flag: 0-255 samples instead of 16-235 */
    uint8_t matrix;             /* VUI matrix_coefficients: 1 BT.709, 5 or 6 BT.601, 2 unspecified */
    bool idr;                   /* an IDR picture: decoding can (re)start here */
    uint32_t error_mbs;         /* macroblocks concealed after a bitstream error, 0 when clean */
    const uint8_t *changed_mb;  /* NULL: unknown, treat every macroblock as changed */
} vj_frame_t;

vj_decoder_t *vj_decoder_create(void);
vj_status_t vj_decode_au(vj_decoder_t *dec, const uint8_t *au, size_t len, vj_frame_t *out);
void vj_decoder_destroy(vj_decoder_t *dec);

/* Length of the first access unit at `data`: up to the next AU's start code, `len` for the last AU, 0 when `data`
 * holds no start code. Constrained baseline without arbitrary slice order or redundant pictures. */
size_t vj_au_length(const uint8_t *data, size_t len);

/* Per-stage CPU cycles since the last take, then zeroed. False when profiling is compiled out. */
typedef struct { uint64_t parse, recon, inter, deblock; } vj_profile_t;
bool vj_profile_take(vj_profile_t *out);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the failing unit tests**

`host/test_api.c`:
```c
/* test_api STREAMS_DIR: unit checks of the public API that the stream gate cannot reach. Plain asserts. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "videojet.h"

#define LEN(a) (sizeof(a))

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    assert(b && fread(b, 1, *len, f) == *len);
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
    /* No start code, NULL, empty. */
    static const uint8_t junk[] = {1, 2, 3, 4};
    assert(vj_au_length(junk, LEN(junk)) == 0);
    assert(vj_au_length(NULL, 10) == 0);
    assert(vj_au_length(two, 0) == 0);
}

static void test_args(void)
{
    static const uint8_t b[] = {0, 0, 1, 0x67};
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

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../streams";
    test_au_length();
    test_args();
    test_frames(dir);
    puts("test_api: all passed");
    return 0;
}
```

`host/Makefile` (full replacement; recipe lines start with a TAB):
```make
CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -g -D_ERROR_PRINT
B := ../build-host
VJ := ../components/videojet
DEC := ../components/h264bsd/src
LIB_SRC := $(wildcard $(VJ)/src/*.c) $(wildcard $(DEC)/*.c)
LIB_HDR := $(wildcard $(VJ)/include/*.h) $(wildcard $(VJ)/src/*.h) $(wildcard $(DEC)/*.h)
LIB_INC := -I$(VJ)/include -I$(VJ)/src -I$(DEC)
INC := -I$(VJ)/include -I../components/vj_hash/include
HASH := ../components/vj_hash/include/vj_hash.h

all: $(B)/vjdec $(B)/vjhash_raw $(B)/test_api

$(B):
	mkdir -p $@

$(B)/vjdec: vjdec.c $(LIB_SRC) $(LIB_HDR) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) $(LIB_INC) -o $@ vjdec.c $(LIB_SRC)

$(B)/test_api: test_api.c $(LIB_SRC) $(LIB_HDR) | $(B)
	$(CC) $(CFLAGS) $(INC) $(LIB_INC) -o $@ test_api.c $(LIB_SRC)

$(B)/vjhash_raw: vjhash_raw.c $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjhash_raw.c

# The correctness floor: API unit tests, then every stream with a .golden decodes bit-exact.
check: all
	@$(B)/test_api ../streams
	@./check.sh $(B)/vjdec ../streams

.PHONY: all check
```
`vjdec.c` and `test_api.c` include only `videojet.h` (and `vj_hash.h`). `LIB_INC` is there for the library sources compiled in the same command.

Run: `make -C host check`
Expected: FAIL. The compiler reports missing `src/*.c` symbols, or `vj_au_length` undefined.

- [ ] **Step 3: Write the AU splitter**

`components/videojet/src/vj_au.c`:
```c
/* Annex-B access-unit splitting (H.264 7.4.1.2.3, for constrained baseline without arbitrary slice order or
 * redundant pictures). A new AU starts at an AUD/SPS/PPS/SEI or NAL type 14-18, or at a slice whose
 * first_mb_in_slice is 0, once the current AU already holds a slice. first_mb_in_slice is the first ue(v) after the
 * NAL header byte, and ue(v) codes 0 as a single 1 bit: "== 0" is the top bit of that byte. */
#include "videojet.h"

/* Offset of the next 00 00 01 at or after i, or len. */
static size_t next_start(const uint8_t *d, size_t len, size_t i)
{
    for (; i + 3 <= len; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            return i;
    return len;
}

size_t vj_au_length(const uint8_t *d, size_t len)
{
    if (!d)
        return 0;
    size_t sc = next_start(d, len, 0);
    if (sc == len)
        return 0;
    int seen_slice = 0;
    while (sc < len) {
        size_t hdr = sc + 3;
        if (hdr >= len)
            break;
        unsigned type = d[hdr] & 0x1f;
        int starts_au = (type >= 6 && type <= 9) || (type >= 14 && type <= 18);
        int slice = type == 1 || type == 5;
        int first_mb0 = slice && hdr + 1 < len && (d[hdr + 1] & 0x80);
        if (seen_slice && (starts_au || first_mb0))
            return sc > 0 && d[sc - 1] == 0 ? sc - 1 : sc; /* a 4-byte start code's leading zero goes with it */
        seen_slice |= slice;
        sc = next_start(d, len, hdr + 1);
    }
    return len;
}
```

- [ ] **Step 4: Write the API implementation**

`components/videojet/src/videojet.c`:
```c
/* The public API over the vendored h264bsd decoder (no output reordering: DPB = refs + 1, decode order out). */
#include <string.h>
#include "videojet.h"
#include "h264bsd_decoder.h"
#include "h264bsd_storage.h"
#include "h264bsd_util.h"
#include "vj_prof.h"

#define S(dec) ((storage_t *)(void *)(dec))

vj_decoder_t *vj_decoder_create(void)
{
    storage_t *s = h264bsdAlloc();
    if (!s)
        return NULL;
    if (h264bsdInit(s, HANTRO_TRUE) != HANTRO_OK) {
        h264bsdShutdown(s); /* h264bsdInit zeroes the storage first, so this is safe after a failed init */
        h264bsdFree(s);
        return NULL;
    }
    return (vj_decoder_t *)(void *)s;
}

void vj_decoder_destroy(vj_decoder_t *dec)
{
    if (!dec)
        return;
    h264bsdShutdown(S(dec));
    h264bsdFree(S(dec));
}

static void describe(storage_t *s, const u8 *pic, u32 idr, u32 nerr, vj_frame_t *f)
{
    u32 W = h264bsdPicWidth(s) * 16, H = h264bsdPicHeight(s) * 16;
    u32 crop, x0, w, y0, h;
    h264bsdCroppingParams(s, &crop, &x0, &w, &y0, &h);
    if (!crop) {
        x0 = 0;
        y0 = 0;
        w = W;
        h = H;
    }
    f->y = pic;
    f->u = pic + W * H;
    f->v = f->u + (W / 2) * (H / 2);
    f->y_stride = (uint16_t)W;
    f->uv_stride = (uint16_t)(W / 2);
    f->width = (uint16_t)W;
    f->height = (uint16_t)H;
    f->crop_x = (uint16_t)x0;
    f->crop_y = (uint16_t)y0;
    f->crop_w = (uint16_t)w;
    f->crop_h = (uint16_t)h;
    f->full_range = h264bsdVideoRange(s) != 0;
    f->matrix = (uint8_t)h264bsdMatrixCoefficients(s);
    f->idr = idr != 0;
    f->error_mbs = nerr;
    f->changed_mb = NULL;
}

vj_status_t vj_decode_au(vj_decoder_t *dec, const uint8_t *au, size_t len, vj_frame_t *out)
{
    if (!dec || !au || !len || !out || len > 0xffffffffu)
        return VJ_ERR_ARG;
    storage_t *s = S(dec);
    u8 *p = (u8 *)au; /* h264bsd's API is not const-correct; it never writes the stream */
    u32 left = (u32)len;
    int frames = 0;
    unsigned stalls = 0;
    while (left > 0) {
        u32 used = 0;
        u32 r = h264bsdDecode(s, p, left, 0, &used);
        int at_start = p == au;
        p += used;
        left -= used;
        if (r == H264BSD_MEMALLOC_ERROR)
            return VJ_ERR_NOMEM;
        if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR)
            return VJ_ERR_STREAM;
        if (r == H264BSD_PIC_RDY) {
            u32 id, idr, nerr;
            u8 *pic = h264bsdNextOutputPicture(s, &id, &idr, &nerr);
            if (!pic)
                return VJ_ERR_STREAM;
            /* PIC_RDY before consuming anything: h264bsd just concealed the previous AU's unfinished picture,
             * already reported as VJ_ERR_STREAM. Drop it so each call returns only its own picture. */
            if (used == 0 && at_start)
                continue;
            if (++frames > 1)
                return VJ_ERR_STREAM; /* two pictures in one access unit */
            describe(s, pic, idr, nerr, out);
        } else if (used == 0 && ++stalls > 2) {
            return VJ_ERR_STREAM;
        }
        if (used)
            stalls = 0;
    }
    if (frames)
        return VJ_FRAME;
    return s->picStarted ? VJ_ERR_STREAM : VJ_NO_FRAME; /* slices that did not finish a picture */
}

#if defined(VJ_PROFILE) && defined(ESP_PLATFORM)
uint64_t vj_prof[VJP_N]; /* the library owns its counters: consumers define nothing */

bool vj_profile_take(vj_profile_t *out)
{
    if (!out)
        return false;
    out->parse = vj_prof[VJP_PARSE];
    out->recon = vj_prof[VJP_RECON];
    out->inter = vj_prof[VJP_INTER];
    out->deblock = vj_prof[VJP_DEBLOCK];
    memset(vj_prof, 0, sizeof vj_prof);
    return true;
}
#else
bool vj_profile_take(vj_profile_t *out)
{
    (void)out;
    return false;
}
#endif
```
If `storage_t` turns out not to expose `picStarted` under that name, read `h264bsd_storage.h` and use the field h264bsd's `h264bsdDecode` itself tests (`pStorage->picStarted` in `h264bsd_decoder.c`). It is the same field.

`components/videojet/CMakeLists.txt` (Task 1 form; Task 2 folds the decoder in):
```cmake
idf_component_register(SRCS "src/videojet.c" "src/vj_au.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES h264bsd)
```

- [ ] **Step 5: Rewrite the host decoder on the API**

`host/vjdec.c` (full replacement):
```c
/* vjdec in.h264 [out.yuv] : decode through the VideoJet API, one access unit per call, and print one vj_hash per
 * frame. The same calls as the board bench (bench/main/bench_main.c), so host and board agree frame for frame. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "videojet.h"
#include "vj_hash.h"

static void write_crop(FILE *f, const uint8_t *p, size_t stride, size_t x0, size_t y0, size_t w, size_t h)
{
    for (size_t r = 0; r < h; r++)
        fwrite(p + (y0 + r) * stride + x0, 1, w, f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s in.h264 [out.yuv]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n");
        fclose(f);
        free(buf);
        return 2;
    }
    fclose(f);
    FILE *yuv = argc > 2 ? fopen(argv[2], "wb") : NULL;
    if (argc > 2 && !yuv)
        perror(argv[2]);

    int rc = 0; /* every exit goes through `out`, so nothing leaks and ASan's leak check stays usable */
    unsigned frames = 0;
    vj_decoder_t *dec = vj_decoder_create();
    if (!dec) {
        fprintf(stderr, "init failed\n");
        rc = 1;
        goto out;
    }
    clock_t t0 = clock();
    for (size_t off = 0; off < (size_t)size;) {
        size_t n = vj_au_length(buf + off, (size_t)size - off);
        if (!n) {
            fprintf(stderr, "no start code at byte %zu\n", off);
            rc = 1;
            goto out;
        }
        vj_frame_t fr;
        vj_status_t st = vj_decode_au(dec, buf + off, n, &fr);
        off += n;
        if (st < 0) {
            fprintf(stderr, "decode error %d at frame %u\n", (int)st, frames);
            rc = 1;
            goto out;
        }
        if (st == VJ_NO_FRAME)
            continue;
        printf("%08x\n", (unsigned)vj_hash_i420(fr.y, fr.y_stride, fr.u, fr.v, fr.uv_stride, fr.crop_x, fr.crop_y,
                                               fr.crop_w, fr.crop_h));
        if (yuv) {
            write_crop(yuv, fr.y, fr.y_stride, fr.crop_x, fr.crop_y, fr.crop_w, fr.crop_h);
            write_crop(yuv, fr.u, fr.uv_stride, fr.crop_x / 2, fr.crop_y / 2, fr.crop_w / 2, fr.crop_h / 2);
            write_crop(yuv, fr.v, fr.uv_stride, fr.crop_x / 2, fr.crop_y / 2, fr.crop_w / 2, fr.crop_h / 2);
        }
        frames++;
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fprintf(stderr, "%u frames, %.1f fps on host\n", frames, secs > 0 ? frames / secs : 0.0);
out:
    vj_decoder_destroy(dec);
    free(buf);
    if (yuv)
        fclose(yuv);
    return rc;
}
```

- [ ] **Step 6: Run the host gate and the sanitizers**

Run: `make -C host check`
Expected: `test_api: all passed`, 11 × PASS, `11 streams checked`.

If `test_frames`'s truncation case shows different behaviour from h264bsd (for example `VJ_NO_FRAME` instead of `VJ_ERR_STREAM`), read why in `h264bsd_decoder.c`. Change the API only if the API contract above is violated. If the test's expectation is what's wrong, fix the test and report it.

Run, in a scratch build dir:
```bash
S=$SCRATCH/asan-m2 && mkdir -p $S
make -C host B=$S CFLAGS="-std=gnu99 -O1 -g -D_ERROR_PRINT -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined" check
```
Expected: the same PASS lines, and no sanitizer report (leak detection on).

- [ ] **Step 7: Write the comparison tool and its test**

`tools/compare.py`:
```python
#!/usr/bin/env python3
"""Compare a bench run's per-stream fps with a baseline, within the same PSRAM speed class.

  python3 tools/compare.py LABEL [--tol 0.6] [--results results/bench.jsonl] [--baseline docs/baseline-m1.jsonl]

The PSRAM timing tuning index picked at boot sets a speed class (docs/bench-log.md): 5 is the common, slower one;
4 and 10 are the faster one. Only same-class numbers compare. The baseline mean per stream comes from every ok,
non-profile record of that class in the baseline file; the run's from its own rows. Exit 0 only if every stream of
the run is within ±tol % (M2's "fps unchanged" rule)."""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLASS = {5: "slow", 4: "fast", 10: "fast"}


def per_stream(records):
    """Mean fps per stream over every row of `records`."""
    acc = {}
    for rec in records:
        for row in rec["rows"]:
            acc.setdefault(row["stream"], []).append(float(row["fps"]))
    return {s: sum(v) / len(v) for s, v in acc.items()}


def compare(run, baseline, tol):
    """(ok, report lines) for one run record against the baseline records."""
    cls = CLASS.get(run.get("psram_tuning"))
    if cls is None:
        return False, [f"{run['label']}: unknown speed class (psram_tuning={run.get('psram_tuning')})"]
    base = [r for r in baseline
            if r.get("ok") and not r.get("prof") and CLASS.get(r.get("psram_tuning")) == cls]
    if not base:
        return False, [f"no {cls}-class baseline records"]
    b, now = per_stream(base), per_stream([run])
    ok, lines = True, []
    for s in sorted(now):
        if s not in b:
            ok = False
            lines.append(f"{s}: no baseline")
            continue
        d = (now[s] - b[s]) / b[s] * 100
        good = abs(d) <= tol
        ok = ok and good
        lines.append(f"{s:24s} {now[s]:8.2f} vs {b[s]:8.2f} fps ({cls})  {d:+.2f} %  {'ok' if good else 'OUT'}")
    return ok, lines


def load(path):
    return [json.loads(x) for x in Path(path).read_text().splitlines() if x.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--tol", type=float, default=0.6)
    ap.add_argument("--results", default=ROOT / "results" / "bench.jsonl")
    ap.add_argument("--baseline", default=ROOT / "docs" / "baseline-m1.jsonl")
    a = ap.parse_args()
    runs = [r for r in load(a.results) if r["label"] == a.label]
    if not runs or not runs[-1].get("ok"):
        sys.exit(f"FAIL: no passing record labelled {a.label} in {a.results}")
    ok, lines = compare(runs[-1], load(a.baseline), a.tol)
    print("\n".join(lines))
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
```

`tools/test_compare.py`:
```python
#!/usr/bin/env python3
"""Self-test for tools/compare.py: python3 tools/test_compare.py"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare  # noqa: E402


def rec(label, tuning, fps, ok=True, prof=()):
    rows = [{"stream": s, "fps": f"{v:.2f}"} for s, v in fps.items()]
    return {"label": label, "psram_tuning": tuning, "ok": ok, "rows": rows, "prof": list(prof)}


BASE = [rec("b1", 5, {"a": 10.0, "b": 20.0}), rec("b2", 5, {"a": 10.0, "b": 20.0}),
        rec("b3", 10, {"a": 10.1, "b": 20.2}), rec("bp", 5, {"a": 9.0, "b": 18.0}, prof=["x"]),
        rec("bf", 5, {"a": 1.0, "b": 1.0}, ok=False)]

assert compare.compare(rec("r", 5, {"a": 10.05, "b": 19.9}), BASE, 0.6)[0]           # within ±0.6 %
assert not compare.compare(rec("r", 5, {"a": 10.1, "b": 20.0}), BASE, 0.6)[0]        # +1 %: out
assert compare.compare(rec("r", 4, {"a": 10.1, "b": 20.2}), BASE, 0.6)[0]            # fast class vs index 10
assert not compare.compare(rec("r", 7, {"a": 10.0, "b": 20.0}), BASE, 0.6)[0]        # unknown class
assert not compare.compare(rec("r", 5, {"a": 10.0, "c": 5.0}), BASE, 0.6)[0]         # stream with no baseline
print("test_compare: all passed")
```

Run: `python3 tools/test_compare.py`
Expected: `test_compare: all passed`.

- [ ] **Step 8: Port the bench to the API**

`bench/main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "bench_main.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES videojet vj_hash esp_partition esp_timer spi_flash)
```

`bench/main/bench_main.c` changes. The output format and everything else stays as it is:
- Includes: replace `h264bsd_decoder.h`, `h264bsd_util.h` and `vj_prof.h` with `videojet.h`. Remove the `uint64_t vj_prof[...]` definition: the library owns it now.
- Header comment: "…decode every stream … through the public API, one access unit per `vj_decode_au()` call…". The VJP note says "A profile build (`-DVJ_PROFILE=1`) also prints…".
- In `run_stream`, after the partition reads, index the AUs **before** the timed loop:
```c
    /* Split the stream into access units once, outside the timed loop. */
    size_t nau = 0;
    for (size_t off = 0, n; off < e->data_len; off += n, nau++)
        if (!(n = vj_au_length(data + off, e->data_len - off)))
            break;
    uint32_t *au_off = heap_caps_malloc((nau ? nau : 1) * 2 * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
```
  Fill `au_off[2*i]` (offset) and `au_off[2*i+1]` (length) in a second identical pass. If `vj_au_length` returns 0 before the end, print the row with `error=noau`. Free `au_off` at `out:`.
- Replace `h264bsdAlloc`/`h264bsdInit` with `vj_decoder_t *dec = vj_decoder_create();` (`error=init` on NULL), and the teardown with `vj_decoder_destroy(dec)`.
- Profile: before the loop, `vj_profile_t prof; vj_profile_take(&prof);` (zeroes the counters). After the VJ row, keep the same VJP line, computed from `prof` when `vj_profile_take(&prof)` returns true:
  `parse = prof.parse / mhz / n`, and the same for recon, inter and deblock.
- The decode loop, replacing the h264bsd loop:
```c
    int64_t acc = 0; /* time of AUs since the last frame: a parameter-set-only AU counts toward the next frame */
    for (size_t i = 0; i < nau && n < e->frames; i++) {
        vj_frame_t f;
        int64_t t0 = esp_timer_get_time();
        vj_status_t st = vj_decode_au(dec, data + au_off[2 * i], au_off[2 * i + 1], &f);
        acc += esp_timer_get_time() - t0;
        if (st < 0) {
            err = st == VJ_ERR_NOMEM ? "nomem" : "decode";
            break;
        }
        if (st == VJ_NO_FRAME)
            continue;
        t[n] = acc; /* this frame's decode time, deblocking included */
        acc = 0;
        if (vj_hash_i420(f.y, f.y_stride, f.u, f.v, f.uv_stride, f.crop_x, f.crop_y, f.crop_w, f.crop_h)
            != golden[n]) {
            fails++;
            if (first_fail < 0)
                first_fail = (int)n;
        }
        n++;
        vTaskDelay(1); /* let IDLE0 feed the task watchdog: outside the timed region, as in M1 */
    }
```
  `p`, `left`, `stalls` and the `u32` comment go away; the bench no longer touches h264bsd types. Keep the stats block, the printf and the `_Static_assert` exactly as they are.

- [ ] **Step 9: Build, flash, score, compare (timed + profile)**

Run:
```bash
source "$IDF_PATH/export.sh"
idf.py -C bench build 2>&1 | tail -3
idf.py -C bench -p /dev/ttyACM0 flash 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -5
$IDF_PY tools/bench.py --label m2-t1 --set search
python3 tools/compare.py m2-t1
idf.py -C bench -B "$PWD/bench/build-prof" -DVJ_PROFILE=1 build 2>&1 | tail -3
idf.py -C bench -B "$PWD/bench/build-prof" -p /dev/ttyACM0 flash 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -5
$IDF_PY tools/bench.py --label m2-t1-prof --set search --profile --elf bench/build-prof/videojet_bench.elf
idf.py -C bench -p /dev/ttyACM0 flash 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -5   # restore the timed build
```
Expected:
- The build has 0 warnings in our code.
- `m2-t1` and `m2-t1-prof` both PASS, and `m2-t1-prof` prints VJP lines for 3 streams.
- `compare.py m2-t1` prints PASS. If it prints FAIL and only the speed class is unlucky (a class with no baseline), re-run `bench.py` (each run resets, so the class may change). Report every run.

- [ ] **Step 10: Commit**

```bash
git add components/videojet host/vjdec.c host/test_api.c host/Makefile bench/main/bench_main.c bench/main/CMakeLists.txt tools/compare.py tools/test_compare.py
git commit -m "feat: public VideoJet API (one AU per call) + AU splitter; host and bench on the API"
```

---

### Task 2: One private component: Kconfig switches, library-owned placement, consumer guide

**Files:**
- Move: `components/h264bsd/src/*` → `components/videojet/h264bsd/`; `components/h264bsd/{README.md,LICENSE.md}` → `components/videojet/h264bsd/`. Use `git mv`, then delete `components/h264bsd/`.
- Modify: `components/videojet/CMakeLists.txt`, `components/videojet/h264bsd/{vj_prof.h,h264bsd_util.h,h264bsd_decoder.c,README.md}`, `components/videojet/src/videojet.c`, `host/Makefile`
- Create: `components/videojet/Kconfig`, `components/videojet/src/vj_alloc.h`, `components/videojet/src/vj_alloc.c`, `components/videojet/idf_component.yml`, `components/videojet/README.md`, `bench/sdkconfig.profile`
- Modify docs: `CLAUDE.md` (profile command), `docs/bench-log.md` (profile command)

**Interfaces:**
- Consumes: Task 1's API, unchanged.
- Produces:
  - Consumers see `include/` only.
  - `CONFIG_VJ_PROFILE` and `CONFIG_VJ_ERROR_PRINT` (both off by default) replace `-DVJ_PROFILE` and the hard-wired `-D_ERROR_PRINT`.
  - The profile build becomes:
    `idf.py -C bench -B "$PWD/bench/build-prof" -DSDKCONFIG="$PWD/bench/build-prof/sdkconfig" -DSDKCONFIG_DEFAULTS="$PWD/bench/sdkconfig.defaults;$PWD/bench/sdkconfig.profile" build`

- [ ] **Step 1: Move the vendored decoder**

```bash
mkdir -p components/videojet/h264bsd
git mv components/h264bsd/src/* components/videojet/h264bsd/
git mv components/h264bsd/README.md components/h264bsd/LICENSE.md components/videojet/h264bsd/
git rm -q components/h264bsd/CMakeLists.txt && rmdir components/h264bsd/src components/h264bsd
```

- [ ] **Step 2: One component, private internals, Kconfig**

`components/videojet/CMakeLists.txt`:
```cmake
# One component. Consumers see include/videojet.h only; the vendored decoder and our sources stay private.
# CONFIGURE_DEPENDS re-globs on the next build, but IDF's requirements scan runs this file in script mode,
# where it is an error, so it is guarded.
if(CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB dec_srcs "h264bsd/*.c")
else()
    file(GLOB dec_srcs CONFIGURE_DEPENDS "h264bsd/*.c")
endif()
idf_component_register(SRCS "src/videojet.c" "src/vj_au.c" "src/vj_alloc.c" ${dec_srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src" "h264bsd"
                       PRIV_REQUIRES heap)
# IDF's -Werror=all beats a bare -Wno-error, so this is a point fix for a known GCC false positive in
# h264bsd_pic_order_cnt.c (both variables are set and read under the same absFrameNum > 0 test). Other tags may
# need the same as optimisation flags change.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=maybe-uninitialized)
if(CONFIG_VJ_ERROR_PRINT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE _ERROR_PRINT)
endif()
```

`components/videojet/Kconfig`:
```
menu "VideoJet"

config VJ_PROFILE
    bool "Per-stage cycle counters (profiling builds only)"
    default n
    help
        Brackets the decoder's parse / reconstruct / inter / deblock stages with CCOUNT reads, readable through
        vj_profile_take(). Adds about 0.5 % to frame time and moves code layout: never use it for timed scores.

config VJ_ERROR_PRINT
    bool "Print h264bsd bitstream errors to stderr"
    default n
    help
        Compiles in the vendored decoder's ~60 error prints. They print from the decoding task.

endmenu
```

In `components/videojet/h264bsd/vj_prof.h`, replace the guard line `#if defined(VJ_PROFILE) && defined(ESP_PLATFORM)` and the comment above it with:
```c
/* Per-stage cycle counters, CONFIG_VJ_PROFILE builds only (bench: see bench/sdkconfig.profile).
 * Compiled out otherwise, so timed builds are not perturbed. CCOUNT is per core: decode on one core.
 * VJP_BEGIN declares a variable, so use it only at the top of a { } block. The bracket in
 * h264bsd_macroblock_layer.c under H264DEC_OMXDL is dead code: no build defines that. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(ESP_PLATFORM) && defined(CONFIG_VJ_PROFILE) /* a Kconfig bool that is off is simply undefined */
```
In `components/videojet/src/videojet.c`, change `#if defined(VJ_PROFILE) && defined(ESP_PLATFORM)` to `#if defined(ESP_PLATFORM) && defined(CONFIG_VJ_PROFILE)`. Add `#ifdef ESP_PLATFORM` / `#include "sdkconfig.h"` / `#endif` near its includes.

`bench/sdkconfig.profile`:
```
# Overlay for profile builds only. Use it with -DSDKCONFIG_DEFAULTS="...sdkconfig.defaults;...sdkconfig.profile"
# and -DSDKCONFIG=<build-prof>/sdkconfig, so the timed bench/sdkconfig never sees it.
CONFIG_VJ_PROFILE=y
```

- [ ] **Step 3: Library-owned memory placement**

`components/videojet/src/vj_alloc.h`:
```c
#pragma once
/* The library decides where its buffers go, whatever the consumer's malloc settings. Above VJ_INTERNAL_MAX bytes
 * (frames, per-MB state) it is PSRAM first; below it, internal first; either falls back to the other. 16 KiB is the
 * IDF default CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL that M1 was measured with, so placement is unchanged. */
#include <stddef.h>
#define VJ_INTERNAL_MAX 16384
void *vj_alloc(size_t n);
```

`components/videojet/src/vj_alloc.c`:
```c
#include <stdlib.h>
#include "vj_alloc.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

void *vj_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    uint32_t first = (n > VJ_INTERNAL_MAX ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
    return heap_caps_malloc_prefer(n, 2, first, MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}
```

Vendored local changes, so all of h264bsd's live allocations go through `vj_alloc` (`free()` releases either heap):
- `h264bsd/h264bsd_util.h`: in the `ALLOCATE` macro, replace `malloc(` with `vj_alloc(`, and add `#include "vj_alloc.h"` beside its other includes.
- `h264bsd/h264bsd_decoder.c`: in `h264bsdInit`, replace `(macroblockLayer_t*)malloc(size)` with `(macroblockLayer_t*)vj_alloc(size)`. In `h264bsdAlloc`, replace `(storage_t*)malloc(sizeof(storage_t))` with `(storage_t*)vj_alloc(sizeof(storage_t))`. The `conversionBuffer` mallocs serve the unused RGBA converters; leave them.

Under "Local changes" in `h264bsd/README.md`, add:
- Moved into `components/videojet/h264bsd/`.
- Allocations go through `vj_alloc()`.
- `vj_prof.h` keys on `CONFIG_VJ_PROFILE`.

`host/Makefile`: set `DEC := ../components/videojet/h264bsd`. `LIB_SRC` and `LIB_HDR` then pick everything up. On the host, `vj_alloc` is plain `malloc`.

- [ ] **Step 4: Packaging files**

`components/videojet/idf_component.yml`:
```yaml
version: "0.1.0"
description: "Fast H.264 constrained-baseline decoding for the ESP32-S3 (h264bsd core, one access unit per call)"
targets:
  - esp32s3
dependencies:
  idf: ">=5.4"
```

`components/videojet/README.md`: a consumer guide, one screen. It covers:
1. Adding the component: `EXTRA_COMPONENT_DIRS` pointing at `components/`, or a git dependency with `path: components/videojet`.
2. A 15-line usage example: create, a loop of `vj_au_length` + `vj_decode_au`, using `f.y/u/v` + crop, destroy.
3. Requirements:
   - PSRAM enabled. 800×480 needs 2 × 576 KB of frames plus about 0.3 MB of per-MB state.
   - Recommended: 240 MHz, octal PSRAM 80 MHz, `-O2`.
   - The speed reference is `docs/bench-log.md`.
4. Contract:
   - one AU per call
   - decode-order output
   - one decoder per task
   - frame pointers valid until the next call
   - `changed_mb` NULL for now
   - after any `VJ_ERR_*`, request a keyframe and treat frames as unreliable until one with `f.idr` arrives (a lost reference can leave
     `error_mbs` at 0); `error_mbs` counts concealed macroblocks in a returned frame and is rarely non-zero
5. Kconfig options.
6. Licensing: the h264bsd core is Apache-2.0 (`h264bsd/LICENSE.md`). The licence for VideoJet's own code is to be chosen by the owner before publishing.

- [ ] **Step 4b: Folded-in items (Task 1 review carry-overs)**

- **Vendored param-set status.** `h264bsd/h264bsd_decoder.c` (~:283 and ~:298) ignores the return value of
  `h264bsdStoreSeqParamSet` / `h264bsdStorePicParamSet`.
  - Result today: when the SPS/PPS slot allocation fails, the parsed SPS's VUI (952 B) leaks, and the AU returns
    `VJ_NO_FRAME` instead of `VJ_ERR_NOMEM`.
  - Fix: propagate a MEMORY_ALLOCATION_ERROR as `H264BSD_MEMALLOC_ERROR`, freeing what the failed store did not take ownership of.
  - Record it under "Local changes".
  - Prove it with a malloc fault-injection run on the host: a scratch `vj_alloc` that fails at index N, swept across the first ~80
    allocations. Expect no LSan leak and the right status.
- **Header.** On `VJ_ERR_STREAM` in `include/videojet.h`, add: "after any error, frames may be visibly wrong (a lost reference
  can leave `error_mbs` at 0) until the next IDR (`f.idr`): request a keyframe and wait for it."
- **Test buffer.** In `host/test_api.c` `resume_run`, size `big` by the larger of AU3 and AU4, so a regenerated fixture cannot
  overflow it.

- [ ] **Step 5: Verify the host side**

Run: `make -C host check`
Expected: `test_api: all passed`, 11 × PASS.

Run the ASan + UBSan + leak build from Task 1 Step 6. Expected: clean.

- [ ] **Step 6: Verify the encapsulation, then build, score and compare**

Encapsulation check: in a scratch copy of the bench, add `#include "h264bsd_decoder.h"` to `bench_main.c` and run `idf.py -C <copy>/bench build`. It must fail with "No such file". Report the error line, and do not commit that copy.

Run:
```bash
idf.py -C bench reconfigure build 2>&1 | tail -3
idf.py -C bench -p /dev/ttyACM0 flash 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -5
$IDF_PY tools/bench.py --label m2-t2 --set search && python3 tools/compare.py m2-t2
rm -rf bench/build-prof
idf.py -C bench -B "$PWD/bench/build-prof" -DSDKCONFIG="$PWD/bench/build-prof/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$PWD/bench/sdkconfig.defaults;$PWD/bench/sdkconfig.profile" build 2>&1 | tail -3
idf.py -C bench -B "$PWD/bench/build-prof" -p /dev/ttyACM0 flash 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -5
$IDF_PY tools/bench.py --label m2-t2-prof --set search --profile --elf bench/build-prof/videojet_bench.elf
idf.py -C bench -p /dev/ttyACM0 flash 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -5   # restore the timed build
grep -c CONFIG_VJ_PROFILE=y bench/sdkconfig bench/build-prof/sdkconfig
```
Expected:
- 0 warnings in our code.
- `m2-t2` PASS, and `compare.py` PASS: placement and `_ERROR_PRINT`-off move layout, which must stay within ±5 % (ruling R1, the default).
- `m2-t2-prof` PASS with VJP lines.
- The final grep prints `bench/sdkconfig:0` and `bench/build-prof/sdkconfig:1`: the profile overlay never leaks into the timed config.

Update the profile command in `CLAUDE.md` and `docs/bench-log.md` to the `-DSDKCONFIG`/`-DSDKCONFIG_DEFAULTS` form above.

- [ ] **Step 7: Commit**

```bash
git add -A components/videojet components/h264bsd host/Makefile bench/sdkconfig.profile CLAUDE.md docs/bench-log.md
git commit -m "feat: one VideoJet component — private decoder, Kconfig switches, library-owned placement, consumer guide"
```
`git add -A` is limited here to the listed paths. It records the moves and the deleted `components/h264bsd`.

---

### Task 3: Bench measures what M3 needs (exact time, internal RAM, strict end-of-stream)

**Files:**
- Modify: `bench/main/bench_main.c`, `tools/bench.py`, `tools/test_bench.py`

**Interfaces:**
- Consumes: Task 1/2 API and bench.
- Produces: VJ rows gain three fields at the end, before any ` error=`:
  - `total_us=<exact sum of timed µs>`
  - `int_used=<bytes>` (the decoder's internal-RAM high-water mark over this stream)
  - `psram_used=<bytes>`
  - `fps` is now computed from `total_us`. The value is identical: it is the same sum M1 used.
- New error values:
  - `error=read`: partition read failed.
  - `error=extra_frames`: the stream decodes to more pictures than its golden.
- New marker line: `VJ error=container` (count or offsets out of bounds). `bench.py` already ends the capture on it.

- [ ] **Step 1: Failing scorer tests**

In `tools/test_bench.py`, update the canonical good row to carry `total_us`, `int_used` and `psram_used`. Take the values from one real M2 row, or synthesize them consistently. Then add:
- (a) A row missing any of the three fields → FAIL.
- (b) A non-integer value in any of them → FAIL.
- (c) Old M1-format rows (no new fields) → FAIL with a reason naming the missing field.

Run: `python3 tools/test_bench.py`. Expected: the new cases fail.

- [ ] **Step 2: Scorer accepts the new format**

In `tools/bench.py`, add `total_us`, `int_used` and `psram_used` to the INT fields in `FIELDS`, so they are required and plain integers. Update the module docstring's row-format line.

Run: `python3 tools/test_bench.py`. Expected: all pass.

- [ ] **Step 3: Bench changes**

In `bench/main/bench_main.c`:

1. **Exact time.** `int64_t total = 0;` accumulate every timed µs (including parameter-set-only AUs). Print `total_us=%lld`. Compute `fps = n * 1e6 / total` (0 when total is 0).

2. **Internal and PSRAM high-water marks for the decoder alone.** After the bench's own buffers are allocated and the AUs indexed, take:
```c
    size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    heap_caps_monitor_local_minimum_free_size_start();
```
   Right before `vj_decoder_destroy`:
```c
    size_t int_used = int0 - heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_used = ps0 - heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    heap_caps_monitor_local_minimum_free_size_stop();
```
   Print `int_used=%u psram_used=%u` with `(unsigned)` casts. On an early error path that skips the start, print 0 for both and make sure the stop is not called unmatched.

3. **Error rows, not aborts.**
   - Replace each `ESP_ERROR_CHECK(esp_partition_read(...))` in `run_stream` with a check that prints the row with `error=read` and goes to `out`.
   - In `bench_task`, bound `count` by `(part->size - 8) / sizeof(vj_entry_t)`, and every entry's `data_off + data_len` and `golden_off + frames * 4` by `part->size`. Use 64-bit arithmetic. Any violation prints `VJ error=container` and deletes the task.
   - Check the `malloc` of `ents`, and the header and entry reads.

4. **Strict end of stream.** Drop `&& n < e->frames` from the loop condition and decode every AU. A `VJ_FRAME` with `n == e->frames` sets `err = "extra_frames"` and breaks. This prevents writing `t[n]` out of bounds. `frames=n/e->frames` still reports a short stream.

- [ ] **Step 4: Build, flash, score, compare**

```bash
idf.py -C bench build 2>&1 | tail -3 && idf.py -C bench -p /dev/ttyACM0 flash 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -5
$IDF_PY tools/bench.py --label m2-t3 --set all && $IDF_PY tools/bench.py --label m2-t3-search --set search
python3 tools/compare.py m2-t3-search
```
Expected:
- Both PASS. `--set all` exercises the held-out and JVT streams through the strict end-of-stream path.
- `compare.py` PASS.
- `int_used` is constant across runs of a stream and plausible: a few KB for the h264bsd `storage_t` / `mbLayer` and DPB headers. The frames are in PSRAM.
- `psram_used` ≈ (refs + 1) × 576 KB + per-MB state at 800×480.

Report both columns per stream.

- [ ] **Step 5: Commit**

```bash
git add bench/main/bench_main.c tools/bench.py tools/test_bench.py
git commit -m "feat: bench reports exact total_us + decoder internal/PSRAM use, error rows, strict end-of-stream"
```

---

### Task 4: M2 numbers, footprint, docs

**Files:**
- Modify: `docs/bench-log.md`, `CLAUDE.md`, `docs/superpowers/m1-carryover.md`, `streams/heldout/README.md`

**Interfaces:**
- Consumes: Tasks 1-3 records in `results/bench.jsonl`. Every number must come from there, or from a command whose output is shown in the report.
- Produces: the M2 section of the bench log, which is M3's cost-axis baseline.

- [ ] **Step 1: Library footprint**

Run: `$IDF_PY -m esp_idf_size --archives bench/build/videojet_bench.map 2>/dev/null | head -40`. If that form fails, use `idf.py -C bench size-components`.
Record from the output the `libvideojet.a` row: flash code, flash rodata, IRAM, DRAM/.bss.

- [ ] **Step 2: bench-log M2 section**

Add a section "M2 (library API)" to `docs/bench-log.md` containing:
- A table: for each search stream, M1 baseline fps (same class), M2 fps, Δ %, and the class. Take it from `compare.py m2-t3-search` and the earlier `m2-t1`/`m2-t2` runs.
- `int_used` and `psram_used` per stream, from `m2-t3` (search and held-out), plus the static footprint. This is M3's cost axis baseline (spec budget ≤ 100 KB internal).
- The R14 corrections from `docs/superpowers/m1-carryover.md` "Parked M1 doc nits":
  - Qualify "98/100 rows" and "84 search rows" as "through `timed-restore-check`".
  - Name the binaries of `baseline-prof` and `timed-restore-check` if the record has `elf_sha`. Otherwise say they are not recorded.
  - Fix 0.002 → 0.0022 %.
- In `streams/heldout/README.md`, say that the feature list describes the set as a whole: refs 1/5/4/1/4/3; non-reference pictures only in NRF_MW_E; single IDR in BA1/BA2.

- [ ] **Step 3: CLAUDE.md and carry-over**

`CLAUDE.md`:
- Add the API one-liner (`vj_decode_au` takes one AU) and the component path.
- Update Status (M2 done, with numbers).
- Update the board loop / profile commands if anything changed.

`m1-carryover.md`: mark the M2 items closed, each with the commit that closed it. Leave the M3 list intact.

- [ ] **Step 4: Commit**

```bash
git add docs/bench-log.md CLAUDE.md docs/superpowers/m1-carryover.md streams/heldout/README.md
git commit -m "docs: M2 numbers, library footprint, internal/PSRAM baselines, carry-over status"
```
