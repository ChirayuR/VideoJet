/* Motion-compensation kernels in ESP32-S3 PIE (128-bit SIMD) assembly. The C kernels in h264bsd_reconstruct.c are
 * the oracle: these match them bit for bit (on-board test, see components/videojet/h264bsd/README.md).
 *
 * Arithmetic: samples are XORed with 0x80, which makes them s8 values p - 128. The 6-tap filter (1, -5, 20, 20, -5,
 * 1) taps sum to 32, so on such samples QACC = 16 + sum(tap * (p - 128)) = (sum(tap * p) + 16) - 4096, and
 * EE.SRCMB.S8.QACC (arithmetic shift right, saturate to s8) by 5 gives exactly clip((sum + 16) >> 5) - 128. XOR 0x80
 * turns that back into the clipped u8 sample. A quarter sample (r + n + 1) >> 1 is (16 r' + 16 n' + 16) >> 5 on the
 * same s8 values. The centre sample j sums the 6-tap sums b1' = b1 - 4096 in 16-bit lanes: 512 + sum(tap * b1') =
 * (j1 + 512) - 131072, shifted by 10 and saturated to s8 is clip((j1 + 512) >> 10) - 128. Chroma weights sum to 64:
 * QACC = 32 + sum(w * (p - 128)) shifted by 6 is the bilinear sample - 128.
 *
 * Every kernel reads its window in place: rows from 16-aligned chunks (EE.LD.128.USAR + EE.SRC.Q), so it reads up to
 * 47 bytes past a luma row start and 31 past a chroma row start. Callers pass windows inside the picture; the DPB
 * buffers are 16-aligned with 32 bytes of slack after Cr, and luma is followed by chroma. Prediction rows: 16 bytes
 * per luma row (16-aligned for width 16, 8-aligned for width 8), 8 per chroma row (8-aligned for width 8).
 */
    .section .rodata.vj_pie_k, "a"
    .align  16
vj_pie_k:
    .rept 16
    .byte 0x80                     /* +0  centring */
    .endr
    .rept 16
    .byte 16                       /* +16 luma rounding */
    .endr
    .byte 1, -5, 20, 16            /* +32 luma taps; 16 scales a quarter-sample average */
    .rept 12
    .byte 0
    .endr
    .rept 16
    .byte 32                       /* +48 chroma rounding */
    .endr
    .short 1, -5, 20, 0, 0, 0, 0, 0 /* +64 luma taps, 16-bit lanes */
    .rept 8
    .short 512                     /* +80 centre rounding, 16-bit lanes */
    .endr

    .text
    .literal_position

/* the 16 bytes at \p into \qa, s8-centred with \qc (0x80s); \qt and \t are clobbered, \p is kept */
.macro ROW16 qa, qt, p, t, qc
    mov     \t, \p
    ee.ld.128.usar.ip \qa, \t, 16
    ee.ld.128.usar.ip \qt, \t, 16
    ee.src.q \qa, \qa, \qt
    ee.xorq \qa, \qa, \qc
.endm

/* d row of width \w into \q, s8-centred */
.macro LOADD q, d, w, qc
.if \w == 16
    ee.vld.128.ip \q, \d, 0
.else
    ee.vld.l.64.ip \q, \d, 0
.endif
    ee.xorq \q, \q, \qc
.endm

/* store the low \w bytes of \q at \d, then \d += 16 */
.macro STORED q, d, w
.if \w == 16
    ee.vst.128.ip \q, \d, 16
.else
    ee.vst.l.64.ip \q, \d, 16
.endif
.endm

/* (r' + n' + 1) >> 1 into \qr, from r' in \qr and n' in \qn (s8-centred); QACC starts at 16 (\q16) */
.macro AVG8 qr, qn, q16, qk, sh
    ee.mov.s8.qacc \q16
    ee.vsmulas.s8.qacc \qr, \qk, 3
    ee.vsmulas.s8.qacc \qn, \qk, 3
    ee.srcmb.s8.qacc \qr, \sh, 0
