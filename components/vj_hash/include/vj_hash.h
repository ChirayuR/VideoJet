#pragma once
/* The one frame hash in VideoJet: ffmpeg goldens, host decoder and board bench all use it.
 * An FNV-1a-style hash over little-endian 32-bit words of each plane row, with an xorshift
 * (h ^= h >> 15) after every multiply. NOT standard FNV-1a: a bare multiply only carries
 * differences upward, so a difference confined to one byte lane (e.g. the top byte) would
 * never reach the low bits. A row's tail bytes (width % 4) are folded in one at a time the
 * same way. Planes in order Y, U, V (I420). */
#include <stddef.h>
#include <stdint.h>

#define VJ_FNV_OFFSET 2166136261u
#define VJ_FNV_PRIME  16777619u

static inline uint32_t vj_hash_row(uint32_t h, const uint8_t *p, size_t n)
{
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 |
                     (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
        h = (h ^ w) * VJ_FNV_PRIME;
        h ^= h >> 15;
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * VJ_FNV_PRIME;
        h ^= h >> 15;
    }
    return h;
}

/* Hash the w x h window at (x0, y0) of an I420 picture. x0, y0, w, h must be even. */
static inline uint32_t vj_hash_i420(const uint8_t *y, size_t y_stride,
                                    const uint8_t *u, const uint8_t *v, size_t c_stride,
                                    size_t x0, size_t y0, size_t w, size_t h)
{
    uint32_t hv = VJ_FNV_OFFSET;
    for (size_t r = 0; r < h; r++)
        hv = vj_hash_row(hv, y + (y0 + r) * y_stride + x0, w);
    for (size_t r = 0; r < h / 2; r++)
        hv = vj_hash_row(hv, u + (y0 / 2 + r) * c_stride + x0 / 2, w / 2);
    for (size_t r = 0; r < h / 2; r++)
        hv = vj_hash_row(hv, v + (y0 / 2 + r) * c_stride + x0 / 2, w / 2);
    return hv;
}
