/* Annex-B access-unit splitting (H.264 7.4.1.2.3, for constrained baseline without arbitrary slice order or
 * redundant pictures). A new AU starts at an AUD/SPS/PPS/SEI or NAL type 14-18, or at a slice whose
 * first_mb_in_slice is 0, once the current AU already holds a slice. first_mb_in_slice is the first ue(v) after the
 * NAL header byte, and ue(v) codes 0 as a single 1 bit: "== 0" is the top bit of that byte. */
#include "videojet.h"

/* Offset of the next 00 00 01 at or after i, or len. */
static size_t next_start(const uint8_t *d, size_t len, size_t i)
{
    for (; i + 3 <= len; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            return i;
    return len;
}

size_t vj_au_length(const uint8_t *d, size_t len)
{
    if (!d)
        return 0;
    size_t sc = next_start(d, len, 0);
    if (sc == len)
        return 0;
    int seen_slice = 0;
    while (sc < len) {
        size_t hdr = sc + 3;
        if (hdr >= len)
            break;
        unsigned type = d[hdr] & 0x1f;
        int starts_au = (type >= 6 && type <= 9) || (type >= 14 && type <= 18);
        int slice = type == 1 || type == 5;
        int first_mb0 = slice && hdr + 1 < len && (d[hdr + 1] & 0x80);
        if (seen_slice && (starts_au || first_mb0))
            return sc > 0 && d[sc - 1] == 0 ? sc - 1 : sc; /* a 4-byte start code's leading zero goes with it */
        seen_slice |= slice;
        sc = next_start(d, len, hdr + 1);
    }
    return len;
}