.endm

/*--------------------------------------------------------------------------------------------------------------------
 * vj_pie_luma_hor{0,1,2,3}(const u8 *s, u32 ss, u8 *d, u32 w, u32 h): rows of the horizontal half sample b (0),
 * averaged with G (1: a), H (2: c) or the bytes d holds (3: f, q after vj_pie_luma_mid). s is window column 0 (G - 2),
 * w is 8 or 16.
 */
.macro HROW q, w
    mov     a10, a2
    ee.ld.128.usar.ip q0, a10, 16
    ee.ld.128.usar.ip q1, a10, 16
.if \w == 16
    ee.ld.128.usar.ip q2, a10, 16
    ee.src.q q0, q0, q1            /* window bytes 0..15 */
    ee.src.q q1, q1, q2            /* 16..31 */
.else
    ee.src.q q0, q0, q1            /* bytes 0..15: all that width 8 needs */
.endif
    ee.xorq q0, q0, q7
    ee.xorq q1, q1, q7
    ee.mov.s8.qacc q5
    ee.vsmulas.s8.qacc q0, q6, 0   /* x-2 */
    ee.srci.2q q1, q0, 0
    ee.vsmulas.s8.qacc q0, q6, 1   /* x-1 */
    ee.srci.2q q1, q0, 0
    ee.vsmulas.s8.qacc q0, q6, 2   /* x: G */
.if \q == 1
    mv.qr   q3, q0
.endif
    ee.srci.2q q1, q0, 0
    ee.vsmulas.s8.qacc q0, q6, 2   /* x+1: H */
.if \q == 2
    mv.qr   q3, q0
.endif
    ee.srci.2q q1, q0, 0
    ee.vsmulas.s8.qacc q0, q6, 1   /* x+2 */
    ee.srci.2q q1, q0, 0
    ee.vsmulas.s8.qacc q0, q6, 0   /* x+3 */
    ee.srcmb.s8.qacc q2, a9, 0     /* b - 128 */
.if \q == 3
    LOADD   q3, a4, \w, q7
.endif
.if \q != 0
    AVG8    q2, q3, q5, q6, a9
.endif
    ee.xorq q2, q2, q7
    STORED  q2, a4, \w
    add     a2, a2, a3
.endm

.macro LUMA_HOR q
    .align  4
    .global vj_pie_luma_hor\q
    .type   vj_pie_luma_hor\q, @function
vj_pie_luma_hor\q:
    entry   a1, 32
    movi    a8, vj_pie_k
    ee.vld.128.ip q7, a8, 16       /* 0x80s */
    ee.vld.128.ip q5, a8, 16       /* 16s */
    ee.vld.128.ip q6, a8, 16       /* taps */
    movi    a9, 5
    bbci    a5, 4, .Lh8\q
    loopnez a6, .Lh16e\q
    HROW    \q, 16
.Lh16e\q:
    retw
.Lh8\q:
    loopnez a6, .Lh8e\q
    HROW    \q, 8
.Lh8e\q:
    retw
    .size   vj_pie_luma_hor\q, . - vj_pie_luma_hor\q
.endm

    LUMA_HOR 0
    LUMA_HOR 1
    LUMA_HOR 2
    LUMA_HOR 3

/*--------------------------------------------------------------------------------------------------------------------
 * vj_pie_luma_ver{0,1,2,3}(const u8 *s, u32 ss, u8 *d, u32 w, u32 h): columns of the vertical half sample h (0),
 * averaged with G (1: d), M (2: n) or the bytes d holds (3: e, g, p, r after vj_pie_luma_hor0; i, k after
 * vj_pie_luma_mid). s is window row 0 (G - 2 rows). The h + 5 window rows are first centred into an aligned stack
 * buffer, 16 bytes per row; then each output row is 6 fused MAC + load steps down that buffer.
 */
