# VideoJet

Fast H.264 constrained-baseline decoding for the ESP32-S3 (ESP-IDF 5.4 or newer): one Annex-B access unit (AU) in, one
decoded I420 picture out. The decoder inside is a private, vendored copy of h264bsd; you see `include/videojet.h` only.

## 1. Add it
- Before `project()` in your top-level `CMakeLists.txt`: `set(EXTRA_COMPONENT_DIRS /path/to/VideoJet/components)`, then
  `PRIV_REQUIRES videojet` in your component.
- Or a git dependency in your component's `idf_component.yml`: `videojet: {git: <repo url>, path: components/videojet}`.

## 2. Use it
```c
#include "videojet.h"

vj_decoder_t *dec = vj_decoder_create();
if (!dec)                                                /* out of memory */
    return;
bool need_idr = true;                                    /* frames are unreliable until an IDR arrives */
for (size_t off = 0, n; off < len; off += n) {           /* buf: the byte stream, in writable RAM */
    n = vj_au_length(buf + off, len - off);              /* one access unit; 0: no start code left */
    if (n == 0)
        break;
    vj_frame_t f;
    vj_status_t st = vj_decode_au(dec, buf + off, n, &f);
    if (st < 0) {                                        /* VJ_ERR_*: see the contract */
        need_idr = true;
        request_keyframe();
        continue;
    }
    if (st == VJ_FRAME && f.idr)
        need_idr = false;
    if (st == VJ_FRAME && f.error_mbs) {                 /* concealed macroblocks, even in an IDR: a keyframe cue too */
        need_idr = true;
        request_keyframe();
    }
    if (st == VJ_FRAME && !need_idr) /* the cropped picture; crop offsets are even, so the chroma offsets are exact */
        show(f.y + f.crop_y * f.y_stride + f.crop_x, f.u + (f.crop_y / 2) * f.uv_stride + f.crop_x / 2,
             f.v + (f.crop_y / 2) * f.uv_stride + f.crop_x / 2, f.y_stride, f.uv_stride, f.crop_w, f.crop_h);
}
vj_decoder_destroy(dec);
```

## 3. Requirements
- PSRAM in the heap: `CONFIG_SPIRAM=y` with `CONFIG_SPIRAM_USE_MALLOC` (the default) or `CONFIG_SPIRAM_USE_CAPS_ALLOC`.
  `CONFIG_SPIRAM_USE_MEMMAP` leaves the library no PSRAM to allocate from, so the frames fail with `VJ_ERR_NOMEM`.
