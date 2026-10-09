# VideoJet M1: Eval Harness + Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give VideoJet a deterministic, bit-exact scorer: test streams, ffmpeg goldens, a host decoder check, and a board bench. Then measure the unmodified h264bsd baseline on the ESP32-S3.

**Architecture:**
- h264bsd (Android's baseline decoder) is vendored as an ESP-IDF component; the same source also builds on the host.
- One shared header-only hash (`vj_hash.h`) is used for ffmpeg goldens, the host decoder and the board.
- Streams live in a flash data partition. The bench app decodes them, times each frame and prints `VJ ...` lines.
- `tools/bench.py` resets the board, parses those lines and logs JSONL.

**Tech Stack:**
- Board side: ESP-IDF v5.4.2 (C), ESP32-S3.
- Host side: Python 3.10 (numpy, Pillow, pyserial in the IDF env), x264 CLI, ffmpeg bundled via `imageio-ffmpeg`, GNU make.

**Spec:** `docs/superpowers/specs/2026-10-10-videojet-design.md` (settled context in `CLAUDE.md`)

## Global Constraints

- VideoJet is a decode-only ESP-IDF component library. The bench app in `bench/` is test apparatus.
  Nothing display-, USB- or protocol-related belongs in this repo; that is the separate consumer project.
- Bench board: Waveshare ESP32-S3-Touch-LCD-3.49 **V2** on `/dev/ttyACM0`.
- ESP-IDF v5.4.2: `source "$IDF_PATH/export.sh"`. IDF python: `$IDF_PY` (has pyserial).
- No sudo. There is no system ffmpeg; use `imageio-ffmpeg`'s bundled binary.
- h264bsd source: `https://github.com/oneam/h264bsd` at commit `42bcb5d753ad86d84903354bf3c68423c28adb7b`, Apache-2.0.
  - Already cloned at `$SCRATCH/h264bsd_src/h264bsd`.
  - Copy only `src/`. Keep it unmodified except where a task says otherwise.
  - Record every local change in `components/h264bsd/README.md`.
- Always init the decoder with `h264bsdInit(dec, HANTRO_TRUE)` (no output reordering). That also caps the DPB at refs+1.
- The only frame hash is `vj_hash_i420()` from `components/vj_hash/include/vj_hash.h`, on host and board alike.
- Goldens = ffmpeg decode with `-fps_mode passthrough` (exactly one output frame per decoded frame).
- AUX stream layout: 800×480 frame, 640×172 picture at top-left, black margins, 30 fps, 300 frames.
- Partition table: `streams` data partition (subtype 0x40) at `0x310000`, size `0xCF0000`.
- Downloads are untrusted data. Put each in its own new empty directory under the scratchpad; never execute anything from it.
- The board's original flash must be backed up to `backups/board_flash_4MB.bin` before the first flash. The controller does this; check that it exists.
- Commit steps apply only once the user has approved git for this repo. Until then, skip them.

## File Structure

| Path | Responsibility |
|---|---|
| `components/h264bsd/src/*` | vendored decoder (unmodified in Tasks 1–5) |
| `components/h264bsd/{CMakeLists.txt,README.md,LICENSE.md}` | IDF component wrapper, provenance, licence |
| `components/vj_hash/include/vj_hash.h` + `CMakeLists.txt` | the one frame hash |
| `host/{Makefile,vjdec.c,vjhash_raw.c}` | host decoder → hashes; raw-YUV → hashes; `make check` gate |
| `tools/setup_host_tools.sh` | installs bundled ffmpeg |
| `tools/golden.sh` | ffmpeg decode → `.golden` |
| `tools/gen_streams.py` | synthetic AA-like streams + goldens |
| `tools/pack_streams.py` | streams → `build-host/streams.bin` container |
| `tools/flash_streams.sh` | writes the container to the `streams` partition |
| `tools/bench.py` | reset board, parse `VJ` lines, `results/bench.jsonl` |
| `bench/{CMakeLists.txt,partitions.csv,sdkconfig.defaults}` | bench IDF app; pulls the library from `../components` |
| `bench/main/{CMakeLists.txt,bench_main.c}` | board bench app (test apparatus, not part of the library) |
| `components/h264bsd/src/vj_prof.h` | per-stage cycle counters (Task 6, compiled out by default) |
| `streams/search/*`, `streams/heldout/*` | `.h264` + `.golden` pairs |
| `docs/bench-log.md` | human-readable results, one row per experiment |

---

### Task 1: Vendored decoder, shared hash, host check

> **Done (7c9c628..c2c5351).** Fix round 1 superseded these listings:
> - `vj_hash.h` gained an xorshift.
> - The gate moved to `host/check.sh`; it fails on zero, empty or orphan goldens and on decoder errors.
> - `golden.sh` uses `-vf 'format=yuv420p|yuvj420p'` and writes atomically.
> - `vjhash_raw` validates its input.
>
> The committed files are authoritative; do not copy the code below.

**Files:**
- Create: `components/h264bsd/src/*` (copy), `components/h264bsd/LICENSE.md` (copy), `components/h264bsd/README.md`
- Create: `components/vj_hash/include/vj_hash.h`, `components/vj_hash/CMakeLists.txt`
- Create: `host/Makefile`, `host/vjdec.c`, `host/vjhash_raw.c`
- Create: `tools/setup_host_tools.sh`, `tools/golden.sh`
- Create: `streams/heldout/test_640x360.h264` (copy of upstream `test/test_640x360.h264`) + `.golden`

**Interfaces:**
- Produces:
  - `uint32_t vj_hash_i420(const uint8_t *y, size_t y_stride, const uint8_t *u, const uint8_t *v, size_t c_stride, size_t x0, size_t y0, size_t w, size_t h)`
  - `build-host/vjdec <in.h264> [out.yuv]` prints one `%08x` hash per frame to stdout.
  - `build-host/vjhash_raw W H < frames.yuv` prints the same for raw I420.
  - `tools/golden.sh <in.h264> W H` writes `<in>.golden`.
  - `make -C host check` decodes every `streams/**/X.h264` that has an `X.golden` and fails on any difference.

- [ ] **Step 1: Copy the decoder and write provenance**

```bash
SRC=$SCRATCH/h264bsd_src/h264bsd
mkdir -p components/h264bsd streams/heldout
cp -r "$SRC/src" components/h264bsd/src
cp "$SRC/LICENSE.md" components/h264bsd/LICENSE.md
cp "$SRC/test/test_640x360.h264" streams/heldout/
```

`components/h264bsd/README.md`:
```markdown
# h264bsd (vendored)

Android's Hantro H.264 baseline decoder, standalone fork. Apache-2.0 (LICENSE.md).
Upstream: https://github.com/oneam/h264bsd @ 42bcb5d753ad86d84903354bf3c68423c28adb7b (src/ only).

## Local changes
None yet. Every change made for VideoJet is listed here, newest last.
```

- [ ] **Step 2: Write the shared hash**

`components/vj_hash/include/vj_hash.h`:
```c
#pragma once
/* The one frame hash in VideoJet: ffmpeg goldens, host decoder and board bench all use it.
 * FNV-1a 32 over little-endian 32-bit words of each plane row; a row's tail bytes
 * (width % 4) are folded in one at a time. Planes in order Y, U, V (I420). */
#include <stddef.h>
#include <stdint.h>

#define VJ_FNV_OFFSET 2166136261u
#define VJ_FNV_PRIME  16777619u

static inline uint32_t vj_hash_row(uint32_t h, const uint8_t *p, size_t n)
{
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 |
                     (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
        h = (h ^ w) * VJ_FNV_PRIME;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * VJ_FNV_PRIME;
    return h;
}

/* Hash the w x h window at (x0, y0) of an I420 picture. x0, y0, w, h must be even. */
static inline uint32_t vj_hash_i420(const uint8_t *y, size_t y_stride,
                                    const uint8_t *u, const uint8_t *v, size_t c_stride,
                                    size_t x0, size_t y0, size_t w, size_t h)
{
    uint32_t hv = VJ_FNV_OFFSET;
    for (size_t r = 0; r < h; r++)
        hv = vj_hash_row(hv, y + (y0 + r) * y_stride + x0, w);
    for (size_t r = 0; r < h / 2; r++)
        hv = vj_hash_row(hv, u + (y0 / 2 + r) * c_stride + x0 / 2, w / 2);
    for (size_t r = 0; r < h / 2; r++)
        hv = vj_hash_row(hv, v + (y0 / 2 + r) * c_stride + x0 / 2, w / 2);
    return hv;
}
```

`components/vj_hash/CMakeLists.txt`:
```cmake
idf_component_register(INCLUDE_DIRS "include")
```

- [ ] **Step 3: Write the raw-YUV hasher and the golden script**

`host/vjhash_raw.c`:
```c
/* vjhash_raw W H < frames.yuv : one vj_hash per raw I420 frame (ffmpeg's side of the golden). */
#include <stdio.h>
#include <stdlib.h>
#include "vj_hash.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s W H < frames.yuv\n", argv[0]);
        return 2;
    }
    size_t w = strtoul(argv[1], NULL, 10), h = strtoul(argv[2], NULL, 10);
    size_t n = w * h * 3 / 2;
    unsigned char *f = malloc(n);
    if (!f)
        return 2;
    while (fread(f, 1, n, stdin) == n) {
        const unsigned char *u = f + w * h, *v = u + (w / 2) * (h / 2);
        printf("%08x\n", (unsigned)vj_hash_i420(f, w, u, v, w / 2, 0, 0, w, h));
    }
    return 0;
}
```

`tools/setup_host_tools.sh`:
```bash
#!/usr/bin/env bash
# Host tools for VideoJet: a bundled ffmpeg (no sudo needed) for golden decodes, plus x264.
set -euo pipefail
python3 -m pip install --user --quiet imageio-ffmpeg
ff=$(python3 -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')
"$ff" -hide_banner -version | head -1
x264 --version | head -1
```

`tools/golden.sh`:
```bash
#!/usr/bin/env bash
# usage: tools/golden.sh in.h264 W H  -> writes in.golden (one vj_hash per frame of ffmpeg's decode)
set -euo pipefail
in=$1; w=$2; h=$3
root=$(cd "$(dirname "$0")/.." && pwd)
ff=$(python3 -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')
"$ff" -v error -i "$in" -fps_mode passthrough -f rawvideo -pix_fmt yuv420p - \
  | "$root/build-host/vjhash_raw" "$w" "$h" > "${in%.h264}.golden"
echo "$(wc -l < "${in%.h264}.golden") frames -> ${in%.h264}.golden"
```

- [ ] **Step 4: Write the host Makefile with the failing gate**

`host/Makefile` (recipe lines start with a TAB):
```make
CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -g -D_ERROR_PRINT
B := ../build-host
SRC := $(wildcard ../components/h264bsd/src/*.c)
INC := -I../components/h264bsd/src -I../components/vj_hash/include
HASH := ../components/vj_hash/include/vj_hash.h

all: $(B)/vjdec $(B)/vjhash_raw

$(B):
	mkdir -p $@

$(B)/vjdec: vjdec.c $(SRC) $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjdec.c $(SRC)

$(B)/vjhash_raw: vjhash_raw.c $(HASH) | $(B)
	$(CC) $(CFLAGS) $(INC) -o $@ vjhash_raw.c

# The correctness floor: every stream with a .golden decodes bit-exact.
check: all
	@fail=0; for g in $$(find ../streams -name '*.golden' | sort); do \
	  s=$${g%.golden}.h264; \
	  if $(B)/vjdec $$s 2>/dev/null | cmp -s - $$g; then echo "PASS $$s"; \
	  else echo "FAIL $$s"; fail=1; fi; \
	done; exit $$fail

.PHONY: all check
```

- [ ] **Step 5: Install tools, build the hasher, make the first golden**

Run:
```bash
chmod +x tools/*.sh && tools/setup_host_tools.sh
make -C host ../build-host/vjhash_raw
tools/golden.sh streams/heldout/test_640x360.h264 640 360
```
Expected:
- `setup_host_tools.sh` prints an ffmpeg version line (≥ 5.1, which `-fps_mode` needs) and an x264 line.
- `golden.sh` prints `N frames -> streams/heldout/test_640x360.golden` with N > 0.

- [ ] **Step 6: Run the gate, which must fail because `vjdec.c` does not exist yet**

Run: `make -C host check`
Expected: FAIL (`No rule to make target 'vjdec.c'`).

- [ ] **Step 7: Write the host decoder**

`host/vjdec.c`:
```c
/* vjdec in.h264 [out.yuv] : decode with h264bsd, print one vj_hash per output frame.
 * Mirrors the board bench loop (bench/main/bench_main.c) so host and board agree frame for frame. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#include "vj_hash.h"

static void write_crop(FILE *f, const u8 *p, u32 stride, u32 x0, u32 y0, u32 w, u32 h)
{
    for (u32 r = 0; r < h; r++)
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
    u8 *buf = malloc(size);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);
    FILE *yuv = argc > 2 ? fopen(argv[2], "wb") : NULL;

    storage_t *dec = h264bsdAlloc();
    if (!dec || h264bsdInit(dec, HANTRO_TRUE) != HANTRO_OK) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    u8 *p = buf;
    u32 left = (u32)size, frames = 0, stalls = 0;
    clock_t t0 = clock();
    while (left > 0) {
        u32 used = 0;
        u32 r = h264bsdDecode(dec, p, left, 0, &used);
        p += used;
        left -= used;
        if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR || r == H264BSD_MEMALLOC_ERROR) {
            fprintf(stderr, "decode error %u at frame %u\n", r, frames);
            return 1;
        }
        if (used == 0 && r != H264BSD_PIC_RDY && ++stalls > 2) {
            fprintf(stderr, "decoder made no progress at frame %u\n", frames);
            return 1;
        }
        if (used)
            stalls = 0;
        if (r != H264BSD_PIC_RDY)
            continue;
        u32 id, idr, nerr, crop, x0, w, y0, h;
        u8 *pic = h264bsdNextOutputPicture(dec, &id, &idr, &nerr);
        if (!pic) {
            fprintf(stderr, "PIC_RDY without a picture at frame %u\n", frames);
            return 1;
        }
        u32 W = h264bsdPicWidth(dec) * 16, H = h264bsdPicHeight(dec) * 16;
        h264bsdCroppingParams(dec, &crop, &x0, &w, &y0, &h);
        if (!crop) {
            x0 = 0;
            y0 = 0;
            w = W;
            h = H;
        }
        const u8 *U = pic + W * H, *V = U + (W / 2) * (H / 2);
        printf("%08x\n", (unsigned)vj_hash_i420(pic, W, U, V, W / 2, x0, y0, w, h));
        if (yuv) {
            write_crop(yuv, pic, W, x0, y0, w, h);
            write_crop(yuv, U, W / 2, x0 / 2, y0 / 2, w / 2, h / 2);
            write_crop(yuv, V, W / 2, x0 / 2, y0 / 2, w / 2, h / 2);
        }
        frames++;
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fprintf(stderr, "%u frames, %.1f fps on host\n", frames, s > 0 ? frames / s : 0.0);
    h264bsdShutdown(dec);
    h264bsdFree(dec);
    if (yuv)
        fclose(yuv);
    return 0;
}
```

- [ ] **Step 8: Run the gate, which must pass**

Run: `make -C host check`
Expected: `PASS ../streams/heldout/test_640x360.h264`, exit 0.

If it reports FAIL, debug before going on:
1. Run `build-host/vjdec streams/heldout/test_640x360.h264 /tmp/x.yuv`.
2. Run `"$ff" -v error -i streams/heldout/test_640x360.h264 -fps_mode passthrough -vf 'format=yuv420p|yuvj420p' -f rawvideo /tmp/ref.yuv`.
3. Run `cmp /tmp/x.yuv /tmp/ref.yuv`. The first differing byte offset divided by 345600 is the first bad frame.

- [ ] **Step 9: Commit**

```bash
git add components/h264bsd components/vj_hash host tools/setup_host_tools.sh tools/golden.sh streams/heldout/test_640x360.*
git commit -m "feat: vendor h264bsd, shared frame hash, host bit-exact check"
```

---

### Task 2: Synthetic Android-Auto-like streams

**Files:**
- Create: `tools/gen_streams.py`
- Create (generated): `streams/search/{aux_map,aux_card,full_map}.{h264,golden}`, `streams/heldout/aux_map_dark.{h264,golden}`

**Interfaces:**
- Consumes: `tools/golden.sh` and `build-host/vjhash_raw` (Task 1); `make -C host check`.
- Produces: four 300-frame 800×480 streams with goldens.

- [ ] **Step 1: Write the generator**

`tools/gen_streams.py`:
```python
#!/usr/bin/env python3
"""Synthetic Android-Auto-like H.264 test streams for the VideoJet bench.

AA's second screen arrives as 800x480 H.264 baseline at 30 fps, with the panel-sized
picture (640x172) top-left and blank margins (the Android Auto second-screen profile). These mimic it.
Usage: python3 tools/gen_streams.py   -> streams/search/*, streams/heldout/* (+ .golden each)
"""
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
W, H, PW, PH, FPS, N = 800, 480, 640, 172, 30, 300
NAMES = ["MG Rd", "Hosur Rd", "Outer Ring Rd", "Sarjapur Rd", "Bannerghatta Rd", "Old Airport Rd"]
MAP = 3000


def font(size):
    return ImageFont.load_default(size=size)


def route(t):
    """Camera path: a gentle S-curve down the map, t in [0, 1]."""
    return MAP / 2 + MAP * 0.15 * math.sin(t * 2 * math.pi), MAP * (0.2 + 0.6 * t)


def make_map(seed, dark):
    rng = np.random.default_rng(seed)
    if dark:
        bg, block, minor, major, label = (30, 34, 42), (38, 44, 54), (62, 68, 80), (96, 104, 118), (190, 190, 190)
    else:
        bg, block, minor, major, label = (236, 232, 224), (222, 218, 208), (255, 255, 255), (250, 214, 130), (70, 70, 70)
    im = Image.new("RGB", (MAP, MAP), bg)
    d = ImageDraw.Draw(im)
    for _ in range(400):
        x, y = (int(v) for v in rng.integers(0, MAP, 2))
        w, h = (int(v) for v in rng.integers(40, 260, 2))
        d.rectangle([x, y, x + w, y + h], fill=block)
    for i in range(0, MAP, 110):
        j = int(rng.integers(-30, 30))
        wd = int(rng.choice([5, 5, 9, 15]))
        col = major if wd > 9 else minor
        d.line([(i + j, 0), (i - j, MAP)], fill=col, width=wd)
        d.line([(0, i + j), (MAP, i - j)], fill=col, width=wd)
    for _ in range(10):
        a = [int(v) for v in rng.integers(0, MAP, 4)]
        d.line([(a[0], a[1]), (a[2], a[3])], fill=major, width=18)
    f = font(20)
    for _ in range(260):
        x, y = (int(v) for v in rng.integers(0, MAP - 200, 2))
        d.text((x, y), NAMES[int(rng.integers(0, len(NAMES)))], fill=label, font=f)
    d.line([route(i / 200) for i in range(201)], fill=(66, 133, 244), width=14)  # AA-blue route
    return im


def map_frames(seed, dark, vw, vh):
    """Heading-up camera following the route, ~2 px/frame, like AA's map while riding."""
    m = make_map(seed, dark)
    ink = (255, 255, 255) if dark else (20, 20, 20)
    ox, oy = vw / 2, vh * 0.7  # camera anchor inside the view
    for i in range(N):
        t = i / N * 0.35
        cx, cy = route(t)
        nx, ny = route(t + 0.002)
        th = math.atan2(nx - cx, -(ny - cy))
        c, s = math.cos(th), math.sin(th)
        # output (x, y) -> map (c*x - s*y + e, s*x + c*y + f): rotation about the anchor
        e = cx - (c * ox - s * oy)
        f = cy - (s * ox + c * oy)
        view = m.transform((vw, vh), Image.Transform.AFFINE, (c, -s, e, s, c, f),
                           resample=Image.Resampling.BILINEAR)
        vd = ImageDraw.Draw(view)
        vd.polygon([(ox, oy - 14), (ox - 10, oy + 10), (ox + 10, oy + 10)], fill=(255, 255, 255), outline=(20, 20, 20))
        vd.text((8, 6), "12:34", fill=ink, font=font(18))
        yield view


def card_frames():
    """Turn card: static except the distance (every 0.5 s), a progress bar, and a maneuver flip at 5 s."""
    for i in range(N):
        im = Image.new("RGB", (PW, PH), (24, 28, 34))
        d = ImageDraw.Draw(im)
        arrow = [(70, 30), (110, 70), (85, 70), (85, 140), (55, 140), (55, 70), (30, 70)]
        if (i // 150) % 2:
            arrow = [(140 - x, y) for x, y in arrow]
        d.polygon(arrow, fill=(255, 255, 255))
        d.text((150, 22), f"{max(0, 350 - (i // 15) * 10)} m", fill=(255, 255, 255), font=font(64))
        d.text((150, 108), NAMES[(i // 150) % len(NAMES)], fill=(170, 180, 190), font=font(30))
        d.rectangle([150, 155, 150 + int(450 * (1 - i / N)), 162], fill=(66, 133, 244))
        yield im


def boxed(views):
    """Panel picture at the top-left of an 800x480 frame, the rest black (Android Auto second-screen layout)."""
    for v in views:
        frame = Image.new("RGB", (W, H), (0, 0, 0))
        frame.paste(v, (0, 0))
        yield frame


def to_i420(rgb):
    a = np.asarray(rgb, dtype=np.float32)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    y = 16 + 0.257 * r + 0.504 * g + 0.098 * b
    u = 128 - 0.148 * r - 0.291 * g + 0.439 * b
    v = 128 + 0.439 * r - 0.368 * g - 0.071 * b

    def sub(p):
        return p.reshape(p.shape[0] // 2, 2, p.shape[1] // 2, 2).mean(axis=(1, 3))

    def q(p):
        return np.clip(np.rint(p), 0, 255).astype(np.uint8).tobytes()

    return q(y) + q(sub(u)) + q(sub(v))


def encode(frames, out, kbps, extra=()):
    """x264 tuned like a phone's HW encoder for AA: constrained baseline, 1 ref, one IDR, CBR."""
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["x264", "--demuxer", "y4m", "--profile", "baseline", "--level", "3.1", "--preset", "veryfast",
           "--tune", "zerolatency", "--threads", "1", "--keyint", "600", "--min-keyint", "600",
           "--scenecut", "0", "--ref", "1", "--bframes", "0", "--bitrate", str(kbps),
           "--vbv-maxrate", str(kbps), "--vbv-bufsize", str(kbps), "--colormatrix", "bt470bg",
           "--range", "tv", "--quiet", "--no-progress", *extra, "-o", str(out), "-"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    p.stdin.write(f"YUV4MPEG2 W{W} H{H} F{FPS}:1 Ip A1:1 C420jpeg\n".encode())
    for fr in frames:
        p.stdin.write(b"FRAME\n" + to_i420(fr))
    p.stdin.close()
    if p.wait():
        sys.exit(f"x264 failed for {out}")
    subprocess.run([str(ROOT / "tools" / "golden.sh"), str(out), str(W), str(H)], check=True)


def main():
    s, h = ROOT / "streams" / "search", ROOT / "streams" / "heldout"
    encode(boxed(map_frames(1, False, PW, PH)), s / "aux_map.h264", 2000)
    encode(boxed(card_frames()), s / "aux_card.h264", 800)
    encode(map_frames(1, False, W, H), s / "full_map.h264", 3000)
    encode(boxed(map_frames(2, True, PW, PH)), h / "aux_map_dark.h264", 2500, ("--preset", "faster", "--slices", "4"))


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Generate**

Run: `python3 tools/gen_streams.py`
Expected:
- Four `300 frames -> ...golden` lines.
- `ls -la streams/search streams/heldout` shows `.h264` files of roughly 0.9–4 MB.
- Combined size of all `.h264` files under 11 MB (`du -cb streams/*/*.h264 | tail -1`).

If `ImageFont.load_default(size=...)` raises, Pillow lacks FreeType. Fix it by using `ImageFont.truetype("DejaVuSans.ttf", size)`, after finding the font with `fc-list | grep -i dejavu`.

- [ ] **Step 3: Eyeball one frame of each stream**

Run:
```bash
ff=$(python3 -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')
for n in search/aux_map search/aux_card search/full_map heldout/aux_map_dark; do
  "$ff" -v error -y -i streams/$n.h264 -vf "select=eq(n\,150)" -frames:v 1 /tmp/vj_$(basename $n).png; done
```
Open the PNGs with the Read tool. Check:
- The aux streams show a map or card in the top-left 640×172, with the rest black.
- `full_map` is a full-frame map.

- [ ] **Step 4: Run the gate**

Run: `make -C host check`
Expected: 5 × PASS (4 new streams plus `test_640x360`), exit 0.

- [ ] **Step 5: Commit**

```bash
git add tools/gen_streams.py streams/search streams/heldout/aux_map_dark.*
git commit -m "feat: synthetic AA-like test streams with ffmpeg goldens"
```

---

### Task 3: JVT baseline conformance streams (held-out, correctness only)

**Files:**
- Create (downloaded): `streams/heldout/{BA1_Sony_D,BA2_Sony_F,BA_MW_D,BANM_MW_D,MIDR_MW_D,NRF_MW_E}.{h264,golden}`

**Interfaces:**
- Consumes: `tools/golden.sh`, `make -C host check` (Task 1).
- Produces: extra held-out streams. These add POC type 0, 3–5 reference frames, non-reference pictures, multiple IDRs and missing VUI (each is 1 slice/picture; `aux_map_dark` covers multi-slice).

- [ ] **Step 1: Download into a fresh untrusted directory**

The suite is at `https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/AVCv1/` (the root lists only subdirectories). Each stream is a `.zip` holding the bitstream plus reference YUV.

```bash
D=$SCRATCH/jvt_dl
mkdir -p "$D" && cd "$D"
for n in BA1_Sony_D BA2_Sony_F BA_MW_D BANM_MW_D MIDR_MW_D NRF_MW_E; do
  curl -fsSLO "https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/AVCv1/$n.zip" || echo "MISSING $n"
done
ls -la
```
Expected: six zip files.

If a name is MISSING, fetch the directory listing (`curl -fsSL <dir>/ | grep -o 'href="[^"]*"'`) and take the zip whose name starts with the same prefix. If none exists, leave that stream out and report it.

- [ ] **Step 2: Extract bitstreams into the repo**

```bash
cd "$D" && for z in *.zip; do mkdir -p "${z%.zip}" && unzip -o -q "$z" -d "${z%.zip}"; done
for n in BA1_Sony_D BA2_Sony_F BA_MW_D BANM_MW_D MIDR_MW_D NRF_MW_E; do
  f=$(find "$D/$n" -type f \( -iname '*.jsv' -o -iname '*.264' -o -iname '*.26l' -o -iname '*.h264' -o -iname '*.jvt' -o -iname '*.avc' \) | head -1)
  [ -n "$f" ] && cp "$f" $REPO/streams/heldout/$n.h264 || echo "NO BITSTREAM $n"
done
```

- [ ] **Step 3: Goldens**

All six are QCIF (176×144). Confirm with `"$ff" -i streams/heldout/BA1_Sony_D.h264` (stderr shows `176x144`).
```bash
cd $REPO
for n in BA1_Sony_D BA2_Sony_F BA_MW_D BANM_MW_D MIDR_MW_D NRF_MW_E; do
  [ -f streams/heldout/$n.h264 ] && tools/golden.sh streams/heldout/$n.h264 176 144; done
```

- [ ] **Step 4: Run the gate**

Run: `make -C host check`
Expected: every stream PASS.

A JVT FAIL is a finding about h264bsd, not a task failure. Do not patch the decoder in this task. Move that stream to `streams/heldout/failing/`, rename its `.golden` so the gate skips it, and report the frame index where it first diverges (Task 1 Step 8 recipe).

- [ ] **Step 5: Commit**

```bash
git add streams/heldout
git commit -m "test: JVT baseline conformance streams as held-out correctness set"
```

---

### Task 4: Board bench firmware, stream container, capture tool

**Files:**
- Create: `bench/CMakeLists.txt`, `bench/partitions.csv`, `bench/sdkconfig.defaults`
- Create: `components/h264bsd/CMakeLists.txt`
- Create: `bench/main/CMakeLists.txt`, `bench/main/bench_main.c`
- Create: `tools/pack_streams.py`, `tools/flash_streams.sh`, `tools/bench.py`
- Modify: `host/vjdec.c` (comment only: it mirrors `bench/main/bench_main.c`)
- `.gitignore` already exists (controller setup); it ignores `build/`, `build-*/`, `sdkconfig`, `results/`, `backups/`, `managed_components/`

**Interfaces:**
- Consumes: `h264bsd` component, `vj_hash.h`, and `streams/**` with goldens (Tasks 1–3).
- Produces:
  - Board output lines (exact formats):
    - `VJ start streams=<n>`
    - `VJ run=<r> set=<search|heldout> stream=<name> frames=<got>/<want> fail=<k> first_fail=<i|-1> idr_us=<t> p_mean_us=<t> p50_us=<t> p95_us=<t> max_us=<t> fps=<x.xx> int_min=<bytes> psram_min=<bytes>`
    - `VJ search_done` after the search set, `VJ done` after the held-out set.
    - `VJ ... error=<what>` on failure.
  - `tools/bench.py --label L [--set search|all]` returns 0 only if every stream passes, and appends to `results/bench.jsonl`.
  - Container `build-host/streams.bin`: `"VJS1"`, u32 count, count × `<24s5I>` entries `{name, set, data_off, data_len, frames, golden_off}`, then 16-byte-aligned blobs (golden = frames × u32 LE).

- [ ] **Step 1: Confirm the backup exists**

Run: `ls -la backups/board_flash_4MB.bin`
Expected: a 4194304-byte file. If it is missing, STOP and ask the controller. Never flash without it.

- [ ] **Step 2: Write the IDF project files**

`bench/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
# The bench is a consumer of the library components, exactly like a real app would be.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(videojet_bench)
```

`bench/partitions.csv`:
```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x300000
streams,  data, 0x40,    0x310000, 0xCF0000
```

`bench/sdkconfig.defaults`: the M1 baseline. IDF defaults plus `-O2`, 240 MHz, octal PSRAM. Cache and placement tuning are later ladder steps, so do not add them here.
```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_FREERTOS_HZ=1000
```

`components/h264bsd/CMakeLists.txt`:
```cmake
file(GLOB srcs "src/*.c")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "src")
# Vendored code: keep its warnings visible but never fatal.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -D_ERROR_PRINT)
```

`bench/main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "bench_main.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES h264bsd vj_hash esp_partition esp_timer spi_flash)
```

- [ ] **Step 3: Write the bench app**

`bench/main/bench_main.c`:
```c
/* VideoJet board bench: decode every stream in the `streams` partition, time each frame,
 * check each frame's hash against its golden, print one "VJ ..." line per stream.
 * Hashing and the per-frame yield happen outside the timed region. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#include "vj_hash.h"

#define VJ_RUNS 2

typedef struct {
    char name[24];
    uint32_t set; /* 0 = search, 1 = held-out */
    uint32_t data_off, data_len, frames, golden_off;
} vj_entry_t; /* 44 bytes, matches "<24s5I" in tools/pack_streams.py */

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void run_stream(const esp_partition_t *part, const vj_entry_t *e, int run)
{
    const char *set = e->set ? "heldout" : "search";
    uint8_t *data = heap_caps_malloc(e->data_len, MALLOC_CAP_SPIRAM);
    uint32_t *golden = heap_caps_malloc(e->frames * 4, MALLOC_CAP_SPIRAM);
    int64_t *t = heap_caps_malloc(e->frames * sizeof(int64_t), MALLOC_CAP_SPIRAM);
    storage_t *dec = NULL;
    if (!data || !golden || !t) {
        printf("VJ run=%d set=%s stream=%s error=nomem\n", run, set, e->name);
        goto out;
    }
    ESP_ERROR_CHECK(esp_partition_read(part, e->data_off, data, e->data_len));
    ESP_ERROR_CHECK(esp_partition_read(part, e->golden_off, golden, e->frames * 4));
    dec = h264bsdAlloc();
    if (!dec || h264bsdInit(dec, HANTRO_TRUE) != HANTRO_OK) {
        printf("VJ run=%d set=%s stream=%s error=init\n", run, set, e->name);
        goto out;
    }

    uint8_t *p = data;
    uint32_t left = e->data_len, n = 0, fails = 0, stalls = 0;
    int first_fail = -1;
    const char *err = NULL;
    int64_t t0 = esp_timer_get_time();
    while (left > 0 && n < e->frames) {
        uint32_t used = 0;
        uint32_t r = h264bsdDecode(dec, p, left, 0, &used);
        p += used;
        left -= used;
        if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR || r == H264BSD_MEMALLOC_ERROR) {
            err = "decode";
            break;
        }
        if (used == 0 && r != H264BSD_PIC_RDY && ++stalls > 2) {
            err = "stall";
            break;
        }
        if (used)
            stalls = 0;
        if (r != H264BSD_PIC_RDY)
            continue;
        t[n] = esp_timer_get_time() - t0; /* this frame's decode time, deblocking included */

        uint32_t id, idr, nerr, crop, x0, w, y0, h;
        uint8_t *pic = h264bsdNextOutputPicture(dec, &id, &idr, &nerr);
        if (!pic) {
            err = "nopic";
            break;
        }
        uint32_t W = h264bsdPicWidth(dec) * 16, H = h264bsdPicHeight(dec) * 16;
        h264bsdCroppingParams(dec, &crop, &x0, &w, &y0, &h);
        if (!crop) {
            x0 = 0;
            y0 = 0;
            w = W;
            h = H;
        }
        const uint8_t *U = pic + W * H, *V = U + (W / 2) * (H / 2);
        if (vj_hash_i420(pic, W, U, V, W / 2, x0, y0, w, h) != golden[n]) {
            fails++;
            if (first_fail < 0)
                first_fail = (int)n;
        }
        n++;
        vTaskDelay(1); /* let IDLE0 feed the task watchdog */
        t0 = esp_timer_get_time();
    }

    int64_t idr_us = n ? t[0] : 0, sum = 0;
    uint32_t m = n > 1 ? n - 1 : 0;
    int64_t *s = t + 1; /* P-frame times */
    for (uint32_t i = 0; i < m; i++)
        sum += s[i];
    qsort(s, m, sizeof(int64_t), cmp_i64);
    double fps = (sum + idr_us) ? n * 1e6 / (double)(sum + idr_us) : 0.0;
    printf("VJ run=%d set=%s stream=%s frames=%u/%u fail=%u first_fail=%d idr_us=%lld "
           "p_mean_us=%lld p50_us=%lld p95_us=%lld max_us=%lld fps=%.2f int_min=%u psram_min=%u%s%s\n",
           run, set, e->name, (unsigned)n, (unsigned)e->frames, (unsigned)fails, first_fail,
           (long long)idr_us, (long long)(m ? sum / m : 0), (long long)(m ? s[m / 2] : 0),
           (long long)(m ? s[(m * 95) / 100] : 0), (long long)(m ? s[m - 1] : 0), fps,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
           err ? " error=" : "", err ? err : "");
