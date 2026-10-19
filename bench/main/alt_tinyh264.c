/* Comparison builds (VJ_BENCH_DEC=tinyh264, bench/CMakeLists.txt): upstream tinyH264 (h264bsd trimmed for WebAssembly,
 * unmodified, its code in flash) behind VideoJet's API, so bench_main.c times and hashes it exactly as it does VideoJet.
 * tinyH264's own h264bsdDecode() fails an AU that holds no picture, so this walks the AU's NAL units with
 * h264bsdDecodeInternal() and takes the picture the way that wrapper does. */
#include <stdlib.h>
#include "h264bsd_decoder.h"
#include "videojet.h"

/* Defined in tinyH264's h264bsd_decoder.c, not declared in its header. */
u32 h264bsdDecodeInternal(storage_t *pStorage, u8 *byteStrm, u32 len, u32 *readBytes);

struct vj_decoder {
    storage_t *st;
};

void vj_decoder_destroy(vj_decoder_t *dec)
{
    if (!dec)
        return;
    if (dec->st) {
        h264bsdShutdown(dec->st);
        h264bsdFree(dec->st);
    }
    free(dec);
}

vj_decoder_t *vj_decoder_create(void)
{
    vj_decoder_t *dec = calloc(1, sizeof *dec);
    if (dec && (dec->st = h264bsdAlloc()) && h264bsdInit(dec->st, 1) == H264BSD_RDY) /* 1: no output reordering */
        return dec;
    vj_decoder_destroy(dec);
    return NULL;
}

vj_status_t vj_decode_au(vj_decoder_t *dec, uint8_t *au, size_t len, vj_frame_t *out)
{
    vj_status_t st = VJ_NO_FRAME;
    u32 left = (u32)len;
    while (left) {
        u32 used = 0;
        u32 rc = h264bsdDecodeInternal(dec->st, au, left, &used);
        if (rc == H264BSD_PIC_RDY) {
            const seqParamSet_t *sps = dec->st->activeSps;
            const dpbOutPicture_t *pic = h264bsdDpbOutputPicture(dec->st->dpb);
            if (!pic)
                return VJ_ERR_STREAM;
            u32 w = sps->picWidthInMbs * 16, h = sps->picHeightInMbs * 16;
            *out = (vj_frame_t){.y = pic->data, .u = pic->data + w * h, .v = pic->data + w * h + w * h / 4,
                                .y_stride = w, .uv_stride = w / 2, .width = w, .height = h, .crop_w = w, .crop_h = h};
            if (sps->frameCroppingFlag) { /* offsets count 2 samples in 4:2:0 frames */
                out->crop_x = 2 * sps->frameCropLeftOffset;
                out->crop_y = 2 * sps->frameCropTopOffset;
                out->crop_w = w - 2 * (sps->frameCropLeftOffset + sps->frameCropRightOffset);
                out->crop_h = h - 2 * (sps->frameCropTopOffset + sps->frameCropBottomOffset);
            }
            st = VJ_FRAME;
        } else if (rc == H264BSD_MEMALLOC_ERROR) {
            return VJ_ERR_NOMEM;
        } else if (rc != H264BSD_HDRS_RDY && (rc != H264BSD_RDY || !used)) {
            /* A decode error, or no progress: never spin. HDRS_RDY reads 0 bytes on purpose: the slice that activated
             * new parameter sets is decoded by the next call, as in tinyH264's own h264bsdDecode() loop. */
            return VJ_ERR_STREAM;
        }
        au += used;
        left -= used;
    }
    return st;
}

bool vj_profile_take(vj_profile_t *p)
{
    (void)p;
    return false;
}
