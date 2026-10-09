/* vjdec in.h264 [out.yuv] : decode with h264bsd, print one vj_hash per output frame.
 * Mirrors the board bench loop (bench/main/bench_main.c) so host and board agree frame for frame:
 * change one, change the other. */
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