out:
    if (dec) {
        h264bsdShutdown(dec);
        h264bsdFree(dec);
    }
    free(data);
    free(golden);
    free(t);
}

static void bench_task(void *arg)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "streams");
    uint8_t hdr[8];
    if (!part || esp_partition_read(part, 0, hdr, sizeof hdr) != ESP_OK || memcmp(hdr, "VJS1", 4)) {
        printf("VJ error=no_streams\n");
        vTaskDelete(NULL);
    }
    uint32_t count;
    memcpy(&count, hdr + 4, 4);
    vj_entry_t *ents = malloc(count * sizeof(vj_entry_t));
    ESP_ERROR_CHECK(esp_partition_read(part, 8, ents, count * sizeof(vj_entry_t)));
    printf("VJ start streams=%u\n", (unsigned)count);
    for (uint32_t set = 0; set < 2; set++) {
        for (int run = 0; run < VJ_RUNS; run++)
            for (uint32_t i = 0; i < count; i++)
                if (ents[i].set == set)
                    run_stream(part, &ents[i], run);
        printf(set == 0 ? "VJ search_done\n" : "VJ done\n");
    }
    free(ents);
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 16384, NULL, 5, NULL, 0);
}
```

- [ ] **Step 4: Build**

Run:
```bash
source "$IDF_PATH/export.sh"
idf.py -C bench set-target esp32s3 && idf.py -C bench build
```
Expected: `Project build complete`.

GCC 14 may reject vendored code, for example over implicit function declarations. Apply the smallest fix (usually a missing `#include <string.h>`) and list it under "Local changes" in `components/h264bsd/README.md`.

