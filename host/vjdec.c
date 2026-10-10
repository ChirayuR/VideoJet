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
    if (size < 0) { /* not seekable (a pipe): the size is unknown */
        fprintf(stderr, "%s: cannot size the input, give a regular file\n", argv[1]);
        fclose(f);
        return 2;
    }
    uint8_t *buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n");
        fclose(f);
        free(buf);
        return 2;
    }
    fclose(f);
    FILE *yuv = argc > 2 ? fopen(argv[2], "wb") : NULL;
    if (argc > 2 && !yuv) {
        perror(argv[2]);
        free(buf);
        return 2;
    }

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