.macro VROW q, w
    ee.mov.s8.qacc q5
    mov     a11, a10
    ee.vld.128.ip q0, a11, 16                       /* y */
    ee.vsmulas.s8.qacc.ld.incp q1, a11, q0, q6, 0   /* y   * 1,   load y+1 */
    ee.vsmulas.s8.qacc.ld.incp q0, a11, q1, q6, 1   /* y+1 * -5,  load y+2 */
    ee.vsmulas.s8.qacc.ld.incp q1, a11, q0, q6, 2   /* y+2 * 20 (G), load y+3 */
.if \q == 1
    mv.qr   q3, q0
.endif
    ee.vsmulas.s8.qacc.ld.incp q0, a11, q1, q6, 2   /* y+3 * 20 (M), load y+4 */
.if \q == 2
    mv.qr   q3, q1
.endif
    ee.vsmulas.s8.qacc.ld.incp q1, a11, q0, q6, 1   /* y+4 * -5,  load y+5 */
    ee.vsmulas.s8.qacc q1, q6, 0                    /* y+5 * 1 */
    ee.srcmb.s8.qacc q2, a9, 0                      /* h - 128 */
.if \q == 3
    LOADD   q3, a4, \w, q7
.endif
.if \q != 0
    AVG8    q2, q3, q5, q6, a9
.endif
    ee.xorq q2, q2, q7
    STORED  q2, a4, \w
    addi    a10, a10, 16
.endm

.macro LUMA_VER q
    .align  4
    .global vj_pie_luma_ver\q
    .type   vj_pie_luma_ver\q, @function
vj_pie_luma_ver\q:
    entry   a1, 368                /* 21 rows x 16 B at a1 */
    movi    a8, vj_pie_k
    ee.vld.128.ip q7, a8, 16
    ee.vld.128.ip q5, a8, 16
    ee.vld.128.ip q6, a8, 16
    movi    a9, 5
    addi    a10, a6, 5
    mov     a11, a1
    loopnez a10, .Lvpe\q
    ROW16   q0, q1, a2, a12, q7
    ee.vst.128.ip q0, a11, 16
    add     a2, a2, a3
.Lvpe\q:
    mov     a10, a1                /* row y of the centred window */
    bbci    a5, 4, .Lv8\q
    loopnez a6, .Lv16e\q
    VROW    \q, 16
.Lv16e\q:
    retw
.Lv8\q:
    loopnez a6, .Lv8e\q
    VROW    \q, 8
.Lv8e\q:
    retw
    .size   vj_pie_luma_ver\q, . - vj_pie_luma_ver\q
.endm

    LUMA_VER 0
    LUMA_VER 1
    LUMA_VER 2
    LUMA_VER 3

/*--------------------------------------------------------------------------------------------------------------------
 * vj_pie_luma_mid(const u8 *s, u32 ss, u8 *d, u32 w, u32 h): the centre sample j. s is the window at (G - 2, G - 2).
 * Pass 1: for each of the h + 5 window rows, the horizontal 6-tap sums b1' of the s8-centred samples (b1 - 4096,
 * |b1'| < 6700) in 16-bit lanes, stored per 8-column group: group 0 rows at a1, group 1 (w 16) after h + 5 rows.
 * Pass 2: the vertical 6-tap of b1' per group (EE.SRCMB.S8.QACC leaves s8-saturated values in 16-bit lanes), then
 * EE.VUNZIP.8 packs the groups' low bytes.
 * Headroom: the S16-accumulated QACC lanes (40-bit) are read by SRCMB.S8, exact while |512 + j1'| < 2^19; the worst case
 * (20, -5, ... taps over b1' = +-6700) is 345,272, so there is no overflow.
 */