- [ ] **Step 5: Write the container packer and flasher**

`tools/pack_streams.py`:
```python
#!/usr/bin/env python3
"""Pack streams/{search,heldout}/*.h264 + .golden into build-host/streams.bin (the `streams` partition).

Layout, little-endian: b"VJS1", u32 count, count x <24s5I> {name, set, data_off, data_len, frames,
golden_off}, then 16-byte-aligned blobs. Golden = frames x u32 (vj_hash per frame).
Must match vj_entry_t in bench/main/bench_main.c."""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PART_SIZE = 0xCF0000  # partitions.csv


def align(x):
    return (x + 15) & ~15


def main():
    items = []
    for set_id, sub in ((0, "search"), (1, "heldout")):
        for h264 in sorted((ROOT / "streams" / sub).glob("*.h264")):
            golden = h264.with_suffix(".golden")
            if not golden.exists():
                raise SystemExit(f"missing golden for {h264}")
            hashes = [int(x, 16) for x in golden.read_text().split()]
            items.append((h264.stem[:23].encode(), set_id, h264.read_bytes(),
                          struct.pack(f"<{len(hashes)}I", *hashes), len(hashes)))
    off = align(8 + 44 * len(items))
    head_len = off
    entries, blobs = b"", b""
    for name, set_id, data, gold, frames in items:
        d_off = off
        g_off = align(d_off + len(data))
        off = align(g_off + len(gold))
        entries += struct.pack("<24s5I", name, set_id, d_off, len(data), frames, g_off)
        blobs += data.ljust(g_off - d_off, b"\0") + gold.ljust(off - g_off, b"\0")
    out = (b"VJS1" + struct.pack("<I", len(items)) + entries).ljust(head_len, b"\0") + blobs
    if len(out) > PART_SIZE:
        raise SystemExit(f"streams.bin is {len(out)} bytes; the partition holds {PART_SIZE}")
    (ROOT / "build-host").mkdir(exist_ok=True)
    (ROOT / "build-host" / "streams.bin").write_bytes(out)
    print(f"{len(items)} streams, {len(out)} bytes")


if __name__ == "__main__":
    main()
```

