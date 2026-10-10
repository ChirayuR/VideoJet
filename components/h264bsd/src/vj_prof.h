#pragma once
/* Per-stage cycle counters, profile builds only:
 *   idf.py -C bench -B bench/build-prof -DVJ_PROFILE=1 build
 * Compiled out otherwise, so timed builds are not perturbed. CCOUNT is per core: decode on one core. */
#if defined(VJ_PROFILE) && defined(ESP_PLATFORM)
#include <stdint.h>
#include "esp_cpu.h"
enum { VJP_PARSE, VJP_RECON, VJP_INTER, VJP_DEBLOCK, VJP_N };
extern uint64_t vj_prof[VJP_N];
#define VJP_BEGIN(k) uint32_t vjp_t_##k = esp_cpu_get_cycle_count()
#define VJP_END(k) (vj_prof[(k)] += (uint32_t)(esp_cpu_get_cycle_count() - vjp_t_##k))
#else
#define VJP_BEGIN(k) ((void)0)
#define VJP_END(k) ((void)0)
#endif
