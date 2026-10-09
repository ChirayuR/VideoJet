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
    if (w == 0 || h == 0 || ((w | h) & 1)) {
        fprintf(stderr, "%s: W and H must be non-zero and even\n", argv[0]);
        return 2;
    }
    size_t n = w * h * 3 / 2, got;
    unsigned char *f = malloc(n);
    if (!f)
        return 2;
    while ((got = fread(f, 1, n, stdin)) == n) {
        const unsigned char *u = f + w * h, *v = u + (w / 2) * (h / 2);
        printf("%08x\n", (unsigned)vj_hash_i420(f, w, u, v, w / 2, 0, 0, w, h));
    }
    if (got || ferror(stdin)) {
        fprintf(stderr, "%s: %s\n", argv[0], got ? "partial trailing frame (wrong W H?)" : "read error");
        return 1;
    }
    return 0;
}
