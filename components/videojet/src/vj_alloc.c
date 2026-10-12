#include <stdlib.h>
#include "vj_alloc.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

void *vj_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    /* The capability sets of heap_caps_malloc_default (IDF's malloc): MALLOC_CAP_DEFAULT excludes the internal DMA
     * reserve, which is registered as DMA | INTERNAL | 8BIT without DEFAULT and so would match a bare 8BIT request. */
    uint32_t first = (n > VJ_INTERNAL_MAX ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_DEFAULT;
    return heap_caps_malloc_prefer(n, 2, first, MALLOC_CAP_DEFAULT);
#else
    return malloc(n);
#endif
}

void *vj_alloc_internal(size_t n)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT, MALLOC_CAP_DEFAULT);
#else
    return malloc(n);
#endif
}

void *vj_alloc_psram(size_t n)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT, MALLOC_CAP_DEFAULT);
#else
    return malloc(n);
#endif
}

void *vj_alloc_aligned16(size_t n)
{
#ifdef ESP_PLATFORM
    /* internal first, as vj_alloc for small sizes; IDF frees heap_caps_aligned_alloc memory with free() */
    void *p = heap_caps_aligned_alloc(16, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
    return p ? p : heap_caps_aligned_alloc(16, n, MALLOC_CAP_DEFAULT);
#else
    /* posix_memalign, not C11 aligned_alloc: -std=gnu99 does not declare that one, and an implicit declaration returns int
     * (a 64-bit host pointer cut to 32 bits) */
    void *p;
    return posix_memalign(&p, 16, n) ? NULL : p;
#endif
}
