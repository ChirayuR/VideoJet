#pragma once
/* Per-stage cycle counters, CONFIG_VJ_PROFILE builds only (bench: see bench/sdkconfig.profile).
 * Compiled out otherwise, so timed builds are not perturbed. CCOUNT is per core: decode on one core.
 * VJP_BEGIN declares a variable, so use it only at the top of a { } block. The bracket in
 * h264bsd_macroblock_layer.c under H264DEC_OMXDL is dead code: no build defines that.
 * Inside INTER: MVPRED (motion-vector prediction), COPY (the direct skip copy), MC (h264bsdPredictSamples), WRITE
 * (h264bsdWriteOutputBlocks / h264bsdWriteMacroblock). Inside MC: CHROMA and LUMA + the quarter-sample position
 * 4 * xFrac + yFrac (0 = full-sample copy), via VJP_END_AT. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(ESP_PLATFORM) && defined(CONFIG_VJ_PROFILE) /* a Kconfig bool that is off is simply undefined */
#include <stdint.h>
#include "esp_cpu.h"
enum { VJP_PARSE, VJP_RECON, VJP_INTER, VJP_DEBLOCK, VJP_MVPRED, VJP_COPY, VJP_MC, VJP_WRITE, VJP_CHROMA, VJP_LUMA,
       VJP_N = VJP_LUMA + 16 };
extern uint64_t vj_prof[VJP_N];
#define VJP_BEGIN(k) uint32_t vjp_t_##k = esp_cpu_get_cycle_count()
#define VJP_END_AT(k, i) (vj_prof[(i)] += (uint32_t)(esp_cpu_get_cycle_count() - vjp_t_##k))
#define VJP_END(k) VJP_END_AT(k, k)
#else
#define VJP_BEGIN(k) ((void)0)
#define VJP_END_AT(k, i) ((void)0)
#define VJP_END(k) ((void)0)
#endif
