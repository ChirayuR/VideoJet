/* test_cavlc: the CAVLC symbol decoders of h264bsd_cavlc.c (coeff_token, total_zeros, run_before) against the original
 * h264bsd routines below, over every input: every 16-bit window x every nC of -1..16, every 9-bit window x totalCoeff
 * 1..15 (luma) and 1..3 (chroma DC), every 11-bit window x zerosLeft 0..31 (decoded: 1..15). The whole information
 * word is compared: the symbol, the code length, and 0 for an invalid code.
 * Then the macroblock residual decoder h264bsdDecodeResidualCavlc against the per-block decoder it replaced (main
 * 2ea6242's DecodeResidual, DetermineNc and h264bsdDecodeResidualBlockCavlc, below), on random streams (argv[1] cases,
 * default 400,000): uniform and biased bits, zero runs (escape codes), buffers that end mid-block, random start bits,
 * neighbour totalCoeff, cbp and Intra16x16. The status must match, and for a decoded residual the whole residual_t
 * (levels, totalCoeff, coeffMap) and the bit position and window after it.
 * Prints the case counts; exit 0 when every case matches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264bsd_cavlc.c" /* the decoders under test are static */
#include "h264bsd_neighbour.h"

/* The reference: DecodeCoeffToken, DecodeTotalZeros and DecodeRunBefore and their tables from h264bsd_cavlc.c at
 * b6f006c, before the table-driven decode; only the names carry a ref/Ref prefix. */
/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 * Modified for VideoJet: see h264bsd/README.md, Local changes. Copied from
 * h264bsd_cavlc.c at b6f006c into host/test_cavlc.c as its oracle, names prefixed.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define REF_INFO(vlc) (((vlc) >> 4) & 0xF)

/* VLC tables for coeff_token. Because of long codes (max. 16 bits) some of the
 * tables have been splitted into multiple separate tables. Each array/table
 * element has the following structure:
 * [5 bits for tot.coeff.] [6 bits for tr.ones] [5 bits for VLC length]
 * If there is a 0x0000 value, it means that there is not corresponding VLC
 * codeword for that index. */

/* VLC lengths up to 6 bits, 0 <= nC < 2 */
static const u16 refCoeffToken0_0[32] = {
    0x0000,0x0000,0x0000,0x2066,0x1026,0x0806,0x1865,0x1865,
    0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,
    0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,
    0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,0x0822};

/* VLC lengths up to 10 bits, 0 <= nC < 2 */
static const u16 refCoeffToken0_1[48] = {
    0x0000,0x0000,0x0000,0x0000,0x406a,0x304a,0x282a,0x200a,
    0x3869,0x3869,0x2849,0x2849,0x2029,0x2029,0x1809,0x1809,
    0x3068,0x3068,0x3068,0x3068,0x2048,0x2048,0x2048,0x2048,
    0x1828,0x1828,0x1828,0x1828,0x1008,0x1008,0x1008,0x1008,
    0x2867,0x2867,0x2867,0x2867,0x2867,0x2867,0x2867,0x2867,
    0x1847,0x1847,0x1847,0x1847,0x1847,0x1847,0x1847,0x1847};

/* VLC lengths up to 14 bits, 0 <= nC < 2 */
static const u16 refCoeffToken0_2[56] = {
    0x606e,0x584e,0x502e,0x500e,0x586e,0x504e,0x482e,0x480e,
    0x400d,0x400d,0x484d,0x484d,0x402d,0x402d,0x380d,0x380d,
    0x506d,0x506d,0x404d,0x404d,0x382d,0x382d,0x300d,0x300d,
    0x486b,0x486b,0x486b,0x486b,0x486b,0x486b,0x486b,0x486b,
    0x384b,0x384b,0x384b,0x384b,0x384b,0x384b,0x384b,0x384b,
    0x302b,0x302b,0x302b,0x302b,0x302b,0x302b,0x302b,0x302b,
    0x280b,0x280b,0x280b,0x280b,0x280b,0x280b,0x280b,0x280b};

/* VLC lengths up to 16 bits, 0 <= nC < 2 */
static const u16 refCoeffToken0_3[32] = {
    0x0000,0x0000,0x682f,0x682f,0x8010,0x8050,0x8030,0x7810,
    0x8070,0x7850,0x7830,0x7010,0x7870,0x7050,0x7030,0x6810,
    0x706f,0x706f,0x684f,0x684f,0x602f,0x602f,0x600f,0x600f,
    0x686f,0x686f,0x604f,0x604f,0x582f,0x582f,0x580f,0x580f};

/* VLC lengths up to 6 bits, 2 <= nC < 4 */
static const u16 refCoeffToken2_0[32] = {
    0x0000,0x0000,0x0000,0x0000,0x3866,0x2046,0x2026,0x1006,
    0x3066,0x1846,0x1826,0x0806,0x2865,0x2865,0x1025,0x1025,
    0x2064,0x2064,0x2064,0x2064,0x1864,0x1864,0x1864,0x1864,
    0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,0x1043};