`tools/flash_streams.sh`:
```bash
#!/usr/bin/env bash
# Write build-host/streams.bin into the `streams` partition (0x310000, partitions.csv).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
. "$HOME/esp/esp-idf-v5.4.2/export.sh" > /dev/null
esptool.py --chip esp32s3 -p "${PORT:-/dev/ttyACM0}" -b 921600 write_flash 0x310000 "$root/build-host/streams.bin"
```

- [ ] **Step 6: Write the capture tool**

`tools/bench.py`:
```python
#!/usr/bin/env python3
"""Reset the board, capture the bench's `VJ` lines, score them.

Run with the IDF python (it has pyserial):
  $IDF_PY tools/bench.py --label baseline [--set all]
Appends one JSON line to results/bench.jsonl. Exit 0 only if every stream is complete,
error-free and matches every golden hash (the correctness floor)."""
import argparse
import json
import re
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent
KV = re.compile(r"(\w+)=(\S+)")


def capture(port, stop, timeout):
    """Reset the S3 via the USB-Serial-JTAG RTS line; return (VJ lines, reached `stop`)."""
    s = serial.Serial(port, 115200, timeout=0.5)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    lines, buf, deadline = [], b"", time.time() + timeout
    while time.time() < deadline:
        try:
            buf += s.read(4096)
        except serial.SerialException:  # the port can drop while the chip resets
            time.sleep(0.5)
            s = serial.Serial(port, 115200, timeout=0.5)
            continue
        *complete, buf = buf.split(b"\n")
        for raw in complete:
            line = raw.decode(errors="replace").strip()
            if line.startswith("VJ"):
                print(line, flush=True)
                lines.append(line)
                if line == stop:
                    return lines, True
    return lines, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--set", choices=("search", "all"), default="search")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--timeout", type=float, default=1800)
    a = ap.parse_args()
    lines, finished = capture(a.port, "VJ search_done" if a.set == "search" else "VJ done", a.timeout)
    rows = [dict(KV.findall(x)) for x in lines if x.startswith("VJ ") and " stream=" in x]
    prof = [dict(KV.findall(x)) for x in lines if x.startswith("VJP ")]
    ok = finished and bool(rows) and not any("error=" in x for x in lines)
    for r in rows:
        got, want = r.get("frames", "0/1").split("/")
        ok = ok and r.get("fail") == "0" and got == want
    out = ROOT / "results" / "bench.jsonl"
    out.parent.mkdir(exist_ok=True)
    with out.open("a") as f:
        f.write(json.dumps({"label": a.label, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                            "set": a.set, "ok": ok, "rows": rows, "prof": prof}) + "\n")
    fps = {}
    for r in rows:
        fps.setdefault(r["stream"], []).append(float(r["fps"]))
    for name, v in fps.items():
        print(f"{name:24s} fps " + " ".join(f"{x:7.2f}" for x in v))
    if not finished:
        print("bench: did not reach the end marker (timeout)", file=sys.stderr)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
```