/* 6-tap of the 16-bit vector pair {\qhi:\qlo} (shifted in place, lane by lane) into \qo; QACC starts at 0 */
.macro HSUM16 qlo, qhi, qo, z
    ee.zero.qacc
    ee.vsmulas.s16.qacc \qlo, q6, 0
    ee.srci.2q \qhi, \qlo, 1
    ee.vsmulas.s16.qacc \qlo, q6, 1
    ee.srci.2q \qhi, \qlo, 1
    ee.vsmulas.s16.qacc \qlo, q6, 2
    ee.srci.2q \qhi, \qlo, 1
    ee.vsmulas.s16.qacc \qlo, q6, 2
    ee.srci.2q \qhi, \qlo, 1
    ee.vsmulas.s16.qacc \qlo, q6, 1
    ee.srci.2q \qhi, \qlo, 1
    ee.vsmulas.s16.qacc \qlo, q6, 0
    ee.srcmb.s16.qacc \qo, \z, 0
.endm

/* vertical 6-tap of 16-bit rows at \p (16 B apart) into \qo, s8-saturated after >> 10; QACC starts at 512 (q5) */
.macro VSUM16 p, qo
    ee.mov.s16.qacc q5
    mov     a11, \p
    ee.vld.128.ip q0, a11, 16
    ee.vsmulas.s16.qacc.ld.incp q1, a11, q0, q6, 0
    ee.vsmulas.s16.qacc.ld.incp q0, a11, q1, q6, 1
    ee.vsmulas.s16.qacc.ld.incp q1, a11, q0, q6, 2
    ee.vsmulas.s16.qacc.ld.incp q0, a11, q1, q6, 2
    ee.vsmulas.s16.qacc.ld.incp q1, a11, q0, q6, 1
    ee.vsmulas.s16.qacc q1, q6, 0
    ee.srcmb.s8.qacc \qo, a9, 0
.endm

    .align  4
    .global vj_pie_luma_mid
    .type   vj_pie_luma_mid, @function
vj_pie_luma_mid:
    entry   a1, 704                /* 2 groups x 21 rows x 16 B at a1 */
    movi    a8, vj_pie_k
    ee.vld.128.ip q7, a8, 64       /* 0x80s; a8 -> 16-bit taps */
    ee.vld.128.ip q6, a8, 16       /* 16-bit taps; a8 -> 512s */
    ee.zero.q q5
    movi    a9, 0
    addi    a10, a6, 5             /* window rows */
    slli    a13, a10, 4            /* group stride */
    mov     a11, a1                /* group 0 row */
    bbci    a5, 4, .Lmp8
    loopnez a10, .Lmp16e
    mov     a12, a2
    ee.ld.128.usar.ip q0, a12, 16
    ee.ld.128.usar.ip q1, a12, 16
    ee.ld.128.usar.ip q2, a12, 16
    ee.src.q q0, q0, q1            /* window bytes 0..15 */
    ee.src.q q1, q1, q2            /* 16..31 */
    ee.xorq q0, q0, q7
    ee.xorq q1, q1, q7
    ee.vcmp.lt.s8 q2, q0, q5
    ee.vzip.8 q0, q2               /* q0: bytes 0..7, q2: 8..15, as 16-bit */
    ee.vcmp.lt.s8 q3, q1, q5
    ee.vzip.8 q1, q3               /* q1: bytes 16..23 */
    mv.qr   q3, q2
    HSUM16  q0, q3, q4, a9         /* columns 0..7 */
    ee.vst.128.ip q4, a11, 0
    HSUM16  q2, q1, q4, a9         /* columns 8..15 */
    add     a12, a11, a13
    ee.vst.128.ip q4, a12, 0
    addi    a11, a11, 16
    add     a2, a2, a3
.Lmp16e:
    j       .Lmpass2
.Lmp8:
    loopnez a10, .Lmp8e
    mov     a12, a2
    ee.ld.128.usar.ip q0, a12, 16
    ee.ld.128.usar.ip q1, a12, 16
    ee.src.q q0, q0, q1
    ee.xorq q0, q0, q7
    ee.vcmp.lt.s8 q2, q0, q5
    ee.vzip.8 q0, q2
    HSUM16  q0, q2, q4, a9         /* columns 0..7 */
    ee.vst.128.ip q4, a11, 16
    add     a2, a2, a3
