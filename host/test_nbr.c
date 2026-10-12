/* The parser's shadow copies only the totalCoeff entries a neighbour macroblock can supply to nC (VJ_COPY_TC). They must be
 * exactly the targets of the MB_A / MB_B rows of the neighbour tables DetermineNc reads through: a missing one would read
 * stale shadow bytes (the differential finds that on real streams, this finds it without a stream). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "h264bsd_neighbour.h"
#include "h264bsd_parse.h"

int main(void)
{
    int need[24] = {0};
    for (u32 b = 0; b < 24; b++) {
        const neighbour_t *a = h264bsdNeighbour4x4BlockA(b), *u = h264bsdNeighbour4x4BlockB(b);
        if (a->mb != MB_CURR) {
            assert(a->mb == MB_A);
            need[a->index] = 1;
        }
        if (u->mb != MB_CURR) {
            assert(u->mb == MB_B);
            need[u->index] = 1;
        }
    }
    i16 src[27];
    u8 dst[24];
    for (int i = 0; i < 27; i++)
        src[i] = (i16)(i + 1);
    memset(dst, 0, sizeof dst);
    VJ_COPY_TC(dst, src);
    int n = 0;
    for (int i = 0; i < 24; i++) {
        assert(need[i] ? dst[i] == i + 1 : dst[i] == 0); /* copied exactly where a neighbour can read */
        n += need[i];
    }
    printf("test_nbr: ok (%d of 24 entries)\n", n);
    return 0;
}