/* VLC lengths up to 9 bits, 2 <= nC < 4 */
static const u16 refCoeffToken2_1[32] = {
    0x0000,0x0000,0x0000,0x0000,0x4869,0x3849,0x3829,0x3009,
    0x2808,0x2808,0x3048,0x3048,0x3028,0x3028,0x2008,0x2008,
    0x4067,0x4067,0x4067,0x4067,0x2847,0x2847,0x2847,0x2847,
    0x2827,0x2827,0x2827,0x2827,0x1807,0x1807,0x1807,0x1807};

/* VLC lengths up to 14 bits, 2 <= nC < 4 */
static const u16 refCoeffToken2_2[128] = {
    0x0000,0x0000,0x786d,0x786d,0x806e,0x804e,0x802e,0x800e,
    0x782e,0x780e,0x784e,0x702e,0x704d,0x704d,0x700d,0x700d,
    0x706d,0x706d,0x684d,0x684d,0x682d,0x682d,0x680d,0x680d,
    0x686d,0x686d,0x604d,0x604d,0x602d,0x602d,0x600d,0x600d,
    0x580c,0x580c,0x580c,0x580c,0x584c,0x584c,0x584c,0x584c,
    0x582c,0x582c,0x582c,0x582c,0x500c,0x500c,0x500c,0x500c,
    0x606c,0x606c,0x606c,0x606c,0x504c,0x504c,0x504c,0x504c,
    0x502c,0x502c,0x502c,0x502c,0x480c,0x480c,0x480c,0x480c,
    0x586b,0x586b,0x586b,0x586b,0x586b,0x586b,0x586b,0x586b,
    0x484b,0x484b,0x484b,0x484b,0x484b,0x484b,0x484b,0x484b,
    0x482b,0x482b,0x482b,0x482b,0x482b,0x482b,0x482b,0x482b,
    0x400b,0x400b,0x400b,0x400b,0x400b,0x400b,0x400b,0x400b,
    0x506b,0x506b,0x506b,0x506b,0x506b,0x506b,0x506b,0x506b,
    0x404b,0x404b,0x404b,0x404b,0x404b,0x404b,0x404b,0x404b,
    0x402b,0x402b,0x402b,0x402b,0x402b,0x402b,0x402b,0x402b,
    0x380b,0x380b,0x380b,0x380b,0x380b,0x380b,0x380b,0x380b};

/* VLC lengths up to 6 bits, 4 <= nC < 8 */
static const u16 refCoeffToken4_0[64] = {
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x1806,0x3846,0x3826,0x1006,0x4866,0x3046,0x3026,0x0806,
    0x2825,0x2825,0x2845,0x2845,0x2025,0x2025,0x2045,0x2045,
    0x1825,0x1825,0x4065,0x4065,0x1845,0x1845,0x1025,0x1025,
    0x3864,0x3864,0x3864,0x3864,0x3064,0x3064,0x3064,0x3064,
    0x2864,0x2864,0x2864,0x2864,0x2064,0x2064,0x2064,0x2064,
    0x1864,0x1864,0x1864,0x1864,0x1044,0x1044,0x1044,0x1044,
    0x0824,0x0824,0x0824,0x0824,0x0004,0x0004,0x0004,0x0004};

/* VLC lengths up to 10 bits, 4 <= nC < 8 */
static const u16 refCoeffToken4_1[128] = {
    0x0000,0x800a,0x806a,0x804a,0x802a,0x780a,0x786a,0x784a,
    0x782a,0x700a,0x706a,0x704a,0x702a,0x680a,0x6829,0x6829,
    0x6009,0x6009,0x6849,0x6849,0x6029,0x6029,0x5809,0x5809,
    0x6869,0x6869,0x6049,0x6049,0x5829,0x5829,0x5009,0x5009,
    0x6068,0x6068,0x6068,0x6068,0x5848,0x5848,0x5848,0x5848,
    0x5028,0x5028,0x5028,0x5028,0x4808,0x4808,0x4808,0x4808,
    0x5868,0x5868,0x5868,0x5868,0x5048,0x5048,0x5048,0x5048,
    0x4828,0x4828,0x4828,0x4828,0x4008,0x4008,0x4008,0x4008,
    0x3807,0x3807,0x3807,0x3807,0x3807,0x3807,0x3807,0x3807,
    0x3007,0x3007,0x3007,0x3007,0x3007,0x3007,0x3007,0x3007,
    0x4847,0x4847,0x4847,0x4847,0x4847,0x4847,0x4847,0x4847,
    0x2807,0x2807,0x2807,0x2807,0x2807,0x2807,0x2807,0x2807,
    0x5067,0x5067,0x5067,0x5067,0x5067,0x5067,0x5067,0x5067,
    0x4047,0x4047,0x4047,0x4047,0x4047,0x4047,0x4047,0x4047,
    0x4027,0x4027,0x4027,0x4027,0x4027,0x4027,0x4027,0x4027,
    0x2007,0x2007,0x2007,0x2007,0x2007,0x2007,0x2007,0x2007};