- [ ] **Step 7: Pack, flash, run**

Run:
```bash
chmod +x tools/*.sh tools/*.py
python3 tools/pack_streams.py
tools/flash_streams.sh
source "$IDF_PATH/export.sh" && idf.py -C bench -p /dev/ttyACM0 flash
$IDF_PY tools/bench.py --label m1-smoke --set all
```
Expected:
- `pack_streams.py` prints `<n> streams, <bytes>`, with bytes ≤ 13565952.
- `bench.py` prints a `VJ run=...` line per stream per run (search set ×2, then held-out ×2), every one with `fail=0`, `frames=N/N` and no `error=`. Then the per-stream fps table, then `PASS`.

If the console shows nothing, the board may have latched. Unplug it for 5 s, then retry.

- [ ] **Step 8: Commit**

```bash
git add bench components/h264bsd/CMakeLists.txt host/vjdec.c tools/pack_streams.py tools/flash_streams.sh tools/bench.py
git commit -m "feat: board bench app, stream container, capture tool"
```

---

### Task 5: Baseline numbers and noise floor

**Files:**
- Create: `docs/bench-log.md`

**Interfaces:**
- Consumes: `tools/bench.py`, the baseline firmware from Task 4.
- Produces: the incumbent's numbers, which every later candidate is compared against, and the run-to-run noise %.

