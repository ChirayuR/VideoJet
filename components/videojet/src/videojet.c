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

#if defined(ESP_PLATFORM) && defined(CONFIG_VJ_DCACHE_AUTOLOAD)
#include "esp_memory_utils.h"
#include "esp32s3/rom/cache.h"
#include "soc/extmem_reg.h"
#include "soc/soc.h"

/* D-cache autoload over the decoder's own PSRAM blocks, armed at the start of every call. Output cannot change: autoload
 * only decides which lines are already cached. Its rules are in no IDF header; they were measured on the S3 with a scratch
 * probe (2026-10-11):
 *  - The ROM stores any byte address and size, but the hardware works in cache lines: a miss requests the SIZE + 1 lines
 *    after the missed one, a request whose first line starts below the region is dropped, and one that crosses the
 *    region's end is cut there. So the start is rounded down and the end up to 64 B, the largest S3 line (right for 16
 *    and 32 B too).
 *  - A region that ends at 0x3E000000 (the end of the data window) makes Cache_Enable_DCache_Autoload() panic ("Dcache
 *    preload parameter configuration error"); every lower end is accepted. The end is clamped one line below it.
 *  - IDF's flash operations (esp_partition_read, NVS, OTA) leave the size and the region alone but clear the enable bit:
 *    esp_cache_resume_ext_mem_cache restores the boot state, autoload off. Cache_Enable_DCache_Autoload() sets it again,
 *    and every call does. Ascending order and the miss trigger are the zero bits of AL_CTRL_WANT. */
#define AL_LINE 64u
#define AL_CTRL_MASK (EXTMEM_DCACHE_AUTOLOAD_SCT0_ENA | EXTMEM_DCACHE_AUTOLOAD_ORDER | EXTMEM_DCACHE_AUTOLOAD_RQST_M | \
                      EXTMEM_DCACHE_AUTOLOAD_SIZE_M)
#define AL_CTRL_WANT (EXTMEM_DCACHE_AUTOLOAD_SCT0_ENA | (CONFIG_VJ_DCACHE_AUTOLOAD_SIZE << EXTMEM_DCACHE_AUTOLOAD_SIZE_S))

/* Grow [*lo, *hi) by the n bytes at p, if p is in the PSRAM mapping (esp_ptr_external_ram tests the mapped range itself). */
static void al_span(uintptr_t *lo, uintptr_t *hi, const void *p, size_t n)
{
    if (p && esp_ptr_external_ram(p)) {
        if ((uintptr_t)p < *lo)
            *lo = (uintptr_t)p;
        if ((uintptr_t)p + n > *hi)
            *hi = (uintptr_t)p + n;
    }
}

/* Region 0 = the min..max span of the PSRAM blocks the decoder holds right now. It is read back from the hardware and
 * rewritten only when it differs, so a resolution change, or someone else's autoload setting, is caught the same way.
 * The call that first activates an SPS allocates these blocks, so that picture decodes before there is a region: the next
 * call arms it.
 * ponytail: one span, so blocks far apart prefetch the gap too (the hardware has a second section, SCT1, if that shows). */
static void autoload_arm(const storage_t *s)
{
    const dpbStorage_t *d = s->dpb;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    size_t mbs = d->vjMbs; /* MBs per picture as allocated: mb[], the two maps and every DPB picture are sized by it */
    if (mbs && d->buffer) {
        al_span(&lo, &hi, s->mb, mbs * sizeof *s->mb);
        al_span(&lo, &hi, s->sliceGroupMap, mbs * sizeof *s->sliceGroupMap);
        al_span(&lo, &hi, d->vjLastChanged, mbs * sizeof *d->vjLastChanged);
        for (u32 i = 0; i <= d->dpbSize; i++) /* h264bsdInitDpb: dpbSize + 1 pictures of 384 B per MB */
            al_span(&lo, &hi, d->buffer[i].data, mbs * 384);
    }
    if (hi > lo) {
        lo &= ~(uintptr_t)(AL_LINE - 1);
        hi = (hi + AL_LINE - 1) & ~(uintptr_t)(AL_LINE - 1);
        if (hi > SOC_EXTRAM_DATA_HIGH - AL_LINE)
            hi = SOC_EXTRAM_DATA_HIGH - AL_LINE;
        if (hi > lo && esp_ptr_external_ram((const void *)(hi - 1))) { /* the whole span is inside the mapping */
            uint32_t size = (uint32_t)(hi - lo);
            if ((REG_READ(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG) & AL_CTRL_MASK) != AL_CTRL_WANT ||
                REG_READ(EXTMEM_DCACHE_AUTOLOAD_SCT0_ADDR_REG) != lo ||
                (REG_READ(EXTMEM_DCACHE_AUTOLOAD_SCT0_SIZE_REG) & EXTMEM_DCACHE_AUTOLOAD_SCT0_SIZE) != size) {
                const struct autoload_config c = {.ena = 1, .order = CACHE_AUTOLOAD_POSITIVE,
                                                  .trigger = CACHE_AUTOLOAD_MISS_TRIGGER,
                                                  .size = CONFIG_VJ_DCACHE_AUTOLOAD_SIZE};
                const struct autoload_region_config r = {.region = CACHE_AUTOLOAD_REGION0, .ena = 1, .addr = lo,
                                                         .size = size};
                Cache_Config_DCache_Autoload(&c); /* switches autoload off first */
                (void)Cache_Config_DCache_Region_Autoload(&r);
            }
        }
    }
    Cache_Enable_DCache_Autoload();
}
#else
#define autoload_arm(s) ((void)0)
#endif

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
    autoload_arm(s);
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
    out->mvpred = vj_prof[VJP_MVPRED];
    out->copy = vj_prof[VJP_COPY];
    out->mc = vj_prof[VJP_MC];
    out->write = vj_prof[VJP_WRITE];
    out->chroma = vj_prof[VJP_CHROMA];
    memcpy(out->luma, vj_prof + VJP_LUMA, sizeof out->luma);
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