/* fixed 6 bit length VLC, nC <= 8 */
static const u16 refCoeffToken8[64] = {
    0x0806,0x0826,0x0000,0x0006,0x1006,0x1026,0x1046,0x0000,
    0x1806,0x1826,0x1846,0x1866,0x2006,0x2026,0x2046,0x2066,
    0x2806,0x2826,0x2846,0x2866,0x3006,0x3026,0x3046,0x3066,
    0x3806,0x3826,0x3846,0x3866,0x4006,0x4026,0x4046,0x4066,
    0x4806,0x4826,0x4846,0x4866,0x5006,0x5026,0x5046,0x5066,
    0x5806,0x5826,0x5846,0x5866,0x6006,0x6026,0x6046,0x6066,
    0x6806,0x6826,0x6846,0x6866,0x7006,0x7026,0x7046,0x7066,
    0x7806,0x7826,0x7846,0x7866,0x8006,0x8026,0x8046,0x8066};

/* VLC lengths up to 3 bits, nC == -1 */
static const u16 refCoeffTokenMinus1_0[8] = {
    0x0000,0x1043,0x0002,0x0002,0x0821,0x0821,0x0821,0x0821};

/* VLC lengths up to 8 bits, nC == -1 */
static const u16 refCoeffTokenMinus1_1[32] = {
    0x2067,0x2067,0x2048,0x2028,0x1847,0x1847,0x1827,0x1827,
    0x2006,0x2006,0x2006,0x2006,0x1806,0x1806,0x1806,0x1806,
    0x1006,0x1006,0x1006,0x1006,0x1866,0x1866,0x1866,0x1866,
    0x1026,0x1026,0x1026,0x1026,0x0806,0x0806,0x0806,0x0806};

/* VLC tables for total_zeros. One table containing longer code, refTotalZeros_1,
 * has been broken into two separate tables. Table elements have the
 * following structure:
 * [4 bits for info] [4 bits for VLC length] */

/* VLC lengths up to 5 bits */
static const u8 refTotalZeros_1_0[32] = {
    0x00,0x00,0x65,0x55,0x44,0x44,0x34,0x34,
    0x23,0x23,0x23,0x23,0x13,0x13,0x13,0x13,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01};

/* VLC lengths up to 9 bits */
static const u8 refTotalZeros_1_1[32] = {
    0x00,0xf9,0xe9,0xd9,0xc8,0xc8,0xb8,0xb8,
    0xa7,0xa7,0xa7,0xa7,0x97,0x97,0x97,0x97,
    0x86,0x86,0x86,0x86,0x86,0x86,0x86,0x86,
    0x76,0x76,0x76,0x76,0x76,0x76,0x76,0x76};

static const u8 refTotalZeros_2[64] = {
    0xe6,0xd6,0xc6,0xb6,0xa5,0xa5,0x95,0x95,
    0x84,0x84,0x84,0x84,0x74,0x74,0x74,0x74,
    0x64,0x64,0x64,0x64,0x54,0x54,0x54,0x54,
    0x43,0x43,0x43,0x43,0x43,0x43,0x43,0x43,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23,
    0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13,
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03};

static const u8 refTotalZeros_3[64] = {
    0xd6,0xb6,0xc5,0xc5,0xa5,0xa5,0x95,0x95,
    0x84,0x84,0x84,0x84,0x54,0x54,0x54,0x54,
    0x44,0x44,0x44,0x44,0x04,0x04,0x04,0x04,
    0x73,0x73,0x73,0x73,0x73,0x73,0x73,0x73,
    0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23,
    0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13};

static const u8 refTotalZeros_4[32] = {
    0xc5,0xb5,0xa5,0x05,0x94,0x94,0x74,0x74,
    0x34,0x34,0x24,0x24,0x83,0x83,0x83,0x83,
    0x63,0x63,0x63,0x63,0x53,0x53,0x53,0x53,
    0x43,0x43,0x43,0x43,0x13,0x13,0x13,0x13};

static const u8 refTotalZeros_5[32] = {
    0xb5,0x95,0xa4,0xa4,0x84,0x84,0x24,0x24,
    0x14,0x14,0x04,0x04,0x73,0x73,0x73,0x73,
    0x63,0x63,0x63,0x63,0x53,0x53,0x53,0x53,
    0x43,0x43,0x43,0x43,0x33,0x33,0x33,0x33};

static const u8 refTotalZeros_6[64] = {
    0xa6,0x06,0x15,0x15,0x84,0x84,0x84,0x84,
    0x93,0x93,0x93,0x93,0x93,0x93,0x93,0x93,
    0x73,0x73,0x73,0x73,0x73,0x73,0x73,0x73,
    0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,
    0x53,0x53,0x53,0x53,0x53,0x53,0x53,0x53,
    0x43,0x43,0x43,0x43,0x43,0x43,0x43,0x43,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23};

static const u8 refTotalZeros_7[64] = {
    0x96,0x06,0x15,0x15,0x74,0x74,0x74,0x74,
    0x83,0x83,0x83,0x83,0x83,0x83,0x83,0x83,
    0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,
    0x43,0x43,0x43,0x43,0x43,0x43,0x43,0x43,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52};

