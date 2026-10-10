/* The C kernels as the oracle: the library's h264bsd_reconstruct.c compiled once more with PIE off and its global
 * symbols renamed, linked next to the library's own copy (PIE on, as CONFIG_VJ_PIE builds it). */
#include "sdkconfig.h"
#undef CONFIG_VJ_PIE
#undef CONFIG_VJ_PROFILE
#define h264bsdPredictSamples c_PredictSamples
#define h264bsdFillBlock c_FillBlock
#define h264bsdFillRow7 c_FillRow7
#define h264bsdInterpolateChromaHor c_ChromaHor
#define h264bsdInterpolateChromaVer c_ChromaVer
#define h264bsdInterpolateChromaHorVer c_ChromaHorVer
#include "h264bsd_reconstruct.c"
