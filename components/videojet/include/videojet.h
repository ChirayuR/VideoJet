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
