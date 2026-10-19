/* SPDX-License-Identifier: Apache-2.0 */
/* Write-out kernels in ESP32-S3 PIE assembly, the C of h264bsdWriteOutputBlocks
 * (h264bsd_image.c) is the oracle: bit for bit identical for every residual in s16 range (the standard's inverse
 * transform output is within +-512..767 for valid streams; the C reads a 1280-entry clip table at that index).
 *
 * vj_pie_write_out: per 4x4 block, an empty residual (res[0] == 0xFFFFFF) copies the 4 prediction rows (u32 loads and
 * stores); otherwise the 16 residuals (i32, low 16 bits kept: unzip.16) are added to the zero-extended prediction in
 * s16 lanes (saturating), clipped to 0..255 (max/min) and the low bytes stored 4 at a time.
 * Memory touched: exactly the 4x4 pred bytes (u32 loads at 4-aligned addresses), the 64 residual bytes per block
 * (ee.vld.128: the residual array must be 16-aligned, rounded down silently otherwise: the caller checks), and exactly the
 * 4x4 destination bytes (s32i, 4-aligned).
 */
    .section .rodata.vj_wr_k, "a"
    .align  16
vj_wr_k:
    .rept 8
    .short 255
    .endr
vj_wr_tbl:                          /* x, y of the 16 luma blocks; chroma uses the first 4 */
    .byte 0,0, 4,0, 0,4, 4,4, 8,0, 12,0, 8,4, 12,4, 0,8, 4,8, 0,12, 4,12, 8,8, 12,8, 8,12, 12,12

    .text
    .literal_position

/* a10 pred, a11 dst, a12 dst stride, a13 pred stride, a7 residual block (advanced by 64); q4 zeros, q5 255s */
.macro WBLK
    l32i    a14, a7, 0
    movi    a15, 0xFFFFFF
    bne     a14, a15, 1f
    l32i    a14, a10, 0
    add     a10, a10, a13
    l32i    a15, a10, 0
    add     a10, a10, a13
    s32i    a14, a11, 0
    add     a11, a11, a12
    s32i    a15, a11, 0
    add     a11, a11, a12
    l32i    a14, a10, 0
    add     a10, a10, a13
    l32i    a15, a10, 0
    s32i    a14, a11, 0
    add     a11, a11, a12
    s32i    a15, a11, 0
    addi    a7, a7, 64
    j       3f
1:
    ee.vld.128.ip q0, a7, 16
    ee.vld.128.ip q1, a7, 16
    ee.vld.128.ip q2, a7, 16
    ee.vld.128.ip q3, a7, 16
    ee.vunzip.16 q0, q1            /* q0: rows 0, 1 as s16 */
    ee.vunzip.16 q2, q3            /* q2: rows 2, 3 */
    l32i    a14, a10, 0
    add     a10, a10, a13
    l32i    a15, a10, 0
    add     a10, a10, a13
    ee.movi.32.q q1, a14, 0
    ee.movi.32.q q1, a15, 1
    l32i    a14, a10, 0
    add     a10, a10, a13
    l32i    a15, a10, 0
    ee.movi.32.q q1, a14, 2
    ee.movi.32.q q1, a15, 3
    ee.zero.q q3
    ee.vzip.8 q1, q3               /* q1: pred rows 0, 1 as s16; q3: rows 2, 3 */
    ee.vadds.s16 q0, q0, q1
    ee.vadds.s16 q2, q2, q3
    ee.vmax.s16 q0, q0, q4
    ee.vmax.s16 q2, q2, q4
    ee.vmin.s16 q0, q0, q5
    ee.vmin.s16 q2, q2, q5
    ee.vunzip.8 q0, q2             /* q0: the 16 clipped bytes, row-major */
    ee.movi.32.a q0, a14, 0
    s32i    a14, a11, 0
    add     a11, a11, a12
    ee.movi.32.a q0, a14, 1
    s32i    a14, a11, 0
    add     a11, a11, a12
    ee.movi.32.a q0, a14, 2
    s32i    a14, a11, 0
    add     a11, a11, a12
    ee.movi.32.a q0, a14, 3
    s32i    a14, a11, 0
3:
.endm

/* void vj_pie_write_out(u8 *lum, u8 *cb, u8 *cr, u32 lstride, const u8 *data, const i32 *res) */
    .align  4
    .global vj_pie_write_out
    .type   vj_pie_write_out, @function
vj_pie_write_out:
    entry   a1, 32
    movi    a9, vj_wr_k
    movi    a9, vj_wr_k
    ee.vld.128.ip q5, a9, 0
    ee.zero.q q4
    mov     a12, a5
    movi    a13, 16
    movi    a9, vj_wr_tbl
    movi    a8, 16
.Lwlum:
    l8ui    a14, a9, 0
    l8ui    a15, a9, 1
    addi    a9, a9, 2
    slli    a10, a15, 4
    add     a10, a10, a14
    add     a10, a10, a6
    mull    a11, a15, a12
    add     a11, a11, a14
    add     a11, a11, a2
    WBLK
    addi    a8, a8, -1
    bnez    a8, .Lwlum
    srli    a12, a5, 1
    movi    a13, 8
    addi    a6, a6, 256
    mov     a2, a3
    movi    a5, 2
.Lwplane:
    movi    a9, vj_wr_tbl
    movi    a8, 4
.Lwc:
    l8ui    a14, a9, 0
    l8ui    a15, a9, 1
    addi    a9, a9, 2
    slli    a10, a15, 3
    add     a10, a10, a14
    add     a10, a10, a6
    mull    a11, a15, a12
    add     a11, a11, a14
    add     a11, a11, a2
    WBLK
    addi    a8, a8, -1
    bnez    a8, .Lwc
    addi    a6, a6, 64
    mov     a2, a4
    addi    a5, a5, -1
    bnez    a5, .Lwplane
    retw
    .size   vj_pie_write_out, . - vj_pie_write_out
