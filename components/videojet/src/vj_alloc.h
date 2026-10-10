#pragma once
/* The library decides where its buffers go, whatever the consumer's malloc settings. Above VJ_INTERNAL_MAX bytes
 * (frames, per-MB state) it is PSRAM first; below it, internal first; either falls back to the other. 16 KiB is the
 * IDF default CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL that M1 was measured with, so placement is unchanged. */
#include <stddef.h>
#define VJ_INTERNAL_MAX 16384
void *vj_alloc(size_t n);