- [ ] **Step 1: Two identical runs (A/A)**

Run, with the same firmware and no rebuild:
```bash
PY=$IDF_PY
$PY tools/bench.py --label baseline-A1 --set search
$PY tools/bench.py --label baseline-A2 --set search
```
Expected: `PASS` both times.

- [ ] **Step 2: Write the log**

`docs/bench-log.md`. Fill every cell from `results/bench.jsonl`, using the mean of the 4 fps values per stream (2 runs × 2 invocations):
```markdown
# Bench log

fps = frames / total decode time, decode-only (no display) unless noted. Board: Waveshare 3.49 V2,
S3 240 MHz, octal PSRAM 80 MHz. Each row = mean of 4 runs. Correctness floor: every hash passes.

| date | label | aux_map fps | aux_card fps | full_map fps | aux_map IDR ms / P p95 ms | int_min KB | notes |
|---|---|---|---|---|---|---|---|
| 2026-10-10 | baseline (unmodified h264bsd, IDF defaults + -O2) | … | … | … | … / … | … | noise A/A: ±x.x % |
```
Noise % = max over streams of |mean(A1) − mean(A2)| / mean × 100.

- [ ] **Step 3: Commit**

```bash
git add docs/bench-log.md
git commit -m "docs: M1 baseline and noise floor"
```