- Memory at 800x480, measured on the board with the bench configuration (slow PSRAM class). Internal RAM is the library's
  static part (`CONFIG_VJ_IRAM`, the default: hot code in IRAM, tables and the string literals that sit with them in DRAM,
  from the linker map by `tools/ram_static.py`) plus the heap high-water mark of one decoder (`int_used` in the bench rows,
  which includes the deblock worker task's stack), against the 100,000 B budget:

  | configuration | static | heap | internal total | PSRAM |
  |---|---|---|---|---|
  | `CONFIG_VJ_DEBLOCK_WORKER` (the library default is off; pipeline off) | 57,158 B | 42,392 B | **99,550 B** | 1,625,620 B |
  | + `CONFIG_VJ_PARSE_PIPELINE` | 58,881 B | 36,952 B | **95,833 B** | 1,644,056 B |

  The NAL extraction (`h264bsd_byte_stream`: one call per NAL, 645 B of code that runs from the I-cache) is mapped to flash
  with the other once-per-NAL-or-less code (`linker.lf`), which took 325 B off both static figures (328 B in the image, alignment
  included); the heap figures did not change. The worker off was not remeasured since the worker tip (93,488 B). The PSRAM holds the frames (about
  1.5 x width x height bytes each, refs + 1 of them, so 2 for a one-reference stream) and the per-macroblock state
  (1500 MBs x 216 B). Each extra reference frame adds 589,828 B. IDF's heap rounds every block up to its size class, so
  the PSRAM figure is a little above the sum of the structure sizes. Internal RAM includes 17 B per macroblock:
  - 5 B for skip elision and `changed_mb` (7,500 B here; each of its two blocks moves to PSRAM above 16 KiB);
  - 12 B of deblock records (18,000 B here), which are internal first whatever their size, so they scale with resolution:
    about 43 KB at 720p and 98 KB at 1080p. With `CONFIG_VJ_PARSE_PIPELINE` they are PSRAM first instead (the ring needs
    their room; it costs the decoding task about 4 % on a static picture).
  The decode task's stack is not counted.
- Stack: decoding runs on the stack of the task that calls `vj_decode_au()`. The stack it needs has not been measured; only a
  16 KiB task stack (the bench's) has been tested.
- The library places its own buffers, whatever your `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`: above 16 KiB PSRAM first, below it
  internal RAM first, each falling back to the other. The exceptions are the deblock records, which are internal RAM first at
  any size (PSRAM first with the parse pipeline), and the deblock worker's marks and undo log, PSRAM first. It asks for the
  same memory as IDF's `malloc`, so it never takes the
  internal DMA reserve (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`).

## 4. Recommended config
The bench's `sdkconfig.defaults` is the reference. At M3 step 0 the first three settings below (64 B D-cache line, 64 KB
D-cache, decoder code in internal RAM) took aux_card / aux_map / full_map (800x480) from 17.11 / 9.67 / 5.58 fps (M2: code in
flash, 16 KB I-cache, 32 KB D-cache with 32 B lines) to 22.21 / 12.72 / 7.68 (+30 / +32 / +38 %), with the tables still in
flash then. Numbers and records: `docs/bench-log.md`, M3 step 0 and M3 frontier. D-cache autoload, the last bullet, is optional,
off by default and came later.
- 240 MHz CPU, octal PSRAM at 80 MHz, `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`).
- **D-cache line 64 B** (`CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`): the largest lever, and it costs no RAM. At 32 KB with the
  code in IRAM it gained +19 / +11 / +7 %.
- **D-cache 64 KB** (`CONFIG_ESP32S3_DATA_CACHE_64KB=y`): another +8 / +6 / +7 % on top of the 64 B line, for 32 KB of
  internal RAM (31,996 B less heap, measured).
- **Decoder code and tables in internal RAM** (`CONFIG_VJ_IRAM=y`, the default): 49,455 B IRAM + 7,695 B DRAM (the bench
  config with the deblock worker and the PIE kernels, without the pipeline; the string literals that sit in DRAM are in the
  DRAM figure; `CONFIG_VJ_ERROR_PRINT=y` adds ~0.8 KB of strings). The code in IRAM gained +1 / +11 / +21 % over flash code with
  the default 16 KB I-cache (step 0), and with it moving the code changed fps by at most 0.11 %. The tables in DRAM gained
  another +0.7 / +1.4 / +2.0 % on top of that (in flash they cost up to ~2,350 cache misses per frame).
- The small-RAM alternative, 16 KB of internal RAM instead of 57,150 B: `CONFIG_VJ_IRAM=n` with
  `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y`. At step 0 it stayed within 1 % of code-in-IRAM (22.17 / 12.60 / 7.63; flash code
  has no layout pin), but it keeps the tables in flash too, so expect it about 1 / 2 / 3 % below the default (estimate).
- With 64 B lines, a PSRAM buffer that DMA reads or writes needs 64 B alignment in address and size
  (`CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE`; for example `heap_caps_aligned_alloc(64, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)`
  with `n` a multiple of 64). The library does no DMA, and its own buffers, `f.y`, `f.u` and `f.v` included, are not
  guaranteed line-aligned: do not DMA from the decoded picture, convert it into your own buffer first.
- **D-cache autoload** (optional, `CONFIG_VJ_DCACHE_AUTOLOAD=y`, default n; the bench sets it): +9.35 / +5.05 / +4.54 % on top of the
  settings above, but it is a global cache setting with hazards for code that does its own cache work: read the next
  subsection before enabling it.

### D-cache autoload (optional)
After a cache miss the hardware also fetches the next cache lines. With `CONFIG_VJ_DCACHE_AUTOLOAD=y` every `vj_decode_au()` arms
it over the span of the decoder's own PSRAM blocks (the frames and the per-macroblock state: 1,493,568 B at 800x480, see
`docs/bench-log.md`), from the second decode call on. It only decides which lines are cached, so the decoder's own output is bit-exact.
- **Scored:** T11, record `m3-autoload`, at the T6d tip, slow PSRAM class, 800x480, **64 KB D-cache with 64 B lines**, size 1.
  aux_card / aux_map / full_map 73.87 / 17.84 / 8.82 -> 80.77 / 18.74 / 9.22 fps, **+9.35 / +5.05 / +4.54 %**.
- **Per size, from the earlier autoload experiment** (not re-scored; its region was the whole 8 MB PSRAM mapping and it was armed
  before the first picture; same cache config):

  | `CONFIG_VJ_DCACHE_AUTOLOAD_SIZE` | aux_card | aux_map | full_map |
  |---|---|---|---|
  | 0 | -3.39 % | -0.25 % | +0.31 % |
  | 1 (default) | +9.50 % | +5.08 % | +4.41 % |
  | 3 | +0.34 % | +5.47 % | +6.40 % |

  In that experiment size 3 favoured full_map (+6.40 % against +4.41 %), size 1 favoured aux_card (+9.50 % against +0.34 %), aux_map
  was about equal and size 0 lost on aux_card. The size counts cache lines (a miss fetches size + 1 lines; measured with 64 B
  lines), so these numbers are for the 64 KB / 64 B-line D-cache.

What to know before enabling it:
- **It is a global cache setting made with ROM cache calls** (`Cache_Config_DCache_Autoload`, `Cache_Config_DCache_Region_Autoload`,
  `Cache_Enable_DCache_Autoload`), which IDF marks "not for applications". The library owns autoload region 0 and its trigger, order
  and size (nothing else in IDF 5.4 configures D-cache autoload). There is one region: with several decoders the last caller wins.
- **It stays on.** Autoload is left on when `vj_decode_au()` returns and after `vj_decoder_destroy()`, with the region and the enable
  still on the freed blocks (your code may reuse that memory). The boot default is off and you opted in by enabling the option.
  `Cache_Disable_DCache_Autoload()` stops it only after the last `vj_decode_au()`: every call enables it again.
- **Hazard with IDF's own cache use (unmeasured).** IDF suspends autoload around its cache write-back (`esp_cache_msync` C2M)
  because of an S3 erratum (`ESP_ROM_HAS_CACHE_WRITEBACK_BUG`, patched in `esp_rom_cache_esp32s2_esp32s3.c`), and esp_lcd's RGB
  panel starts manual preloads from its ISR (`Cache_Start_DCache_Preload`, `esp_lcd_panel_rgb.c`) that suspend it.
  `vj_decode_au()` on another core re-enables it without coordinating with either. If you write back PSRAM or use an RGB panel
  with a PSRAM framebuffer, keep those on the decoder's task or leave this option off. (Read from the IDF 5.4.2 sources; the
  interaction has not been tested.)
- **The span can include your own PSRAM** that lies between the decoder's blocks. Autoload only reads, but a buffer that DMA writes
  inside the span needs `esp_cache_msync(..., M2C)` before the CPU reads it, as IDF already requires.
- **The first picture of a decoder is not covered.** The call that first activates an SPS allocates the blocks, so that picture
  decodes before the region exists (with whatever region is still programmed, none on a fresh boot); the next call arms it. The same
  holds after a resolution change.
- **Two traps, both handled.** A region that ends at `0x3E000000`, the end of the data window, makes
  `Cache_Enable_DCache_Autoload()` panic ("Dcache preload parameter configuration error") and the chip boot-loops: the library
  clamps the end one line below it. And any IDF flash operation (`esp_partition_read`, NVS, OTA) clears the autoload enable bit
  (it restores the boot state) while keeping the size and region: every `vj_decode_au()` sets it again. Evidence:
  `docs/bench-log.md`, "D-cache autoload (T11)".
- **Cost:** 350 B of IRAM and 4 B of DRAM (libvideojet 41,318 B IRAM and 7,014 B DRAM with it on, 40,968 B and 7,010 B off, at the
  T6d tip).

## 5. Contract
- One AU per `vj_decode_au()` call; `vj_au_length()` splits a byte stream. Pictures come out in decode order.
- One decoder per task: calls on one decoder must not overlap.
- With `CONFIG_VJ_DEBLOCK_WORKER` the decoder owns a worker task from `vj_decoder_create()` to `vj_decoder_destroy()`: call both
  from the decoding task, which must not be deleted while a `vj_decode_au()` is in flight. Every call returns with the worker idle.
- `f.y`, `f.u` and `f.v` are valid until the next `vj_decode_au()` or `vj_decoder_destroy()`.
- The call rewrites the AU in place (it removes emulation-prevention bytes): pass writable RAM, never flash-mapped data, and
  decode each AU once.
- `f.changed_mb` has one byte per 16x16 macroblock of the coded frame, `(width / 16) x (height / 16)`, row by row, valid
  as long as `f.y`. 0 means the macroblock is bit-identical to the same macroblock in the previous `VJ_FRAME` this
  decoder returned, so a consumer that converted that frame can skip it; 1 means it may differ. Frames decoded in between
  but never returned (an error call, a dropped concealed picture) are accounted for. If you skip converting a `VJ_FRAME`,
  copy its map (it lives only until the next call) and OR it into the next frame's: the result is the change since the
  last frame you converted. Treating every macroblock of the next frame as changed is the simple fallback. NULL means the
  same as all 1. The first frame, an IDR, and a frame with concealed macroblocks have every byte at 1.
- Frames are unreliable until one with `f.idr` arrives: before the first IDR of a stream (a P-frame can decode as a
  clean-looking `VJ_FRAME`) and after any `VJ_ERR_*` (a lost reference can leave `error_mbs` at 0). After an error,
  request a keyframe. `error_mbs` counts concealed macroblocks in a returned frame and is rarely non-zero, but
  `error_mbs` > 0 on a returned frame is also a keyframe cue: the frame is damaged, so request one and show nothing
  until the next IDR.

### Rows as they become final (optional)
`vj_decoder_set_rows_callback(dec, fn, ctx)` hands each macroblock row of the picture being decoded to `fn` as soon as
nothing later in the picture changes it. With `CONFIG_VJ_DEBLOCK_WORKER` that is about two rows behind the decode, on the
worker's task, so a display can convert and send a strip of the picture while the rest of it decodes. That avoids
re-reading the finished picture from PSRAM and overlaps the panel transfer with the decode.
- **Arguments:** the picture so far (`f->changed_mb` is NULL; the row's flags are the `changed` argument, exactly what
  `f->changed_mb` will hold for that row), and the row.
- **Order:** increasing within a pass. A slice error or concealment makes the picture's final pass hand every row over
  again in the same call, and the last delivery wins.
- **When to show:** every row of a returned `VJ_FRAME` is delivered before the call returns. Rows delivered in a call
  that returns anything else are not a picture to show.
- **Without the worker:** every row is delivered at the end of the call (no overlap).
- **Where it runs:** mostly inside the worker's parse/deblock loop, so keep `fn` short.
- **Cost:** none when no callback is set beyond a test per deblock segment; 354 B of IRAM with the bench configuration.
- **Tests:** `host/test_rows.c` (in `make check`, every configuration, clean and damaged streams). It checks every row of
  every returned frame, the last delivery against the final picture and `changed_mb`, and that the output is identical
  with and without a callback.

## 6. Kconfig (menu "VideoJet")
- `CONFIG_VJ_DEBLOCK_WORKER` (default n; the bench turns it on): the deblocking filter runs on a worker task, one MB row
  behind the decode (rows r filtered once r + 1 is decoded), bit-identical to the sequential decoder on every path
  including slice errors and concealment (undo log of the rows the failed slice touched; `h264bsd/h264bsd_deblocking.c` has
  the argument; the host gate runs it threaded, `make -C host check-tsan` under ThreadSanitizer).
  - **Core and priority:** `vj_decoder_create()` creates a FreeRTOS task "vjdbw" pinned to `CONFIG_VJ_WORKER_CORE`, at the
    creating task's own priority, with a 2 KB internal stack (3 KB with the parse pipeline; 988 B / 1,132 B used).
    `vj_decoder_destroy()` stops it.
  - **Rules:** create, decode and destroy from one task pinned to one core (the worker takes the other core); never delete that
    task while a `vj_decode_au()` is in flight. `vj_decode_au()` returns with the worker idle on every path.
  - **Starvation:** if a task on the worker's core at or above its priority keeps it from starting a run, `vj_decode_au()`
    takes the run back after 50 ms and filters on the decoding task (bit-identical, no overlap; one warning in the log).
    A run the worker has started is waited for however long it takes, so a task that preempts the worker mid-run can
    still stall the call while it runs: keep higher-priority work off the worker's core.
  - **Profiling:** with `CONFIG_VJ_PROFILE` the deblock bucket measures only the decoding task's wait for the worker (with the
    pipeline the parse bucket measures only the decoding task's fetch, i.e. its wait for a record), and the worker's log line
    at destroy gives its stack high water and, with the pipeline, its slice arms, ring-full sleeps and wakes.
  - **Budget:** the table in section 3. The worker adds about 2.7 KB of heap (2 KB stack, control blocks), 2.6 KB of IRAM, and
    118,280 B of PSRAM (the 4-row undo log, 115,200 B at 50 MBs per row, and the 1,530 B of marks, both PSRAM-first).
  - Gain on the bench (slow PSRAM class, same tree, worker off to on): aux_card / aux_map / full_map 95.30 / 21.89 / 11.65
    to 100.95 / 26.03 / 13.96 fps. Both cores share one D-cache and PSRAM bus, so the decode core slows a little while the
    worker runs.
- `CONFIG_VJ_PARSE_PIPELINE` (default n; the bench turns it on; needs `CONFIG_VJ_DEBLOCK_WORKER`): the same worker task also parses the
  slice's macroblocks (skip runs, macroblock_layer, CAVLC) into a ring of 6 records, ahead of the decoding task, which
  reconstructs them in place. The parser reads nothing the decoding task writes, so the output (pixels, `changed_mb`, status,
  concealment, every error path) is bit-identical to the sequential decoder: a parse error is a record the decoding task reaches in
  order (`h264bsd/h264bsd_slice_data.c` has the protocol). Between records the task filters in segments of 10 macroblocks.
  - **When it runs:** one slice group, a primary (not redundant) slice, a worker on another core than the caller. Otherwise the same
    records are made on the decoding task, as without the option (single-core builds, FMO, redundant slices).
  - **Scheduling:** the worker task stays runnable through each slice it parses: it parses while the ring has room, filters
    in between, polls for about 200 us when the ring is full and no slot was freed, and then sleeps until the decoding task
    frees one (the decoding task notifies only a sleeping worker: no kernel call per macroblock in the normal case). Tasks on
    its core with a lower priority than the worker (the priority of the task that created the decoder) run only in its gaps
    between slices and while it sleeps; work that must run on that core during a decode needs a higher priority, which
    stalls the decode for its run time (and after 50 ms without a record the decoding task parses itself). One decoder per
    worker core.
  - **Rules:** as for the worker (create, decode, destroy from one task; keep the decoding task off the worker's core). The take-back
    uses the esp_timer clock, so an unpinned decoding task is safe: if the worker produces no record for 50 ms the decoding task takes
    the parser back and parses inline (one warning in the log), then parses the next 30 slices inline before asking again; a
    waiter that lands on the worker's core sleeps a tick instead of spinning (1 ms at `CONFIG_FREERTOS_HZ=1000`, the bench's;
    10 ms at IDF's default 100; with `CONFIG_VJ_PIE` a task that calls `vj_decode_au` is pinned at its first PIE
    instruction anyway).
  - **Budget:** **95,833 B** internal, against 99,550 B without the pipeline (section 3): the heap is 5.4 KB lower because the
    18,000 B of deblock records move to PSRAM, even though the ring (6 x 2,112 B), the neighbour shadow (1,275 B) and a 3 KB
    worker stack (against 2 KB) are added; the code is 1.7 KB of IRAM larger. The worker's stack high water with the parser
    is 1,132 B on every bench stream (and 4 KB are reserved with `CONFIG_VJ_ERROR_PRINT`, which prints from the worker).
  - **Gain** on the bench (slow PSRAM class, tuning 5, the same tree without the pipeline, then with it): aux_card / aux_map /
    full_map 101.93 / 26.04 / 13.94 to 100.28 / 31.85 / 16.61 fps (-1.6 / +22.3 / +19.1 %). A near-static picture pays for the
    hand-off and the PSRAM-resident records.
  - **Sleeps:** the worker sleeps when a long record (a skip run of a hundred macroblocks) keeps the ring full for 200 us:
    on the bench 0.1 to 3 times per frame (9 on test_640x360), each with one wake and no effect on the frame rate.
- `CONFIG_VJ_WORKER_CORE` (-1, 0 or 1; default -1 = the core other than the creating task's): the worker's core.
- `CONFIG_VJ_IRAM` (default y): run the decoder from internal RAM (hot code + tables: 49,455 + 7,695 B without the pipeline,
  section 4).
- `CONFIG_VJ_PIE` (default y, ESP32-S3 only): motion compensation, residual write-out and the 4x4 inverse transform in PIE SIMD assembly (MC for 8 and 16 wide partitions),
  bit-exact with the C kernels (which every other case and the host build use). About 3 KB of IRAM.
  Note: IDF pins a task to its current core at its first PIE instruction (portasm.S _frxt_coproc_exc_hook), so an unpinned decode task (any task that calls vj_decode_au) is pinned permanently, normally at its first I-frame (the first block with AC coefficients). PIE destinations must be aligned to the store width (16 B for luma w 16, 8 B for w 8, 4 B for chroma w 4); the library checks per partition and otherwise uses C.
- `CONFIG_VJ_ERROR_PRINT` (default n): print the decoder's bitstream errors to stderr (about 60 messages, from the decoding task;
  with `CONFIG_VJ_PARSE_PIPELINE` the parse errors print from the worker task).
- `CONFIG_VJ_DCACHE_AUTOLOAD` (default n; needs `CONFIG_SPIRAM`): D-cache autoload over the decoder's PSRAM blocks, armed by every
  `vj_decode_au()` (section 4, "D-cache autoload": numbers, hazards and traps).
- `CONFIG_VJ_DCACHE_AUTOLOAD_SIZE` (0 to 3, default 1): lines fetched after a miss, minus one.
- `CONFIG_VJ_PROFILE` (default n): per-stage cycle counters, read with `vj_profile_take()`. Their cost is not separable from code-layout
  noise (`docs/bench-log.md`) and they move code layout: profiling builds only, never timed scores (the bench enables it
  through `bench/sdkconfig.profile`).

## 7. Licensing
The h264bsd core is Apache-2.0 (`h264bsd/LICENSE.md`). The licence for VideoJet's own code is to be chosen by the owner
before publishing.