static const u8 refTotalZeros_8[64] = {
    0x86,0x06,0x25,0x25,0x14,0x14,0x14,0x14,
    0x73,0x73,0x73,0x73,0x73,0x73,0x73,0x73,
    0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42};

static const u8 refTotalZeros_9[64] = {
    0x16,0x06,0x75,0x75,0x24,0x24,0x24,0x24,
    0x53,0x53,0x53,0x53,0x53,0x53,0x53,0x53,
    0x62,0x62,0x62,0x62,0x62,0x62,0x62,0x62,
    0x62,0x62,0x62,0x62,0x62,0x62,0x62,0x62,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,
    0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32};

static const u8 refTotalZeros_10[32] = {
    0x15,0x05,0x64,0x64,0x23,0x23,0x23,0x23,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32};

static const u8 refTotalZeros_11[16] = {
    0x04,0x14,0x23,0x23,0x33,0x33,0x53,0x53,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41};

static const u8 refTotalZeros_12[16] = {
    0x04,0x14,0x43,0x43,0x22,0x22,0x22,0x22,
    0x31,0x31,0x31,0x31,0x31,0x31,0x31,0x31};

static const u8 refTotalZeros_13[8] = {0x03,0x13,0x32,0x32,0x21,0x21,0x21,0x21};

static const u8 refTotalZeros_14[4] = {0x02,0x12,0x21,0x21};

/* VLC tables for run_before. Table elements have the following structure:
 * [4 bits for info] [4bits for VLC length]
 */

static const u8 refRunBefore_6[8] = {0x13,0x23,0x43,0x33,0x63,0x53,0x02,0x02};

static const u8 refRunBefore_5[8] = {0x53,0x43,0x33,0x23,0x12,0x12,0x02,0x02};

static const u8 refRunBefore_4[8] = {0x43,0x33,0x22,0x22,0x12,0x12,0x02,0x02};

static const u8 refRunBefore_3[4] = {0x32,0x22,0x12,0x02};

static const u8 refRunBefore_2[4] = {0x22,0x12,0x01,0x01};

static const u8 refRunBefore_1[2] = {0x11,0x01};

static u32 RefDecodeCoeffToken(u32 bits, u32 nc)
{

/* Variables */

    u32 value;

/* Code */

    /* standard defines that nc for decoding of chroma dc coefficients is -1,
     * represented by u32 here -> -1 maps to 2^32 - 1 */
    ASSERT(nc <= 16 || nc == (u32)(-1));

    if (nc < 2)
    {
        if (bits >= 0x8000)
        {
            value = 0x0001;
        }
        else if (bits >= 0x0C00)
            value = refCoeffToken0_0[bits >> 10];
        else if (bits >= 0x0100)
            value = refCoeffToken0_1[bits >> 6];
        else if (bits >= 0x0020)
            value = refCoeffToken0_2[(bits>>2)-8];
        else
            value = refCoeffToken0_3[bits];
    }
    else if (nc < 4)
    {
        if (bits >= 0x8000)
        {
            value = bits & 0x4000 ? 0x0002 : 0x0822;
        }
        else if (bits >= 0x1000)
            value = refCoeffToken2_0[bits >> 10];
        else if (bits >= 0x0200)
            value = refCoeffToken2_1[bits >> 7];
        else
            value = refCoeffToken2_2[bits>>2];
    }
    else if (nc < 8)
    {
        value = refCoeffToken4_0[bits >> 10];
        if (!value)
            value = refCoeffToken4_1[bits>>6];
    }
    else if (nc <= 16)
    {
        value = refCoeffToken8[bits>>10];
    }
    else
    {
        value = refCoeffTokenMinus1_0[bits >> 13];
        if (!value)
            value = refCoeffTokenMinus1_1[bits>>8];
    }

    return(value);

}

static u32 RefDecodeTotalZeros(u32 bits, u32 totalCoeff, u32 isChromaDC)
{

/* Variables */

    u32 value = 0x0;

/* Code */

    ASSERT(totalCoeff);

    if (!isChromaDC)
    {
        ASSERT(totalCoeff < 16);
        switch (totalCoeff)
        {
            case 1:
                value = refTotalZeros_1_0[bits >> 4];
                if (!value)
                    value = refTotalZeros_1_1[bits];
                break;

            case 2:
                value = refTotalZeros_2[bits >> 3];
                break;

            case 3:
                value = refTotalZeros_3[bits >> 3];
                break;

            case 4:
                value = refTotalZeros_4[bits >> 4];
                break;

            case 5:
                value = refTotalZeros_5[bits >> 4];
                break;

            case 6:
                value = refTotalZeros_6[bits >> 3];
                break;

            case 7:
                value = refTotalZeros_7[bits >> 3];
                break;

            case 8:
                value = refTotalZeros_8[bits >> 3];
                break;

            case 9:
                value = refTotalZeros_9[bits >> 3];
                break;

            case 10:
                value = refTotalZeros_10[bits >> 4];
                break;

            case 11:
                value = refTotalZeros_11[bits >> 5];
                break;

            case 12:
                value = refTotalZeros_12[bits >> 5];
                break;

            case 13:
                value = refTotalZeros_13[bits >> 6];
                break;

            case 14:
                value = refTotalZeros_14[bits >> 7];
                break;

            default: /* case 15 */
                value = (bits >> 8) ? 0x11 : 0x01;
                break;
        }
    }
    else
    {
        ASSERT(totalCoeff < 4);
        bits >>= 6;
        if (bits > 3)
            value = 0x01;
        else
        {
            if (totalCoeff == 3)
                value = 0x11;
            else if (bits > 1)
            {
                value = 0x12;
            }
            else if (totalCoeff == 2)
                value = 0x22;
            else if (bits)
                value = 0x23;
            else
                value = 0x33;
        }
    }

    return(value);

}

