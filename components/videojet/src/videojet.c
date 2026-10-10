/* The public API over the vendored h264bsd decoder (no output reordering: DPB = refs + 1, decode order out). */
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
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

/* Fill *f for the picture just decoded. 0 when the frame cannot be described (its fields are 16 bit). */
static int describe(storage_t *s, const u8 *pic, u32 idr, u32 nerr, vj_frame_t *f)
{
    u32 W = h264bsdPicWidth(s) * 16, H = h264bsdPicHeight(s) * 16;
    if (W > 0xFFFF || H > 0xFFFF)
        return 0;
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
    f->changed_mb = s->dpb->vjChanged;
    return 1;
}

vj_status_t vj_decode_au(vj_decoder_t *dec, uint8_t *au, size_t len, vj_frame_t *out)
{
    if (!dec || !au || !len || !out || len > 0xffffffffu)
        return VJ_ERR_ARG;
    storage_t *s = S(dec);
    s->prevBufNotFinished = HANTRO_FALSE; /* a call never continues the last: no stale "resume at this pointer" */
    u8 *p = au;
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
            /* PIC_RDY before consuming anything: h264bsd just concealed the previous AU's unfinished picture,
             * already reported as VJ_ERR_STREAM. Drop it, pixels or not, and go on to this call's own AU (it may be
             * the recovery IDR): each call decodes and returns only its own picture. */
            if (used == 0 && at_start)
                continue;
            if (!pic)
                return VJ_ERR_STREAM;
            if (++frames > 1)
                return VJ_ERR_STREAM; /* two pictures in one access unit */
            if (!describe(s, pic, idr, nerr, out))
                return VJ_ERR_STREAM;
        } else if (used == 0 && ++stalls > 2) {
            return VJ_ERR_STREAM;
        }
        if (used)
            stalls = 0;
    }
    /* The AU ended inside a picture, whether or not it also completed one. The next call conceals and drops that
     * picture and every later one decodes on it: only an error now makes the consumer ask for a keyframe, VJ_FRAME
     * would hide it. (picStarted also stays set when a slice failed before the first activation, activeSps == NULL:
     * parameter-set-only AUs then return VJ_ERR_STREAM until a picture completes. Harmless: that slice's own error
     * already asked for a keyframe.) */
    if (s->picStarted)
        return VJ_ERR_STREAM;
    if (!frames)
        return VJ_NO_FRAME;
    /* changed_mb holds the changes since the previous picture decoded. Make them the changes since the last picture
     * returned when some were decoded in between and not returned (dropped, or completed by a call that failed). */
    dpbStorage_t *d = s->dpb;
    if (d->vjLastOut + 1 != d->vjPicIdx)
        for (u32 i = 0; i < d->vjMbs; i++)
            d->vjChanged[i] = d->vjLastChanged[i] > d->vjLastOut;
    d->vjLastOut = d->vjPicIdx;
    return VJ_FRAME;
}

#if defined(ESP_PLATFORM) && defined(CONFIG_VJ_PROFILE)
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
