#include <stdlib.h>
#include "vj_alloc.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

void *vj_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    uint32_t first = (n > VJ_INTERNAL_MAX ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
    return heap_caps_malloc_prefer(n, 2, first, MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}
