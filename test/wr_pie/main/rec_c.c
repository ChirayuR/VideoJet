/* The C write-out of the library's h264bsd_image.c compiled once more with PIE off and its global symbols renamed,
 * linked next to the library's own copy (PIE on). */
#include "sdkconfig.h"
#undef CONFIG_VJ_PIE
#define h264bsdWriteMacroblock c_WriteMacroblock
#define h264bsdWriteOutputBlocks c_WriteOutputBlocks
#include "h264bsd_image.c"