---

### Task 6: Per-stage profile build

**Files:**
- Create: `components/h264bsd/src/vj_prof.h`
- Modify: `components/h264bsd/src/h264bsd_slice_data.c` (calls at ~:175 and ~:185), `components/h264bsd/src/h264bsd_macroblock_layer.c` (calls at ~:1028 and ~:1123), `components/h264bsd/src/h264bsd_decoder.c` (call at ~:475)
- Modify: `components/h264bsd/CMakeLists.txt`, `components/h264bsd/README.md`, `bench/main/bench_main.c`

**Interfaces:**
- Consumes: Task 4 bench.
- Produces: in `VJ_PROFILE` builds only, after each `VJ ... stream=` line:
  `VJP run=<r> stream=<name> parse_us=<a> recon_us=<b> inter_us=<c> deblock_us=<d> other_us=<e>`
  (per-frame averages; other = mean frame time − parse − recon − deblock).
  Timed builds compile identically to before.

- [ ] **Step 1: Write the counter header**

`components/h264bsd/src/vj_prof.h`:
```c
#pragma once
/* Per-stage cycle counters, profile builds only:
 *   idf.py -C bench -B bench/build-prof -DVJ_PROFILE=1 build
 * Compiled out otherwise, so timed builds are not perturbed. CCOUNT is per core: decode on one core. */
#if defined(VJ_PROFILE) && defined(ESP_PLATFORM)
#include <stdint.h>
#include "esp_cpu.h"
enum { VJP_PARSE, VJP_RECON, VJP_INTER, VJP_DEBLOCK, VJP_N };
extern uint64_t vj_prof[VJP_N];
#define VJP_BEGIN(k) uint32_t vjp_t_##k = esp_cpu_get_cycle_count()
#define VJP_END(k) (vj_prof[(k)] += (uint32_t)(esp_cpu_get_cycle_count() - vjp_t_##k))
#else
#define VJP_BEGIN(k) ((void)0)
#define VJP_END(k) ((void)0)
#endif
```