.Lmp8e:
.Lmpass2:
    ee.vld.128.ip q5, a8, 0        /* 512s */
    movi    a9, 10
    mov     a10, a1                /* group 0, row y */
    bbci    a5, 4, .Lmr8
    loopnez a6, .Lmr16e
    VSUM16  a10, q2                /* j - 128, columns 0..7 */
    add     a12, a10, a13
    VSUM16  a12, q3                /* columns 8..15 */
    ee.vunzip.8 q2, q3
    ee.xorq q2, q2, q7
    ee.vst.128.ip q2, a4, 16
    addi    a10, a10, 16
.Lmr16e:
    retw
.Lmr8:
    loopnez a6, .Lmr8e
    VSUM16  a10, q2
    ee.vunzip.8 q2, q3
    ee.xorq q2, q2, q7
    ee.vst.l.64.ip q2, a4, 16
    addi    a10, a10, 16
.Lmr8e:
    retw
    .size   vj_pie_luma_mid, . - vj_pie_luma_mid

/*--------------------------------------------------------------------------------------------------------------------
 * vj_pie_luma_copy(const u8 *s, u32 ss, u8 *d, u32 w, u32 h): the full-sample block.
 */
.macro CROW w
    mov     a10, a2
    ee.ld.128.usar.ip q0, a10, 16
    ee.ld.128.usar.ip q1, a10, 16
    ee.src.q q0, q0, q1
    STORED  q0, a4, \w
    add     a2, a2, a3
.endm

    .align  4
    .global vj_pie_luma_copy
    .type   vj_pie_luma_copy, @function
vj_pie_luma_copy:
    entry   a1, 32
    bbci    a5, 4, .Lc8
    loopnez a6, .Lc16e
    CROW    16
.Lc16e:
    retw
.Lc8:
    loopnez a6, .Lc8e
    CROW    8
.Lc8e:
    retw
    .size   vj_pie_luma_copy, . - vj_pie_luma_copy

/*--------------------------------------------------------------------------------------------------------------------
 * vj_pie_chroma(const u8 *s, u32 ss, u8 *d, u32 w, u32 h, u32 wts, u32 ps): both planes, Cb window at s and Cr at
 * s + ps, Cb rows at d and Cr at d + 64 (8 bytes per row). w is 8 or 4, h 8, 4 or 2. wts packs the bilinear weights
 * A | B << 8 | C << 16 | D << 24 (A = (8 - xFrac)(8 - yFrac), B = xFrac (8 - yFrac), C = (8 - xFrac) yFrac,
 * D = xFrac yFrac), so B != 0 exactly when xFrac is and C != 0 when yFrac is. Four row loops: copy, horizontal
 * (A, B; h rows), vertical (A, C; h + 1 rows) and bilinear (h + 1 rows). A 4-wide row may be 4-aligned only: it is
 * stored as one word.
 */
.macro CSTORE w
.if \w == 8
    ee.vst.l.64.ip q4, a11, 8
.else
    ee.movi.32.a q4, a8, 0
    s32i    a8, a11, 0
    addi    a11, a11, 8
.endif
.endm

/* rows of one plane: a10 source row, a11 destination row, a6 rows; kind 0 copy, 1 hor, 2 ver, 3 bilinear */
.macro CROWS kind, w, lbl
.if \kind >= 2
    ROW16   q0, q1, a10, a8, q7    /* row 0 */
.endif
.if \kind == 3
    mv.qr   q1, q0
    ee.srci.2q q2, q1, 0           /* row 0 from x+1 */
.endif
    loopnez a6, .Lce\lbl
.if \kind == 0
    mov     a8, a10
    ee.ld.128.usar.ip q4, a8, 16
    ee.ld.128.usar.ip q2, a8, 16
    ee.src.q q4, q4, q2
    add     a10, a10, a3
    CSTORE  \w