static u32 RefDecodeRunBefore(u32 bits, u32 zerosLeft)
{

/* Variables */

    u32 value = 0x0;

/* Code */

    switch (zerosLeft)
    {
        case 1:
            value = refRunBefore_1[bits>>10];
            break;

        case 2:
            value = refRunBefore_2[bits>>9];
            break;

        case 3:
            value = refRunBefore_3[bits>>9];
            break;

        case 4:
            value = refRunBefore_4[bits>>8];
            break;

        case 5:
            value = refRunBefore_5[bits>>8];
            break;

        case 6:
            value = refRunBefore_6[bits>>8];
            break;

        default:
            if (bits >= 0x100)
                value = ((7-(bits>>8))<<4)+0x3;
            else if (bits >= 0x80)
                value = 0x74;
            else if (bits >= 0x40)
                value = 0x85;
            else if (bits >= 0x20)
                value = 0x96;
            else if (bits >= 0x10)
                value = 0xa7;
            else if (bits >= 0x8)
                value = 0xb8;
            else if (bits >= 0x4)
                value = 0xc9;
            else if (bits >= 0x2)
                value = 0xdA;
            else if (bits)
                value = 0xeB;
            if (REF_INFO(value) > zerosLeft)
                value = 0;
            break;
    }

    return(value);

}


/* ---- The macroblock residual oracle: h264bsdDecodeResidualBlockCavlc and its macros (h264bsd_cavlc.c), DecodeResidual
 * and DetermineNc (h264bsd_macroblock_layer.c), as of main 2ea6242; names prefixed, the neighbour tables of
 * h264bsd_neighbour.c inlined, DecodeResidual given is16x16 instead of the mbType it tested. ---- */

#define REF_BUFFER_INIT(value, bits) \
{ \
    bits = 32; \
    value = h264bsdShowBits32(pStrmData); \
}

#define REF_BUFFER_SHOW(value, bits, outVal, numBits) \
{ \
    if (bits < (numBits)) \
    { \
        if(h264bsdFlushBits(pStrmData,32-bits) == END_OF_STREAM) \
            return(HANTRO_NOK); \
        value = h264bsdShowBits32(pStrmData); \
        bits = 32; \
    } \
    (outVal) = value >> (32 - (numBits)); \
}

#define REF_BUFFER_FLUSH(value, bits, numBits) \
{ \
    value <<= (numBits); \
    bits -= (numBits); \
}

#define REF_BUFFER_GET(value, bits, outVal, numBits) \
{ \
    if (bits < (numBits)) \
    { \
        if(h264bsdFlushBits(pStrmData,32-bits) == END_OF_STREAM) \
            return(HANTRO_NOK); \
        value = h264bsdShowBits32(pStrmData); \
        bits = 32; \
    } \
    (outVal) = value >> (32 - (numBits)); \
    value <<= (numBits); \
    bits -= (numBits); \
}

static u32 RefDecodeLevelPrefix(u32 bits)
{
    if (!bits)
        return(VLC_NOT_FOUND);
    return((u32)__builtin_clz(bits) - 16);
}

/* coverage of the oracle's decodes: level_prefix 14 and 15, suffixLength reaching 6, total_zeros, long run_before */
static unsigned long covPrefix14, covPrefix15, covSuffix6, covTz, covRunLong;