- [ ] **Step 2: Bracket the five call sites**

Include `"vj_prof.h"` in each of the three files. Wrap each existing call statement in its own block, leaving the statement unchanged:
```c
{ VJP_BEGIN(VJP_PARSE); tmp = h264bsdDecodeMacroblockLayer(/* unchanged args */); VJP_END(VJP_PARSE); }
```
Do the same for:
- `tmp = h264bsdDecodeMacroblock(...)` → `VJP_RECON`
- both `tmp = h264bsdInterPrediction(...)` → `VJP_INTER`
- `h264bsdFilterPicture(pStorage->currImage, pStorage->mb);` → `VJP_DEBLOCK`

If a call is the body of an un-braced `if`/`else`, the wrapping block keeps it a single statement. Re-read each site after editing.

`components/h264bsd/CMakeLists.txt`, append:
```cmake
if(VJ_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC VJ_PROFILE=1)
endif()
```
Under "Local changes" in `components/h264bsd/README.md`, add:
`- VJ_PROFILE cycle counters around parse/recon/inter/deblock call sites (compiled out by default).`

- [ ] **Step 3: Report from the bench**

In `bench/main/bench_main.c`:
- Add `#include "vj_prof.h"` after the other includes.
- At file scope, add:
```c
#ifdef VJ_PROFILE
uint64_t vj_prof[VJP_N];
#endif
```
- In `run_stream`, right before `int64_t t0 = esp_timer_get_time();`, add:
```c
#ifdef VJ_PROFILE
    memset(vj_prof, 0, sizeof vj_prof);
#endif
```
- Right after the `printf("VJ run=...")` call, add:
```c
#ifdef VJ_PROFILE
    if (n) {
        const uint64_t mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        uint64_t parse = vj_prof[VJP_PARSE] / mhz / n, recon = vj_prof[VJP_RECON] / mhz / n;
        uint64_t inter = vj_prof[VJP_INTER] / mhz / n, dblk = vj_prof[VJP_DEBLOCK] / mhz / n;
        uint64_t frame = (uint64_t)((sum + idr_us) / n);
        printf("VJP run=%d stream=%s parse_us=%llu recon_us=%llu inter_us=%llu deblock_us=%llu other_us=%lld\n",
               run, e->name, parse, recon, inter, dblk, (long long)frame - (long long)(parse + recon + dblk));
    }
#endif
```

- [ ] **Step 4: Check that timed builds are unchanged**

Run: `make -C host check && idf.py -C bench build`
Expected:
- `make` prints all PASS.
- The default build succeeds and is byte-identical in behaviour: no `VJP` lines on the board.

- [ ] **Step 5: Profile on the board**

Run:
```bash
idf.py -C bench -B "$PWD/bench/build-prof" -DVJ_PROFILE=1 build && idf.py -C bench -B "$PWD/bench/build-prof" -p /dev/ttyACM0 flash
$IDF_PY tools/bench.py --label baseline-prof --set search
idf.py -C bench -p /dev/ttyACM0 flash   # restore the timed baseline firmware
```
Expected:
- `PASS`, with `VJP` lines for each search stream.
- `parse + recon + deblock + other` ≈ the stream's mean frame time.
- `inter ≤ recon`.

Add a "baseline profile" table to `docs/bench-log.md`: per-frame µs per stage for each search stream, and % of frame time.

- [ ] **Step 6: Commit**

```bash
git add components/h264bsd bench/main/bench_main.c docs/bench-log.md
git commit -m "feat: VJ_PROFILE per-stage cycle counters + baseline profile"
```
