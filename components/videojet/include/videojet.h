/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* VideoJet: fast H.264 constrained-baseline decoding for the ESP32-S3.
 *
 * Feed one Annex-B access unit (AU) per vj_decode_au() call. The decoder rewrites the AU in place (it removes
 * emulation-prevention bytes): pass a writable buffer and decode each AU once. A call that completes a picture returns
 * it fully decoded and deblocked, unless the AU also leaves a later picture unfinished: that call returns
 * VJ_ERR_STREAM instead. No decode work is left running when any call returns. Pictures come out in decode
 * order (no output reordering), which is display order for baseline streams that never reorder.
 * Byte-stream sources split their buffer with vj_au_length(). One decoder per task: calls on one decoder must not
 * overlap. Frame buffers live in PSRAM (about 1.5 x width x height bytes each, refs + 1 of them, refs + 2 with
 * vj_decoder_set_hold()). */
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
    VJ_ERR_STREAM = -1,  /* bitstream error, unsupported stream, or the AU ended inside a picture. After any error,
                          * frames may be visibly wrong (a lost reference can leave error_mbs at 0) until the next
                          * IDR (f.idr), including before the first one: request a keyframe and wait for it. */
    VJ_ERR_NOMEM = -2,   /* a buffer could not be allocated (frames need PSRAM) */
    VJ_ERR_ARG = -3,     /* NULL decoder, AU or out; empty AU, or one over 4 GiB */
} vj_status_t;

typedef struct {
    const uint8_t *y, *u, *v;   /* I420 planes, valid until the next vj_decode_au() or vj_decoder_destroy(); with hold
                                 * on, until the next VJ_FRAME has been returned (vj_decoder_set_hold()) */
    uint16_t y_stride, uv_stride;
    uint16_t width, height;     /* coded size, multiples of 16 */
    uint16_t crop_x, crop_y, crop_w, crop_h; /* the picture to show (SPS cropping), else the whole frame */
    bool full_range;            /* VUI video_full_range_flag: 0-255 samples instead of 16-235 */
    uint8_t matrix;             /* VUI matrix_coefficients: 1 BT.709, 5 or 6 BT.601, 2 unspecified */
    bool idr;                   /* an IDR picture: decoding can (re)start here */
    uint32_t error_mbs;         /* macroblocks concealed in this returned frame, rarely non-zero: concealment normally
                                 * surfaces as VJ_ERR_STREAM on the damaged AU. Either is a cue to request a keyframe */
    const uint8_t *changed_mb;  /* one byte per 16x16 macroblock, (width/16) x (height/16) in raster order; valid as long
                                 * as y. 0: the macroblock is bit-identical to the same one in the previous VJ_FRAME of
                                 * this decoder, so a consumer that showed that frame can skip it. 1: it may differ (every
                                 * macroblock of the first frame, an IDR or a concealed frame). NULL: treat all as 1 */
} vj_frame_t;

vj_decoder_t *vj_decoder_create(void);
vj_status_t vj_decode_au(vj_decoder_t *dec, uint8_t *au, size_t len, vj_frame_t *out);
void vj_decoder_destroy(vj_decoder_t *dec);

/* Hold (default off): keep the last frame returned intact while later AUs decode, so another task can read frame k while
 * vj_decode_au() decodes k + 1. With hold on, the y, u, v planes and changed_mb of a frame returned with VJ_FRAME stay
 * valid and unchanged through every later vj_decode_au() call up to and including the one that returns the next
 * VJ_FRAME, whatever the calls in between return (VJ_NO_FRAME, an error, a new SPS with another picture size), and until
 * vj_decoder_destroy() if no other frame comes. The call after the next VJ_FRAME may overwrite or free them. Every frame
 * qualifies: reference or not, IDR, concealed, after MMCO or frame_num gaps, the last one before a resolution change
 * (whose buffer outlives the old DPB until then: PSRAM for both at once).
 * Output equals hold off (statuses, frames, pixels) but for two cases. changed_mb may mark more macroblocks changed, never
 * fewer: on a frame decoded into the extra buffer while it holds no known picture, in practice the frame after the first
 * non-reference frame of a stream or picture size (streams whose pictures are all references never use it). And with
 * redundant slices (redundant_pic_cnt_present_flag), a failing one can make the decoder finish a picture with macroblocks
 * no slice decoded, which show the buffer's old contents: with hold that can be another buffer's, and frames predicted
 * from them differ too, up to the next IDR.
 * Cost: one more frame buffer in PSRAM (589,828 B at 800x480 with the IDF heap), no internal RAM; changed_mb then lives
 * in the frame's buffer (PSRAM), copied there when the frame is returned (one byte per macroblock).
 * Call it after vj_decoder_create() and before the first vj_decode_au() on dec. Returns true when applied, false (and
 * changes nothing) for a NULL dec or once vj_decode_au() has been called on dec, whatever that call returned. */
bool vj_decoder_set_hold(vj_decoder_t *dec, bool on);

/* Length of the first access unit at `data`: up to the next AU's start code, `len` for the last AU, 0 when `data`
 * holds no start code. Constrained baseline without arbitrary slice order or redundant pictures. */
size_t vj_au_length(const uint8_t *data, size_t len);

/* Per-stage CPU cycles since the last take, then zeroed. False when profiling is compiled out.
 * Within inter: mvpred (motion-vector prediction), copy (direct skip copy), mc (motion compensation) and write
 * (prediction + residual into the picture). Within mc: chroma, and luma by quarter-sample position 4 * xFrac + yFrac
 * (0: full-sample copy). */
typedef struct {
    uint64_t parse, recon, inter, deblock;
    uint64_t mvpred, copy, mc, write, chroma, luma[16];
} vj_profile_t;
bool vj_profile_take(vj_profile_t *out);

#ifdef __cplusplus
}
#endif