static u32 RefBlockCavlc(strmData_t *pStrmData, i32 *coeffLevel, i32 nc, u32 maxNumCoeff)
{
    u32 i, tmp, totalCoeff, trailingOnes, suffixLength, levelPrefix;
    u32 levelSuffix, zerosLeft, bit;
    i32 level[16];
    u32 run[16];
    u32 bufferValue;
    u32 bufferBits;

    REF_BUFFER_INIT(bufferValue, bufferBits);

    REF_BUFFER_SHOW(bufferValue, bufferBits, bit, 16);
    tmp = DecodeCoeffToken(bit, (u32)nc);
    if (!tmp)
        return(HANTRO_NOK);
    REF_BUFFER_FLUSH(bufferValue, bufferBits, LENGTH_TC(tmp));

    totalCoeff = TOTAL_COEFF(tmp);
    if (totalCoeff > maxNumCoeff)
        return(HANTRO_NOK);
    trailingOnes = TRAILING_ONES(tmp);

    if (totalCoeff != 0)
    {
        i = 0;
        if (trailingOnes)
        {
            REF_BUFFER_GET(bufferValue, bufferBits, bit, trailingOnes);
            tmp = 1 << (trailingOnes - 1);
            for (; tmp; i++)
            {
                level[i] = bit & tmp ? -1 : 1;
                tmp >>= 1;
            }
        }

        if (totalCoeff > 10 && trailingOnes < 3)
            suffixLength = 1;
        else
            suffixLength = 0;

        for (; i < totalCoeff; i++)
        {
            REF_BUFFER_SHOW(bufferValue, bufferBits, bit, 16);
            levelPrefix = RefDecodeLevelPrefix(bit);
            if (levelPrefix == VLC_NOT_FOUND)
                return(HANTRO_NOK);
            REF_BUFFER_FLUSH(bufferValue, bufferBits, levelPrefix+1);
            covPrefix14 += levelPrefix == 14;
            covPrefix15 += levelPrefix == 15;
            covSuffix6 += suffixLength == 6;

            if (levelPrefix < 14)
                tmp = suffixLength;
            else if (levelPrefix == 14)
            {
                tmp = suffixLength ? suffixLength : 4;
            }
            else
            {
                if (!suffixLength)
                    suffixLength = 1;
                tmp = 12;
            }

            if (suffixLength)
                levelPrefix <<= suffixLength;

            if (tmp)
            {
                REF_BUFFER_GET(bufferValue, bufferBits, levelSuffix, tmp);
                levelPrefix += levelSuffix;
            }

            tmp = levelPrefix;

            if (i == trailingOnes && trailingOnes < 3)
                tmp += 2;

            level[i] = (tmp+2)>>1;

            if (suffixLength == 0)
                suffixLength = 1;

            if ((level[i] > (3 << (suffixLength - 1))) && suffixLength < 6)
                suffixLength++;

            if (tmp & 0x1)
                level[i] = -level[i];
        }

        if (totalCoeff < maxNumCoeff)
        {
            REF_BUFFER_SHOW(bufferValue, bufferBits, bit,9);
            zerosLeft = DecodeTotalZeros(bit, totalCoeff,
                                        (u32)(maxNumCoeff == 4));
            if (!zerosLeft)
                return(HANTRO_NOK);
            covTz++;
            REF_BUFFER_FLUSH(bufferValue, bufferBits, LENGTH(zerosLeft));
            zerosLeft = INFO(zerosLeft);
        }
        else
            zerosLeft = 0;

        for (i = 0; i < totalCoeff - 1; i++)
        {
            if (zerosLeft > 0)
            {
                REF_BUFFER_SHOW(bufferValue, bufferBits, bit,11);
                tmp = DecodeRunBefore(bit, zerosLeft);
                if (!tmp)
                    return(HANTRO_NOK);
                covRunLong += LENGTH(tmp) > 3;
                REF_BUFFER_FLUSH(bufferValue, bufferBits, LENGTH(tmp));
                run[i] = INFO(tmp);
                zerosLeft -= run[i]++;
            }
            else
            {
                run[i] = 1;
            }
        }

        tmp = zerosLeft;
        coeffLevel[tmp] = level[totalCoeff-1];
        levelSuffix = 1 << tmp;
        for (i = totalCoeff-1; i--;)
        {
            tmp += run[i];
            levelSuffix |= 1 << tmp;
            coeffLevel[tmp] = level[i];
        }

    }
    else
        levelSuffix = 0;

    if (h264bsdFlushBits(pStrmData, 32-bufferBits) != HANTRO_OK)
        return(HANTRO_NOK);

    return((totalCoeff << 4) | (levelSuffix << 16));
}

/* h264bsd_neighbour.c's left and upper neighbours of the 4x4 blocks */
static const neighbour_t refNA[24] = {
    {MB_A,5},    {MB_CURR,0}, {MB_A,7},    {MB_CURR,2},
    {MB_CURR,1}, {MB_CURR,4}, {MB_CURR,3}, {MB_CURR,6},
    {MB_A,13},   {MB_CURR,8}, {MB_A,15},   {MB_CURR,10},
    {MB_CURR,9}, {MB_CURR,12},{MB_CURR,11},{MB_CURR,14},
    {MB_A,17},   {MB_CURR,16},{MB_A,19},   {MB_CURR,18},
    {MB_A,21},   {MB_CURR,20},{MB_A,23},   {MB_CURR,22} };
static const neighbour_t refNB[24] = {
    {MB_B,10},   {MB_B,11},   {MB_CURR,0}, {MB_CURR,1},
    {MB_B,14},   {MB_B,15},   {MB_CURR,4}, {MB_CURR,5},
    {MB_CURR,2}, {MB_CURR,3}, {MB_CURR,8}, {MB_CURR,9},
    {MB_CURR,6}, {MB_CURR,7}, {MB_CURR,12},{MB_CURR,13},
    {MB_B,18},   {MB_B,19},   {MB_CURR,16},{MB_CURR,17},
    {MB_B,22},   {MB_B,23},   {MB_CURR,20},{MB_CURR,21} };

