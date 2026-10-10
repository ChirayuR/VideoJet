#pragma once
/* PIE (ESP32-S3 SIMD) motion-compensation kernels, vj_mc_pie.S. Windows lie inside the picture; see the .S header for
 * the read slack they need. Luma w is 8 or 16, rows go to d with stride 16.
 * PRECONDITION: d is aligned to the store width: luma w 16 -> 16 B (ee.vst.128.ip silently rounds the address down,
 * corrupting the bytes below), luma w 8 -> 8 B (ee.vst.l.64), chroma w 8 -> 8 B, w 4 -> 4 B (one s32i). The callers in h264bsd_reconstruct.c check this and fall back to C. */
#include "basetype.h"

void vj_pie_luma_copy(const u8 *s, u32 ss, u8 *d, u32 w, u32 h);
void vj_pie_luma_hor0(const u8 *s, u32 ss, u8 *d, u32 w, u32 h); /* b */
void vj_pie_luma_hor1(const u8 *s, u32 ss, u8 *d, u32 w, u32 h); /* a */
void vj_pie_luma_hor2(const u8 *s, u32 ss, u8 *d, u32 w, u32 h); /* c */
void vj_pie_luma_hor3(const u8 *s, u32 ss, u8 *d, u32 w, u32 h); /* averaged into d: f q */
void vj_pie_luma_ver0(const u8 *s, u32 ss, u8 *d, u32 w, u32 h); /* h */
void vj_pie_luma_ver1(const u8 *s, u32 ss, u8 *d, u32 w, u32 h); /* d */
void vj_pie_luma_ver2(const u8 *s, u32 ss, u8 *d, u32 w, u32 h); /* n */
void vj_pie_luma_ver3(const u8 *s, u32 ss, u8 *d, u32 w, u32 h); /* averaged into d: e g p r i k */
void vj_pie_luma_mid(const u8 *s, u32 ss, u8 *d, u32 w, u32 h);  /* j; s at (G - 2, G - 2) */
/* both chroma planes: w 8 or 4; wts = A | B << 8 | C << 16 | D << 24; Cr window ps after Cb, Cr rows at d + 64 */
void vj_pie_chroma(const u8 *s, u32 ss, u8 *d, u32 w, u32 h, u32 wts, u32 ps);
