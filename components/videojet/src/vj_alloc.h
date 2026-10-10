#pragma once
/* The library decides where its buffers go, whatever the consumer's malloc settings. Above VJ_INTERNAL_MAX bytes
 * (frames, per-MB state) it is PSRAM first; below it, internal first; either falls back to the other. 16 KiB is the
 * IDF default CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL that M1 was measured with, so placement is unchanged. It asks for the
 * same capabilities as IDF's own malloc (MALLOC_CAP_DEFAULT), so it can never take the internal DMA reserve
 * (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL) that only explicit DMA requests may use. */
#include <stddef.h>
#define VJ_INTERNAL_MAX 16384
void *vj_alloc(size_t n);