static u32 RefDetermineNc(const vjTc_t *tcA, const vjTc_t *tcB, u32 blockIndex, const i16 *pTotalCoeff)
{
    u32 tmp;
    i32 n;
    const neighbour_t *neighbourA, *neighbourB;
    u8 neighbourAindex, neighbourBindex;

    neighbourA = refNA + blockIndex;
    neighbourB = refNB + blockIndex;
    neighbourAindex = neighbourA->index;
    neighbourBindex = neighbourB->index;
    if (neighbourA->mb == MB_CURR && neighbourB->mb == MB_CURR)
    {
        n = (pTotalCoeff[neighbourAindex] +
             pTotalCoeff[neighbourBindex] + 1)>>1;
    }
    else if (neighbourA->mb == MB_CURR)
    {
        n = pTotalCoeff[neighbourAindex];
        if (tcB)
        {
            n = (n + tcB[neighbourBindex] + 1) >> 1;
        }
    }
    else if (neighbourB->mb == MB_CURR)
    {
        n = pTotalCoeff[neighbourBindex];
        if (tcA)
        {
            n = (n + tcA[neighbourAindex] + 1) >> 1;
        }
    }
    else
    {
        n = tmp = 0;
        if (tcA)
        {
            n = tcA[neighbourAindex];
            tmp = 1;
        }
        if (tcB)
        {
            if (tmp)
                n = (n + tcB[neighbourBindex] + 1) >> 1;
            else
                n = tcB[neighbourBindex];
        }
    }
    return((u32)n);
}

static u32 RefDecodeResidual(strmData_t *pStrmData, residual_t *pResidual,
    const vjTc_t *tcA, const vjTc_t *tcB, u32 is16x16, u32 codedBlockPattern)
{
    u32 i, j, tmp;
    i32 nc;
    u32 blockCoded;
    u32 blockIndex;
    i32 (*level)[16];

    level = pResidual->level;

    if (is16x16)
    {
        nc = (i32)RefDetermineNc(tcA, tcB, 0, pResidual->totalCoeff);
        tmp = RefBlockCavlc(pStrmData, level[24], nc, 16);
        if ((tmp & 0xF) != HANTRO_OK)
            return(tmp);
        pResidual->totalCoeff[24] = (tmp >> 4) & 0xFF;
    }

    for (i = 4, blockIndex = 0; i--;)
    {
        blockCoded = codedBlockPattern & 0x1;
        codedBlockPattern >>= 1;
        if (blockCoded)
        {
            memset(level[blockIndex], 0, 4 * sizeof(*level));
            for (j = 4; j--; blockIndex++)
            {
                nc = (i32)RefDetermineNc(tcA, tcB, blockIndex, pResidual->totalCoeff);
                if (is16x16)
                {
                    tmp = RefBlockCavlc(pStrmData,
                        level[blockIndex] + 1, nc, 15);
                    pResidual->coeffMap[blockIndex] = tmp >> 15;
                }
                else
                {
                    tmp = RefBlockCavlc(pStrmData,
                        level[blockIndex], nc, 16);
                    pResidual->coeffMap[blockIndex] = tmp >> 16;
                }
                if ((tmp & 0xF) != HANTRO_OK)
                    return(tmp);
                pResidual->totalCoeff[blockIndex] = (tmp >> 4) & 0xFF;
            }
        }
        else
            blockIndex += 4;
    }

    blockCoded = codedBlockPattern & 0x3;
    if (blockCoded)
    {
        tmp = RefBlockCavlc(pStrmData, level[25], -1, 4);
        if ((tmp & 0xF) != HANTRO_OK)
            return(tmp);
        pResidual->totalCoeff[25] = (tmp >> 4) & 0xFF;
        tmp = RefBlockCavlc(pStrmData, level[25]+4, -1, 4);
        if ((tmp & 0xF) != HANTRO_OK)
            return(tmp);
        pResidual->totalCoeff[26] = (tmp >> 4) & 0xFF;
    }

    blockCoded = codedBlockPattern & 0x2;
    if (blockCoded)
    {
        memset(level[16], 0, 8 * sizeof(*level));
        for (i = 8; i--;blockIndex++)
        {
            nc = (i32)RefDetermineNc(tcA, tcB, blockIndex, pResidual->totalCoeff);
            tmp = RefBlockCavlc(pStrmData,
                level[blockIndex] + 1, nc, 15);
            if ((tmp & 0xF) != HANTRO_OK)
                return(tmp);
            pResidual->totalCoeff[blockIndex] = (tmp >> 4) & 0xFF;
            pResidual->coeffMap[blockIndex] = (tmp >> 15);
        }
    }

    return(HANTRO_OK);
}

/* ---- the random differential ---- */
static u32 rngState = 0x2545F491u;
static u32 Rnd(void)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

