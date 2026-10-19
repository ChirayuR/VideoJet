/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* PIE 4x4 inverse transform, vj_idct_pie.S (see its header). PRECONDITION: t and d 16-byte aligned (ee.vld/vst.128
 * silently round an address down, so a misaligned d would corrupt the bytes below it). h264bsdProcessBlock checks d and
 * falls back to C. Returns 0 done, 1 done but a value outside [-512, 511], 2 guard failed (d untouched). */
#include "basetype.h"

extern const i32 vj_idct_k[4];
int vj_pie_idct4(const i32 *t, i32 *d, const i32 *k);