.elseif \kind == 1
    ROW16   q0, q1, a10, a8, q7
    mv.qr   q1, q0
    ee.srci.2q q2, q1, 0
    ee.mov.s8.qacc q5
    ee.vsmulas.s8.qacc q0, q6, 0
    ee.vsmulas.s8.qacc q1, q6, 1
    ee.srcmb.s8.qacc q4, a9, 0
    ee.xorq q4, q4, q7
    add     a10, a10, a3
    CSTORE  \w
.elseif \kind == 2
    add     a10, a10, a3
    ROW16   q2, q3, a10, a8, q7    /* row y+1 */
    ee.mov.s8.qacc q5
    ee.vsmulas.s8.qacc q0, q6, 0
    ee.vsmulas.s8.qacc q2, q6, 2
    ee.srcmb.s8.qacc q4, a9, 0
    ee.xorq q4, q4, q7
    mv.qr   q0, q2
    CSTORE  \w
.else
    add     a10, a10, a3
    ROW16   q2, q3, a10, a8, q7    /* row y+1 */
    mv.qr   q3, q2
    ee.srci.2q q4, q3, 0
    ee.mov.s8.qacc q5
    ee.vsmulas.s8.qacc q0, q6, 0
    ee.vsmulas.s8.qacc q1, q6, 1
    ee.vsmulas.s8.qacc q2, q6, 2
    ee.vsmulas.s8.qacc q3, q6, 3
    ee.srcmb.s8.qacc q4, a9, 0
    ee.xorq q4, q4, q7
    mv.qr   q0, q2
    mv.qr   q1, q3
    CSTORE  \w
.endif
.Lce\lbl:
    j       .Lcpnext
.endm

    .align  4
    .global vj_pie_chroma
    .type   vj_pie_chroma, @function
vj_pie_chroma:
    entry   a1, 32
    l32i    a15, a1, 32            /* ps */
    movi    a8, vj_pie_k
    ee.vld.128.ip q7, a8, 48       /* 0x80s; a8 -> chroma rounding */
    ee.vld.128.ip q5, a8, 0        /* 32s */
    ee.zero.q q6
    ee.movi.32.q q6, a7, 0         /* weights in lanes 0..3 */
    movi    a9, 6
    extui   a12, a7, 8, 8          /* B */
    extui   a13, a7, 16, 8         /* C */
    movi    a14, 0                 /* 2 * kind + (w == 8) */
    beqz    a12, 1f
    movi    a14, 2
1:  beqz    a13, 2f
    addi    a14, a14, 4
2:  bbci    a5, 3, 3f
    addi    a14, a14, 1
3:  movi    a12, 2                 /* planes */
.Lcplane:
    mov     a10, a2
    mov     a11, a4
    beqi    a14, 0, .Lck0w4
    beqi    a14, 1, .Lck0w8
    beqi    a14, 2, .Lck1w4
    beqi    a14, 3, .Lck1w8
    beqi    a14, 4, .Lck2w4
    beqi    a14, 5, .Lck2w8
    beqi    a14, 6, .Lck3w4
    j       .Lck3w8
.Lck0w4:
    CROWS   0, 4, k0w4
.Lck0w8:
    CROWS   0, 8, k0w8
.Lck1w4:
    CROWS   1, 4, k1w4
.Lck1w8:
    CROWS   1, 8, k1w8
.Lck2w4:
    CROWS   2, 4, k2w4
.Lck2w8:
    CROWS   2, 8, k2w8
.Lck3w4:
    CROWS   3, 4, k3w4
.Lck3w8:
    CROWS   3, 8, k3w8
.Lcpnext:
    add     a2, a2, a15            /* Cr */
    addi    a4, a4, 64
    addi    a12, a12, -1
    bnez    a12, .Lcplane
    retw
    .size   vj_pie_chroma, . - vj_pie_chroma
