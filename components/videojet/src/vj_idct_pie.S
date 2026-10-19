/* SPDX-License-Identifier: Apache-2.0 */
/* 4x4 inverse transform of h264bsdProcessBlock's full path, in ESP32-S3 PIE (128-bit SIMD) assembly. The C code in
 * h264bsd_transform.c is the oracle: this matches it bit for bit (on-board test, test/idct_pie).
 *
 *   int vj_pie_idct4(const i32 *t, i32 *d, const i32 *k)
 *     t  16 dequantised coefficients, TRANSPOSED: t[4 * col + row] = coefficient (row, col). 16-byte aligned, read-only.
 *     d  the block's 16 outputs, row-major, 16-byte aligned (ee.vld/vst.128 silently round an address down).
 *     k  vj_idct_k below (16-byte aligned).
 *   returns 0: done, d written;  1: done, d written, but a value was outside [-512, 511] (HANTRO_NOK);
 *           2: guard failed, d untouched: the caller must run the C code.
 *
 * Why it matches the i32 C code: the vector adds are saturating (ee.vadds.s32 / ee.vsubs.s32), the C code wraps. They
 * agree while no intermediate leaves the i32 range. The guard first checks every |t| < 2^26 (t + 2^26 must have no bit
 * >= 27 set: a saturated or negative sum has one). Then a row pass gives at most 3.5 * 2^26, the column pass 12.25 * 2^26,
 * plus the rounding bias: far below 2^31.
 *
 * Lanes: V_c = t[4c..4c+3] holds column c, lane = row. The horizontal pass is then lane-wise across V_0..V_3 and
 * gives H_k (lane = row, k = output column). Four ee.vzip.32 transpose H into R_r (lane = column); the vertical pass is
 * lane-wise across R_0..R_3 and each result vector is one output row. Rounding: ((x + 32 + 512 * 64) >> 6) = v + 512
 * exactly (the bias is a multiple of 64), so OR-ing those over the block tests the range (a set bit >= 10 means
 * v + 512 is outside 0..1023); v is that minus 512.
 */
    .section .rodata.vj_idct_k, "a"
    .align  16
    .global vj_idct_k
vj_idct_k:
    .word   0x04000000             /* +0  guard bias 2^26 */
    .word   32 + (512 << 6)        /* +4  rounding + range bias */
    .word   512                    /* +8  range bias removal */
    .word   0

    .text
    .literal_position
    .align  4
    .global vj_pie_idct4
    .type   vj_pie_idct4,@function
vj_pie_idct4:
    entry   a1, 16
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ee.vld.128.ip q2, a2, 16
    ee.vld.128.ip q3, a2, 16
    /* guard: OR over the 16 values of (t + 2^26) */
    ee.vldbc.32 q4, a4
    ee.vadds.s32 q5, q0, q4
    ee.vadds.s32 q6, q1, q4
    ee.vadds.s32 q7, q2, q4
    ee.vadds.s32 q4, q3, q4
    ee.orq  q5, q5, q6
    ee.orq  q7, q7, q4
    ee.orq  q5, q5, q7
    ee.movi.32.a q5, a5, 0
    ee.movi.32.a q5, a6, 1
    ee.movi.32.a q5, a7, 2
    ee.movi.32.a q5, a8, 3
    movi    a9, 1
    ssr     a9
    or      a5, a5, a6
    or      a7, a7, a8
    or      a5, a5, a7
    srli    a5, a5, 27
    bnez    a5, .Lguard
    /* horizontal pass */
    ee.vadds.s32 q4, q0, q2         /* p0 = x0 + x2 */
    ee.vsubs.s32 q5, q0, q2         /* p1 = x0 - x2 */
    ee.vsr.32 q6, q1                /* x1 >> 1 */
    ee.vsr.32 q7, q3                /* x3 >> 1 */
    ee.vsubs.s32 q6, q6, q3         /* p2 = (x1 >> 1) - x3 */
    ee.vadds.s32 q7, q1, q7         /* p3 = x1 + (x3 >> 1) */
    ee.vadds.s32 q0, q4, q7         /* H0 = p0 + p3 */
    ee.vadds.s32 q1, q5, q6         /* H1 = p1 + p2 */
    ee.vsubs.s32 q2, q5, q6         /* H2 = p1 - p2 */
    ee.vsubs.s32 q3, q4, q7         /* H3 = p0 - p3 */
    /* transpose: H_k (lane = row) -> R_r (lane = column) */
    ee.vzip.32 q0, q2
    ee.vzip.32 q1, q3
    ee.vzip.32 q0, q1
    ee.vzip.32 q2, q3
    /* vertical pass, rounding bias folded into x0 */
    addi    a4, a4, 4
    ee.vldbc.32 q4, a4
    addi    a4, a4, 4
    ee.vadds.s32 q0, q0, q4
    ee.vadds.s32 q5, q0, q2         /* t0 */
    ee.vsubs.s32 q6, q0, q2         /* t1 */
    ee.vsr.32 q4, q1
    ee.vsr.32 q7, q3
    ee.vsubs.s32 q4, q4, q3         /* t2 = (x1 >> 1) - x3 */
    ee.vadds.s32 q7, q1, q7         /* t3 = x1 + (x3 >> 1) */
    ee.vadds.s32 q0, q5, q7         /* t0 + t3 */
    ee.vadds.s32 q1, q6, q4         /* t1 + t2 */
    ee.vsubs.s32 q2, q6, q4         /* t1 - t2 */
    ee.vsubs.s32 q3, q5, q7         /* t0 - t3 */
    movi    a9, 6
    ssr     a9
    ee.vsr.32 q0, q0                /* v0 + 512 ... v3 + 512 */
    ee.vsr.32 q1, q1
    ee.vsr.32 q2, q2
    ee.vsr.32 q3, q3
    ee.orq  q4, q0, q1
    ee.orq  q5, q2, q3
    ee.orq  q4, q4, q5
    ee.vldbc.32 q5, a4
    ee.movi.32.a q4, a5, 0
    ee.movi.32.a q4, a6, 1
    ee.movi.32.a q4, a7, 2
    ee.movi.32.a q4, a8, 3
    ee.vsubs.s32 q0, q0, q5
    ee.vsubs.s32 q1, q1, q5
    ee.vsubs.s32 q2, q2, q5
    ee.vsubs.s32 q3, q3, q5
    ee.vst.128.ip q0, a3, 16
    ee.vst.128.ip q1, a3, 16
    ee.vst.128.ip q2, a3, 16
    ee.vst.128.ip q3, a3, 16
    or      a5, a5, a6
    or      a7, a7, a8
    or      a5, a5, a7
    srli    a5, a5, 10
    movi    a2, 0
    beqz    a5, 1f
    movi    a2, 1
1:  retw
.Lguard:
    movi    a2, 2
    retw
    .size   vj_pie_idct4, .-vj_pie_idct4