/* a byte of one of five flavours: uniform, mostly zero bits, mostly one bits, zero, or uniform with rare zero runs */
static u8 StreamByte(u32 mode)
{
    switch (mode)
    {
        case 0: return (u8)Rnd();
        case 1: return (u8)(Rnd() & Rnd() & Rnd());
        case 2: return (u8)(Rnd() | Rnd());
        case 3: return (u8)(Rnd() % 7 ? Rnd() & Rnd() : 0);
        default: return (u8)(Rnd() % 13 ? Rnd() : 0);
    }
}

static unsigned long resCases, resOk, resCoeffs, resBad;

static void ResidualCase(void)
{
    static u8 buf[160];
    static residual_t r1, r2;
    vjTc_t tcA[24], tcB[24];
    strmData_t s1, s2;
    u32 size = 1 + Rnd() % 150, mode = Rnd() % 5, i, is16x16, cbp, st1, st2;
    const vjTc_t *pA, *pB;

    for (i = 0; i < size; i++)
        buf[i] = StreamByte(Rnd() % 4 ? mode : Rnd() % 5);
    for (i = 0; i < 24; i++)
    {
        tcA[i] = (vjTc_t)(Rnd() % 17);
        tcB[i] = (vjTc_t)(Rnd() % 17);
    }
    pA = Rnd() % 3 ? tcA : NULL;
    pB = Rnd() % 3 ? tcB : NULL;
    is16x16 = Rnd() & 1;
    cbp = Rnd() % 48;
    /* stale bytes from "an earlier macroblock", then the caller's lazy clear */
    for (i = 0; i < sizeof r1; i++)
        ((u8 *)&r1)[i] = (u8)Rnd();
    memset(r1.totalCoeff, 0, sizeof r1.totalCoeff);
    memset(r1.level[24], 0, 2 * sizeof r1.level[24]);
    memset(r1.coeffMap, 0, sizeof r1.coeffMap);
    r2 = r1;
    s1.pStrmBuffStart = buf;
    s1.strmBuffSize = size;
    s1.strmBuffReadBits = Rnd() % (8 * size + 8);
    h264bsdStrmFill(&s1);
    s2 = s1;

    st1 = h264bsdDecodeResidualCavlc(&s1, &r1, pA, pB, is16x16, cbp);
    st2 = RefDecodeResidual(&s2, &r2, pA, pB, is16x16, cbp);
    resCases++;
    if (st1 != st2)
    {
        if (resBad++ < 5)
            printf("test_cavlc: residual case %lu: status %u, want %u\n", resCases, st1, st2);
        return;
    }
    if (st1 != HANTRO_OK)
        return;
    resOk++;
    for (i = 0; i < 27; i++)
        resCoeffs += (unsigned long)r2.totalCoeff[i];
    if (s1.strmBuffReadBits != s2.strmBuffReadBits || s1.cache[0] != s2.cache[0] || s1.cache[1] != s2.cache[1] ||
        memcmp(&r1, &r2, sizeof r1))
    {
        if (resBad++ < 5)
            printf("test_cavlc: residual case %lu: decoded differently (bits %u, want %u)\n", resCases,
                s1.strmBuffReadBits, s2.strmBuffReadBits);
    }
}

static unsigned long cases, bad;

static void check(const char *what, int arg, u32 bits, u32 got, u32 want)
{
    cases++;
    if (got != want && bad++ < 5)
        printf("test_cavlc: %s %d, bits %04x: info %04x, want %04x\n", what, arg, bits, got, want);
}

int main(int argc, char **argv)
{
    for (i32 nc = -1; nc <= 16; nc++)
        for (u32 b = 0; b < 0x10000; b++)
            check("coeff_token nC", nc, b, DecodeCoeffToken(b, (u32)nc), RefDecodeCoeffToken(b, (u32)nc));
    for (u32 dc = 0; dc < 2; dc++)
        for (u32 tc = 1; tc <= (dc ? 3u : 15u); tc++)
            for (u32 b = 0; b < 512; b++)
                check(dc ? "total_zeros chroma DC totalCoeff" : "total_zeros totalCoeff", (int)tc, b,
                      DecodeTotalZeros(b, tc, dc), RefDecodeTotalZeros(b, tc, dc));
    for (u32 zl = 0; zl < 32; zl++)
        for (u32 b = 0; b < 2048; b++)
            check("run_before zerosLeft", (int)zl, b, DecodeRunBefore(b, zl), RefDecodeRunBefore(b, zl));
    printf("test_cavlc: %lu cases, %lu mismatches\n", cases, bad);
    for (unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 0) : 400000; n; n--)
        ResidualCase();
    printf("test_cavlc: residual %lu cases (%lu decoded, %lu coefficients; the oracle decoded %lu level_prefix 14, %lu "
           "15, %lu levels at suffixLength 6, %lu total_zeros, %lu long run_before), %lu mismatches\n", resCases, resOk,
           resCoeffs, covPrefix14, covPrefix15, covSuffix6, covTz, covRunLong, resBad);
    return bad || resBad ? 1 : 0;
}
