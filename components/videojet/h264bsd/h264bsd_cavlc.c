/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 * Modified for VideoJet: see h264bsd/README.md, Local changes.
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

/*------------------------------------------------------------------------------

    Table of contents

     1. Include headers
     2. External compiler flags
     3. Module defines
     4. Local function prototypes
     5. Functions
          DecodeCoeffToken
          CoeffTokenLong
          DecodeTotalZeros
          DecodeRunBefore
          h264bsdDecodeResidualCavlc

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include <string.h>
#include "h264bsd_cavlc.h"
#include "h264bsd_util.h"

/* the rare paths stay out of line, so the macroblock's decode keeps its registers */
#define VJ_NOINLINE_CAVLC __attribute__((noinline))

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/* Following descriptions use term "information field" to represent combination
 * of certain decoded symbol value and the length of the corresponding variable
 * length code word. For example, total_zeros information field consists of
 * 4 bits symbol value (bits [4,7]) along with four bits to represent length
 * of the VLC code word (bits [0,3]) */

/* macro to obtain length of the coeff token information field, bits [0,4]  */
#define LENGTH_TC(vlc) ((vlc) & 0x1F)
/* macro to obtain length of the other information fields, bits [0,3] */
#define LENGTH(vlc) ((vlc) & 0xF)
/* macro to obtain code word from the information fields, bits [4,7] */
#define INFO(vlc) (((vlc) >> 4) & 0xF)  /* 4 MSB bits contain information */
/* macro to obtain trailing ones from the coeff token information word,
 * bits [5,10] */
#define TRAILING_ONES(coeffToken) ((coeffToken>>5) & 0x3F)
/* macro to obtain total coeff from the coeff token information word,
 * bits [11,15] */
#define TOTAL_COEFF(coeffToken) (((coeffToken) >> 11) & 0x1F)

#define VLC_NOT_FOUND 0xFFFFFFFEU

/* VLC tables for coeff_token. Because of long codes (max. 16 bits) some of the
 * tables have been splitted into multiple separate tables. Each array/table
 * element has the following structure:
 * [5 bits for tot.coeff.] [6 bits for tr.ones] [5 bits for VLC length]
 * If there is a 0x0000 value, it means that there is not corresponding VLC
 * codeword for that index. The tables of the codes over 6 bits for
 * 0 <= nC < 4 (Long) have a row for each number of leading zeros of the 16
 * bits from 4 (all such codes have 4 or more) to 16, indexed by the 3 bits
 * after the first 1. From 13 zeros on, those 3 bits run into the stop bit
 * below the 16 (CoeffTokenLong): row 13 is read only at odd columns (so pairs
 * repeat), rows 14 to 16 are constant, and rows after the longest code are 0. */

/* VLC lengths up to 6 bits, 0 <= nC < 2 */
static const u16 coeffToken0_0[32] = {
    0x0000,0x0000,0x0000,0x2066,0x1026,0x0806,0x1865,0x1865,
    0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,
    0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,
    0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,0x0822,0x0822};

/* VLC lengths 7 to 16 bits, 0 <= nC < 2 */
static const u16 coeffToken0Long[13][8] = {
    {0x2867,0x2867,0x1847,0x1847,0x2066,0x2066,0x2066,0x2066},
    {0x3068,0x3068,0x2048,0x2048,0x1828,0x1828,0x1008,0x1008},
    {0x3869,0x3869,0x2849,0x2849,0x2029,0x2029,0x1809,0x1809},
    {0x406a,0x406a,0x304a,0x304a,0x282a,0x282a,0x200a,0x200a},
    {0x486b,0x486b,0x384b,0x384b,0x302b,0x302b,0x280b,0x280b},
    {0x400d,0x484d,0x402d,0x380d,0x506d,0x404d,0x382d,0x300d},
    {0x606e,0x584e,0x502e,0x500e,0x586e,0x504e,0x482e,0x480e},
    {0x706f,0x684f,0x602f,0x600f,0x686f,0x604f,0x582f,0x580f},
    {0x8070,0x7850,0x7830,0x7010,0x7870,0x7050,0x7030,0x6810},
    {0x8010,0x8010,0x8050,0x8050,0x8030,0x8030,0x7810,0x7810},
    {0x682f,0x682f,0x682f,0x682f,0x682f,0x682f,0x682f,0x682f},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000}};

/* VLC lengths up to 6 bits, 2 <= nC < 4 */
static const u16 coeffToken2_0[32] = {
    0x0000,0x0000,0x0000,0x0000,0x3866,0x2046,0x2026,0x1006,
    0x3066,0x1846,0x1826,0x0806,0x2865,0x2865,0x1025,0x1025,
    0x2064,0x2064,0x2064,0x2064,0x1864,0x1864,0x1864,0x1864,
    0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,0x1043,0x1043};

/* VLC lengths 7 to 14 bits, 2 <= nC < 4 */
static const u16 coeffToken2Long[13][8] = {
    {0x4067,0x4067,0x2847,0x2847,0x2827,0x2827,0x1807,0x1807},
    {0x2808,0x2808,0x3048,0x3048,0x3028,0x3028,0x2008,0x2008},
    {0x4869,0x4869,0x3849,0x3849,0x3829,0x3829,0x3009,0x3009},
    {0x586b,0x484b,0x482b,0x400b,0x506b,0x404b,0x402b,0x380b},
    {0x580c,0x584c,0x582c,0x500c,0x606c,0x504c,0x502c,0x480c},
    {0x706d,0x684d,0x682d,0x680d,0x686d,0x604d,0x602d,0x600d},
    {0x782e,0x780e,0x784e,0x702e,0x704d,0x704d,0x700d,0x700d},
    {0x806e,0x806e,0x804e,0x804e,0x802e,0x802e,0x800e,0x800e},
    {0x786d,0x786d,0x786d,0x786d,0x786d,0x786d,0x786d,0x786d},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000}};

/* VLC lengths up to 6 bits, 4 <= nC < 8 */
static const u16 coeffToken4_0[64] = {
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x1806,0x3846,0x3826,0x1006,0x4866,0x3046,0x3026,0x0806,
    0x2825,0x2825,0x2845,0x2845,0x2025,0x2025,0x2045,0x2045,
    0x1825,0x1825,0x4065,0x4065,0x1845,0x1845,0x1025,0x1025,
    0x3864,0x3864,0x3864,0x3864,0x3064,0x3064,0x3064,0x3064,
    0x2864,0x2864,0x2864,0x2864,0x2064,0x2064,0x2064,0x2064,
    0x1864,0x1864,0x1864,0x1864,0x1044,0x1044,0x1044,0x1044,
    0x0824,0x0824,0x0824,0x0824,0x0004,0x0004,0x0004,0x0004};

/* VLC lengths up to 10 bits, 4 <= nC < 8 */
static const u16 coeffToken4_1[128] = {
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
static const u16 coeffToken8[64] = {
    0x0806,0x0826,0x0000,0x0006,0x1006,0x1026,0x1046,0x0000,
    0x1806,0x1826,0x1846,0x1866,0x2006,0x2026,0x2046,0x2066,
    0x2806,0x2826,0x2846,0x2866,0x3006,0x3026,0x3046,0x3066,
    0x3806,0x3826,0x3846,0x3866,0x4006,0x4026,0x4046,0x4066,
    0x4806,0x4826,0x4846,0x4866,0x5006,0x5026,0x5046,0x5066,
    0x5806,0x5826,0x5846,0x5866,0x6006,0x6026,0x6046,0x6066,
    0x6806,0x6826,0x6846,0x6866,0x7006,0x7026,0x7046,0x7066,
    0x7806,0x7826,0x7846,0x7866,0x8006,0x8026,0x8046,0x8066};

/* VLC lengths up to 3 bits, nC == -1 */
static const u16 coeffTokenMinus1_0[8] = {
    0x0000,0x1043,0x0002,0x0002,0x0821,0x0821,0x0821,0x0821};

/* VLC lengths up to 8 bits, nC == -1 */
static const u16 coeffTokenMinus1_1[32] = {
    0x2067,0x2067,0x2048,0x2028,0x1847,0x1847,0x1827,0x1827,
    0x2006,0x2006,0x2006,0x2006,0x1806,0x1806,0x1806,0x1806,
    0x1006,0x1006,0x1006,0x1006,0x1866,0x1866,0x1866,0x1866,
    0x1026,0x1026,0x1026,0x1026,0x0806,0x0806,0x0806,0x0806};

/* VLC tables for total_zeros. One table containing longer code, totalZeros_1,
 * has been broken into two separate tables. Table elements have the
 * following structure:
 * [4 bits for info] [4 bits for VLC length] */

/* VLC lengths up to 5 bits */
static const u8 totalZeros_1_0[32] = {
    0x00,0x00,0x65,0x55,0x44,0x44,0x34,0x34,
    0x23,0x23,0x23,0x23,0x13,0x13,0x13,0x13,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01};

/* VLC lengths up to 9 bits */
static const u8 totalZeros_1_1[32] = {
    0x00,0xf9,0xe9,0xd9,0xc8,0xc8,0xb8,0xb8,
    0xa7,0xa7,0xa7,0xa7,0x97,0x97,0x97,0x97,
    0x86,0x86,0x86,0x86,0x86,0x86,0x86,0x86,
    0x76,0x76,0x76,0x76,0x76,0x76,0x76,0x76};

static const u8 totalZeros_2[64] = {
    0xe6,0xd6,0xc6,0xb6,0xa5,0xa5,0x95,0x95,
    0x84,0x84,0x84,0x84,0x74,0x74,0x74,0x74,
    0x64,0x64,0x64,0x64,0x54,0x54,0x54,0x54,
    0x43,0x43,0x43,0x43,0x43,0x43,0x43,0x43,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23,
    0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13,
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03};

static const u8 totalZeros_3[64] = {
    0xd6,0xb6,0xc5,0xc5,0xa5,0xa5,0x95,0x95,
    0x84,0x84,0x84,0x84,0x54,0x54,0x54,0x54,
    0x44,0x44,0x44,0x44,0x04,0x04,0x04,0x04,
    0x73,0x73,0x73,0x73,0x73,0x73,0x73,0x73,
    0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23,
    0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13};

static const u8 totalZeros_4[32] = {
    0xc5,0xb5,0xa5,0x05,0x94,0x94,0x74,0x74,
    0x34,0x34,0x24,0x24,0x83,0x83,0x83,0x83,
    0x63,0x63,0x63,0x63,0x53,0x53,0x53,0x53,
    0x43,0x43,0x43,0x43,0x13,0x13,0x13,0x13};

static const u8 totalZeros_5[32] = {
    0xb5,0x95,0xa4,0xa4,0x84,0x84,0x24,0x24,
    0x14,0x14,0x04,0x04,0x73,0x73,0x73,0x73,
    0x63,0x63,0x63,0x63,0x53,0x53,0x53,0x53,
    0x43,0x43,0x43,0x43,0x33,0x33,0x33,0x33};

static const u8 totalZeros_6[64] = {
    0xa6,0x06,0x15,0x15,0x84,0x84,0x84,0x84,
    0x93,0x93,0x93,0x93,0x93,0x93,0x93,0x93,
    0x73,0x73,0x73,0x73,0x73,0x73,0x73,0x73,
    0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,
    0x53,0x53,0x53,0x53,0x53,0x53,0x53,0x53,
    0x43,0x43,0x43,0x43,0x43,0x43,0x43,0x43,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23};

static const u8 totalZeros_7[64] = {
    0x96,0x06,0x15,0x15,0x74,0x74,0x74,0x74,
    0x83,0x83,0x83,0x83,0x83,0x83,0x83,0x83,
    0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,
    0x43,0x43,0x43,0x43,0x43,0x43,0x43,0x43,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52};

static const u8 totalZeros_8[64] = {
    0x86,0x06,0x25,0x25,0x14,0x14,0x14,0x14,
    0x73,0x73,0x73,0x73,0x73,0x73,0x73,0x73,
    0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,
    0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42};

static const u8 totalZeros_9[64] = {
    0x16,0x06,0x75,0x75,0x24,0x24,0x24,0x24,
    0x53,0x53,0x53,0x53,0x53,0x53,0x53,0x53,
    0x62,0x62,0x62,0x62,0x62,0x62,0x62,0x62,
    0x62,0x62,0x62,0x62,0x62,0x62,0x62,0x62,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,
    0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32};

static const u8 totalZeros_10[32] = {
    0x15,0x05,0x64,0x64,0x23,0x23,0x23,0x23,
    0x52,0x52,0x52,0x52,0x52,0x52,0x52,0x52,
    0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,
    0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32};

static const u8 totalZeros_11[16] = {
    0x04,0x14,0x23,0x23,0x33,0x33,0x53,0x53,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41};

static const u8 totalZeros_12[16] = {
    0x04,0x14,0x43,0x43,0x22,0x22,0x22,0x22,
    0x31,0x31,0x31,0x31,0x31,0x31,0x31,0x31};

static const u8 totalZeros_13[8] = {0x03,0x13,0x32,0x32,0x21,0x21,0x21,0x21};

static const u8 totalZeros_14[4] = {0x02,0x12,0x21,0x21};

static const u8 totalZeros_15[2] = {0x01,0x11};

/* totalCoeff 1..15: the table and the shift that takes its index from the 9
 * bits. totalCoeff 1 has a second level, totalZeros_1_1, for codes of 6 to 9
 * bits (a 0 in totalZeros_1_0) */
static const struct
{
    const u8 *table;
    u32 shift;
} totalZerosLuma[16] = {
    {0, 0}, {totalZeros_1_0, 4}, {totalZeros_2, 3}, {totalZeros_3, 3},
    {totalZeros_4, 4}, {totalZeros_5, 4}, {totalZeros_6, 3}, {totalZeros_7, 3},
    {totalZeros_8, 3}, {totalZeros_9, 3}, {totalZeros_10, 4}, {totalZeros_11, 5},
    {totalZeros_12, 5}, {totalZeros_13, 6}, {totalZeros_14, 7}, {totalZeros_15, 8}};

/* chroma DC, indexed by totalCoeff (1..3) and the first 3 of the 9 bits */
static const u8 totalZerosChromaDC[4][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x33,0x23,0x12,0x12,0x01,0x01,0x01,0x01},
    {0x22,0x22,0x12,0x12,0x01,0x01,0x01,0x01},
    {0x11,0x11,0x11,0x11,0x01,0x01,0x01,0x01}};

/* VLC tables for run_before. Table elements have the following structure:
 * [4 bits for info] [4bits for VLC length]
 */

/* indexed by MIN(zerosLeft, 7) and the first 3 of the 11 bits; 0 means
 * zerosLeft > 6 and a code longer than 3 bits. Row 0, zerosLeft 0, is never
 * decoded; it gives what the zerosLeft > 6 code gives there. */
static const u8 runBefore[8][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03},
    {0x11,0x11,0x11,0x11,0x01,0x01,0x01,0x01},
    {0x22,0x22,0x12,0x12,0x01,0x01,0x01,0x01},
    {0x32,0x32,0x22,0x22,0x12,0x12,0x02,0x02},
    {0x43,0x33,0x22,0x22,0x12,0x12,0x02,0x02},
    {0x53,0x43,0x33,0x23,0x12,0x12,0x02,0x02},
    {0x13,0x23,0x43,0x33,0x63,0x53,0x02,0x02},
    {0x00,0x63,0x53,0x43,0x33,0x23,0x13,0x03}};

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

static u32 DecodeCoeffToken(u32 bits, u32 nc);

static u32 CoeffTokenLong(const u16 (*table)[8], u32 bits);

static u32 DecodeTotalZeros(u32 bits, u32 totalCoeff, u32 isChromaDC);

static u32 DecodeRunBefore(u32 bits,u32 zerosLeft);

/*------------------------------------------------------------------------------

    Function: DecodeCoeffToken

        Functional description:
          Function to decode coeff_token information field from the stream.

        Inputs:
          u32 bits                  next 16 stream bits
          u32 nc                    nC, see standard for details

        Outputs:
          u32  information field (11 bits for value, 5 bits for length)

------------------------------------------------------------------------------*/

u32 DecodeCoeffToken(u32 bits, u32 nc)
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
            value = coeffToken0_0[bits >> 10];
        else
            value = CoeffTokenLong(coeffToken0Long, bits);
    }
    else if (nc < 4)
    {
        if (bits >= 0x8000)
        {
            value = bits & 0x4000 ? 0x0002 : 0x0822;
        }
        else if (bits >= 0x1000)
            value = coeffToken2_0[bits >> 10];
        else
            value = CoeffTokenLong(coeffToken2Long, bits);
    }
    else if (nc < 8)
    {
        value = coeffToken4_0[bits >> 10];
        if (!value)
            value = coeffToken4_1[bits>>6];
    }
    else if (nc <= 16)
    {
        value = coeffToken8[bits>>10];
    }
    else
    {
        value = coeffTokenMinus1_0[bits >> 13];
        if (!value)
            value = coeffTokenMinus1_1[bits>>8];
    }

    return(value);

}

/*------------------------------------------------------------------------------

    Function: CoeffTokenLong

        Functional description:
          Second level of the coeff_token decode, 0 <= nC < 4: a code of
          more than 6 bits

        Inputs:
          table                     coeffToken0Long or coeffToken2Long
          u32 bits                  next 16 stream bits, below 0x1000

        Outputs:
          u32  information field as DecodeCoeffToken, 0 for an invalid code

------------------------------------------------------------------------------*/

u32 CoeffTokenLong(const u16 (*table)[8], u32 bits)
{

/* Variables */

    /* the 16 bits, then a stop bit: 15 + 0..16 leading zeros */
    u32 value = (bits << 1) | 1;
    u32 lz = (u32)__builtin_clz(value);

/* Code */

    ASSERT(lz >= 15 + 4);

    /* row lz - 15 - 4, then the 3 bits after the first 1 */
    return(table[lz - 19][(value << lz) >> 28 & 7]);

}

/*------------------------------------------------------------------------------

    Function: DecodeTotalZeros

        Functional description:
          Function to decode total_zeros information field from the stream

        Inputs:
          u32 bits                  next 9 stream bits
          u32 totalCoeff            total number of coefficients for the block
                                    being decoded
          u32 isChromaDC           flag to indicate chroma DC block

        Outputs:
          u32  information field (4 bits value, 4 bits length)

------------------------------------------------------------------------------*/

u32 DecodeTotalZeros(u32 bits, u32 totalCoeff, u32 isChromaDC)
{

/* Variables */

    u32 value;

/* Code */

    ASSERT(totalCoeff);

    if (!isChromaDC)
    {
        ASSERT(totalCoeff < 16);
        value = totalZerosLuma[totalCoeff].table[
            bits >> totalZerosLuma[totalCoeff].shift];
        if (!value && totalCoeff == 1)
            value = totalZeros_1_1[bits];
    }
    else
    {
        ASSERT(totalCoeff < 4);
        value = totalZerosChromaDC[totalCoeff][bits >> 6];
    }

    return(value);

}

/*------------------------------------------------------------------------------

    Function: DecodeRunBefore

        Functional description:
          Function to decode run_before information field from the stream

        Inputs:
          u32 bits                  next 11 stream bits
          u32 zerosLeft             number of zeros left for the current block

        Outputs:
          u32  information field (4 bits value, 4 bits length)

------------------------------------------------------------------------------*/

u32 DecodeRunBefore(u32 bits, u32 zerosLeft)
{

/* Variables */

    u32 value, lz;

/* Code */

    value = runBefore[MIN(zerosLeft, 7)][bits >> 8];
    if (!value && bits)
    {
        /* zerosLeft > 6, a 1 after 3..10 zeros: run_before is zeros + 4 */
        lz = (u32)__builtin_clz(bits) - 21;
        value = ((lz + 4) << 4) | (lz + 1);
        if (INFO(value) > zerosLeft)
            value = 0;
    }

    return(value);

}

/*------------------------------------------------------------------------------

    Function: h264bsdDecodeResidualCavlc

        Functional description:
          VideoJet: residual() of one macroblock, CAVLC. Every coded block in
          the order of the standard (and of the h264bsd DecodeResidual and
          residual_block_cavlc decoder this replaces, kept as the oracle in
          host/test_cavlc.c): the Intra16x16 luma DC, the luma blocks of the
          coded 8x8 blocks, the two chroma DC blocks, the chroma AC blocks.
          Same symbols, tables, arithmetic and checks, and the same levels
          stored for a decodable macroblock. What changed is the bit
          reading: the window of strmData_t (strmBuffReadBits, cache[0..1])
          stays in registers for the whole macroblock, each symbol is a peek
          and a skip of the window, the levels after the first come from a
          table when their code is short (vjLevelTab), and the run_before
          decode places each level as it goes (the same positions, highest
          first).

          Errors (every one is HANTRO_NOK, as before): an invalid coeff_token,
          total_zeros or run_before code, 16 zeros for a level_prefix,
          totalCoeff above the block's maximum, or a block that ends past the
          end of the buffer (bits there read as 0, as before). The original
          also stopped at a refill past the end inside a block; positions only
          grow, so that block ends past the end too and the result is the same
          HANTRO_NOK. On an error the levels hold an unspecified part of the
          block: a failed macroblock's layer is never reconstructed.

        Inputs:
          pStrmData             stream, at the macroblock's residual
          tcA, tcB              totalCoeff[0..23] of the left and upper
                                macroblocks for nC, NULL when unavailable
          is16x16               an Intra16x16 macroblock
          codedBlockPattern     its cbp

        Outputs:
          pResidual             totalCoeff, coeffMap and level of the coded
                                blocks (the caller lazily cleared the rest)
          pStrmData             after the residual

        Returns:
          HANTRO_OK, HANTRO_NOK

------------------------------------------------------------------------------*/

/* nC neighbours of the 4x4 blocks 0..23 (h264bsd_neighbour.c N_A_4x4B, N_B_4x4B): the block's index in the left (A)
 * or upper (B) macroblock, or VJ_NC_CUR | its index in the current one */
#define VJ_NC_CUR 0x80
static const u8 vjNcA[24] = {
    5, VJ_NC_CUR | 0, 7, VJ_NC_CUR | 2, VJ_NC_CUR | 1, VJ_NC_CUR | 4, VJ_NC_CUR | 3, VJ_NC_CUR | 6,
    13, VJ_NC_CUR | 8, 15, VJ_NC_CUR | 10, VJ_NC_CUR | 9, VJ_NC_CUR | 12, VJ_NC_CUR | 11, VJ_NC_CUR | 14,
    17, VJ_NC_CUR | 16, 19, VJ_NC_CUR | 18, 21, VJ_NC_CUR | 20, 23, VJ_NC_CUR | 22};
static const u8 vjNcB[24] = {
    10, 11, VJ_NC_CUR | 0, VJ_NC_CUR | 1, 14, 15, VJ_NC_CUR | 4, VJ_NC_CUR | 5,
    VJ_NC_CUR | 2, VJ_NC_CUR | 3, VJ_NC_CUR | 8, VJ_NC_CUR | 9, VJ_NC_CUR | 6, VJ_NC_CUR | 7, VJ_NC_CUR | 12,
    VJ_NC_CUR | 13, 18, 19, VJ_NC_CUR | 16, VJ_NC_CUR | 17, 22, 23, VJ_NC_CUR | 20, VJ_NC_CUR | 21};

/* nC of block blk (DetermineNc): the average of the available neighbours' totalCoeff, rounded up, or the one, or 0 */
static inline __attribute__((always_inline)) u32 VjNc(const vjTc_t *tcA,
    const vjTc_t *tcB, const i16 *cur, u32 blk)
{
    u32 a = vjNcA[blk], b = vjNcB[blk], nA = 0, nB = 0, av = 0;

    if (a & VJ_NC_CUR)
    {
        nA = (u32)cur[a & 31];
        av = 1;
    }
    else if (tcA)
    {
        nA = (u32)tcA[a];
        av = 1;
    }
    if (b & VJ_NC_CUR)
    {
        nB = (u32)cur[b & 31];
        av |= 2;
    }
    else if (tcB)
    {
        nB = (u32)tcB[b];
        av |= 2;
    }
    /* an unavailable neighbour counts 0 */
    return av == 3 ? (nA + nB + 1) >> 1 : nA + nB;
}

/* The bit window in registers: strmData_t's strmBuffReadBits (pos), cache[0] (c0) and cache[1] (c1), and the buffer */
typedef struct
{
    u32 pos, c0, c1;
    const u8 *buf;
    u32 size;
} vjBits_t;

/* the next 32 bits: (c0 << pos % 32) | (c1 >> (32 - pos % 32)), c0 at pos % 32 == 0 */
static inline __attribute__((always_inline)) u32 VjPeek(u32 c0, u32 c1, u32 pos)
{
#if defined(__XTENSA__)
    u32 r;

    /* ssl reads pos % 32 and gives SAR 32 for 0: one funnel shift. GCC sets SAR in each of its own shift insns, so it
     * is never live across this one */
    __asm__("ssl %1\n\tsrc %0, %2, %3" : "=r"(r) : "r"(pos), "r"(c0), "r"(c1));
    return r;
#else
    u32 s = pos & 31;

    return s ? (c0 << s) | (c1 >> (32 - s)) : c0;
#endif
}

/* stream word at byte off, h264bsdStrmFill's StrmWord: big-endian, bytes past the end of the buffer read as 0 */
static VJ_NOINLINE_CAVLC u32 VjWordTail(const u8 *buf, u32 size, u32 off)
{
    u32 i, out = 0;

    for (i = 0; i < 4; i++)
        out = (out << 8) | (off + i < size ? (u32)buf[off + i] : 0);
    return(out);
}

static inline __attribute__((always_inline)) u32 VjWord(const u8 *buf, u32 size, u32 off)
{
    if (off + 4 <= size)
        return(((u32)buf[off] << 24) | ((u32)buf[off + 1] << 16) | ((u32)buf[off + 2] << 8) | (u32)buf[off + 3]);
    return(VjWordTail(buf, size, off));
}

/* consume n < 32 bits: h264bsdFlushBits, in registers (one word boundary at most) */
#define VJ_SKIP(n) \
    do { \
        u32 np_ = pos + (n); \
        if ((np_ ^ pos) >= 32) \
        { \
            c0 = c1; \
            c1 = VjWord(buf, size, ((np_ >> 5) << 2) + 4); \
        } \
        pos = np_; \
    } while (0)

#define VJ_BLOCK_ERR 0xFFFFFFFFU

/* a level above vjSuffixUp[suffixLength] raises suffixLength (3 << (suffixLength - 1), never from 6) */
static const u32 vjSuffixUp[7] = {0, 3, 6, 12, 24, 48, 0xFFFFFFFFU};
/* a level of suffixLength 1..5 coded in up to 6 bits, by the top 6 bits: code length | 1 << 5 when it raises
 * suffixLength | the signed level << 8; 0 for a longer code */
static const u16 vjLevelTab[5][64] = {
    {0x0000,0x0000,0x0526,0xfb26,0x0425,0x0425,0xfc25,0xfc25,0x0304,0x0304,0x0304,0x0304,0xfd04,0xfd04,0xfd04,0xfd04,0x0203,0x0203,0x0203,0x0203,0x0203,0x0203,0x0203,0x0203,0xfe03,0xfe03,0xfe03,0xfe03,0xfe03,0xfe03,0xfe03,0xfe03,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0x0102,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02,0xff02},
    {0x0000,0x0000,0x0000,0x0000,0x0726,0xf926,0x0826,0xf826,0x0505,0x0505,0xfb05,0xfb05,0x0605,0x0605,0xfa05,0xfa05,0x0304,0x0304,0x0304,0x0304,0xfd04,0xfd04,0xfd04,0xfd04,0x0404,0x0404,0x0404,0x0404,0xfc04,0xfc04,0xfc04,0xfc04,0x0103,0x0103,0x0103,0x0103,0x0103,0x0103,0x0103,0x0103,0xff03,0xff03,0xff03,0xff03,0xff03,0xff03,0xff03,0xff03,0x0203,0x0203,0x0203,0x0203,0x0203,0x0203,0x0203,0x0203,0xfe03,0xfe03,0xfe03,0xfe03,0xfe03,0xfe03,0xfe03,0xfe03},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0906,0xf706,0x0a06,0xf606,0x0b06,0xf506,0x0c06,0xf406,0x0505,0x0505,0xfb05,0xfb05,0x0605,0x0605,0xfa05,0xfa05,0x0705,0x0705,0xf905,0xf905,0x0805,0x0805,0xf805,0xf805,0x0104,0x0104,0x0104,0x0104,0xff04,0xff04,0xff04,0xff04,0x0204,0x0204,0x0204,0x0204,0xfe04,0xfe04,0xfe04,0xfe04,0x0304,0x0304,0x0304,0x0304,0xfd04,0xfd04,0xfd04,0xfd04,0x0404,0x0404,0x0404,0x0404,0xfc04,0xfc04,0xfc04,0xfc04},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0906,0xf706,0x0a06,0xf606,0x0b06,0xf506,0x0c06,0xf406,0x0d06,0xf306,0x0e06,0xf206,0x0f06,0xf106,0x1006,0xf006,0x0105,0x0105,0xff05,0xff05,0x0205,0x0205,0xfe05,0xfe05,0x0305,0x0305,0xfd05,0xfd05,0x0405,0x0405,0xfc05,0xfc05,0x0505,0x0505,0xfb05,0xfb05,0x0605,0x0605,0xfa05,0xfa05,0x0705,0x0705,0xf905,0xf905,0x0805,0x0805,0xf805,0xf805},
    {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0106,0xff06,0x0206,0xfe06,0x0306,0xfd06,0x0406,0xfc06,0x0506,0xfb06,0x0606,0xfa06,0x0706,0xf906,0x0806,0xf806,0x0906,0xf706,0x0a06,0xf606,0x0b06,0xf506,0x0c06,0xf406,0x0d06,0xf306,0x0e06,0xf206,0x0f06,0xf106,0x1006,0xf006}};

/* residual_block_cavlc() of one block into coeffLevel (cleared by the caller): returns totalCoeff with the map of the
 * coefficients placed in *pMap (bit k: coeffLevel[k]), or VJ_BLOCK_ERR. One copy, out of line: the window passes
 * through bs once per block, a few loads and stores against hundreds of cycles of symbols (and ~1 KB of IRAM per
 * inlined copy) */
static VJ_NOINLINE_CAVLC u32 VjBlock(vjBits_t *bs,
    i32 *coeffLevel, u32 nc, u32 maxNumCoeff, u32 *pMap)
{
    u32 pos = bs->pos, c0 = bs->c0, c1 = bs->c1, size = bs->size;
    const u8 *buf = bs->buf;
    u32 w, tok, totalCoeff, trailingOnes, suffixLength, add2, zerosLeft;
    u32 i, p, map;
    i32 level[16];

    w = VjPeek(c0, c1, pos);
    tok = DecodeCoeffToken(w >> 16, nc);
    if (!tok)
        return(VJ_BLOCK_ERR);
    VJ_SKIP(LENGTH_TC(tok));
    totalCoeff = TOTAL_COEFF(tok);
    if (totalCoeff > maxNumCoeff)
        return(VJ_BLOCK_ERR);
    map = 0;

    if (totalCoeff)
    {
        trailingOnes = TRAILING_ONES(tok);
        i = 0;
        /* nonzero coefficients: +/- 1, a sign bit each */
        if (trailingOnes)
        {
            u32 t;

            w = VjPeek(c0, c1, pos) >> (32 - trailingOnes);
            VJ_SKIP(trailingOnes);
            for (t = 1u << (trailingOnes - 1); t; t >>= 1, i++)
                level[i] = w & t ? -1 : 1;
        }

        /* other levels: level_prefix (its leading zeros), level_suffix */
        suffixLength = (totalCoeff > 10 && trailingOnes < 3) ? 1 : 0;
        /* the first of them, after fewer than 3 trailing ones, is 2 more */
        add2 = trailingOnes < 3 ? 2 : 0;
        /* the first of them: the general decode, with the +2 and suffixLength 0 or 1 */
        if (i < totalCoeff)
        {
            u32 prefix, len, code, lv;

            w = VjPeek(c0, c1, pos);
            /* more than 15 zeros */
            if (w < 0x10000)
                return(VJ_BLOCK_ERR);
            prefix = (u32)__builtin_clz(w);
            if (prefix < 14)
            {
                /* the prefix's 1 and the suffixLength suffix bits are the
                 * top len bits of w less its zeros, x = 2^suffixLength +
                 * suffix: code = (prefix << suffixLength) + suffix */
                len = prefix + 1 + suffixLength;
                code = (w >> (32 - len)) + ((prefix - 1) << suffixLength);
            }
            else if (prefix == 14)
            {
                u32 n = suffixLength ? suffixLength : 4;

                len = 15 + n;
                code = (14u << suffixLength) + ((w << 15) >> (32 - n));
            }
            else
            {
                /* suffixLength 1 here adds 15 to the code when it was 0 */
                if (!suffixLength)
                    suffixLength = 1;
                len = 28;
                code = (15u << suffixLength) + ((w << 16) >> 20);
            }
            VJ_SKIP(len);
            code += add2;
            lv = (code + 2) >> 1;
            /* suffixLength at least 1 after a level, one more above its
             * threshold 3 << (suffixLength - 1), up to 6 */
            suffixLength += !suffixLength;
            suffixLength += lv > vjSuffixUp[suffixLength];
            level[i] = code & 0x1 ? -(i32)lv : (i32)lv;
            i++;
        }
        /* the rest, suffixLength 1..6 (no +2): codes of up to 6 bits from a table on the top 6 bits of the window
         * (suffixLength 1..5), the others as above */
        for (; i < totalCoeff; i++)
        {
            u32 prefix, len, code, lv, e;

            w = VjPeek(c0, c1, pos);
            e = suffixLength < 6 ? vjLevelTab[suffixLength - 1][w >> 26] : 0;
            if (e)
            {
                VJ_SKIP(e & 31);
                suffixLength += (e >> 5) & 1;
                level[i] = (i32)(i8)(e >> 8);
                continue;
            }
            if (w < 0x10000)
                return(VJ_BLOCK_ERR);
            prefix = (u32)__builtin_clz(w);
            if (prefix < 14)
            {
                len = prefix + 1 + suffixLength;
                code = (w >> (32 - len)) + ((prefix - 1) << suffixLength);
            }
            else if (prefix == 14)
            {
                len = 15 + suffixLength;
                code = (14u << suffixLength) + ((w << 15) >> (32 - suffixLength));
            }
            else
            {
                len = 28;
                code = (15u << suffixLength) + ((w << 16) >> 20);
            }
            VJ_SKIP(len);
            lv = (code + 2) >> 1;
            suffixLength += lv > vjSuffixUp[suffixLength];
            level[i] = code & 0x1 ? -(i32)lv : (i32)lv;
        }

        /* zero runs */
        if (totalCoeff < maxNumCoeff)
        {
            w = VjPeek(c0, c1, pos);
            zerosLeft = DecodeTotalZeros(w >> 23, totalCoeff,
                                         (u32)(maxNumCoeff == 4));
            if (!zerosLeft)
                return(VJ_BLOCK_ERR);
            VJ_SKIP(LENGTH(zerosLeft));
            zerosLeft = INFO(zerosLeft);
        }
        else
            zerosLeft = 0;

        /* levels from the highest position down: level[0] at totalCoeff - 1
         * + total_zeros, each next one run_before + 1 below. Positions stay
         * in 0..15 (total_zeros <= 16 - totalCoeff, run_before <= zerosLeft) */
        p = totalCoeff - 1 + zerosLeft;
        coeffLevel[p] = level[0];
        map = 1u << p;
        for (i = 1; i < totalCoeff; i++)
        {
            u32 run = 0;

            if (zerosLeft)
            {
                w = VjPeek(c0, c1, pos);
                run = DecodeRunBefore(w >> 21, zerosLeft);
                if (!run)
                    return(VJ_BLOCK_ERR);
                VJ_SKIP(LENGTH(run));
                run = INFO(run);
                zerosLeft -= run;
            }
            p -= run + 1;
            coeffLevel[p] = level[i];
            map |= 1u << p;
        }
    }

    bs->pos = pos;
    bs->c0 = c0;
    bs->c1 = c1;
    /* the block ends inside the buffer (h264bsdFlushBits' END_OF_STREAM) */
    if (pos > 8 * size)
        return(VJ_BLOCK_ERR);
    *pMap = map;
    return(totalCoeff);
}

u32 h264bsdDecodeResidualCavlc(strmData_t *pStrmData, residual_t *pResidual,
    const vjTc_t *tcA, const vjTc_t *tcB, u32 is16x16, u32 codedBlockPattern)
{

/* Variables */

    vjBits_t bs;
    i32 (*level)[16] = pResidual->level;
    i16 *totalCoeff = pResidual->totalCoeff;
    u32 *coeffMap = pResidual->coeffMap;
    u32 i, j, blk, tc, map;
    u32 status = HANTRO_NOK;

/* Code */

    ASSERT(pStrmData);
    ASSERT(pResidual);

    bs.pos = pStrmData->strmBuffReadBits;
    bs.c0 = pStrmData->cache[0];
    bs.c1 = pStrmData->cache[1];
    bs.buf = pStrmData->pStrmBuffStart;
    bs.size = pStrmData->strmBuffSize;

    /* luma DC is at index 24 */
    if (is16x16)
    {
        tc = VjBlock(&bs, level[24], VjNc(tcA, tcB, totalCoeff, 0), 16, &map);
        if (tc == VJ_BLOCK_ERR)
            goto out;
        totalCoeff[24] = (i16)tc;
    }

    /* luma cbp in bits 0-3 */
    for (i = 4, blk = 0; i--; codedBlockPattern >>= 1)
    {
        if (!(codedBlockPattern & 0x1))
        {
            blk += 4;
            continue;
        }
        /* lazy clear: CAVLC writes only the nonzero levels */
        memset(level[blk], 0, 4 * sizeof(*level));
        for (j = 4; j--; blk++)
        {
            u32 nc = VjNc(tcA, tcB, totalCoeff, blk);

            if (is16x16)
            {
                tc = VjBlock(&bs, level[blk] + 1, nc, 15, &map);
                map <<= 1;
            }
            else
                tc = VjBlock(&bs, level[blk], nc, 16, &map);
            if (tc == VJ_BLOCK_ERR)
                goto out;
            coeffMap[blk] = map;
            totalCoeff[blk] = (i16)tc;
        }
    }

    /* chroma DC blocks are at indices 25 and 26 */
    if (codedBlockPattern & 0x3)
    {
        tc = VjBlock(&bs, level[25], (u32)-1, 4, &map);
        if (tc == VJ_BLOCK_ERR)
            goto out;
        totalCoeff[25] = (i16)tc;
        tc = VjBlock(&bs, level[25] + 4, (u32)-1, 4, &map);
        if (tc == VJ_BLOCK_ERR)
            goto out;
        totalCoeff[26] = (i16)tc;
    }

    /* chroma AC */
    if (codedBlockPattern & 0x2)
    {
        memset(level[16], 0, 8 * sizeof(*level)); /* lazy clear, as luma */
        for (blk = 16; blk < 24; blk++)
        {
            tc = VjBlock(&bs, level[blk] + 1, VjNc(tcA, tcB, totalCoeff, blk),
                15, &map);
            if (tc == VJ_BLOCK_ERR)
                goto out;
            totalCoeff[blk] = (i16)tc;
            coeffMap[blk] = map << 1;
        }
    }
    status = HANTRO_OK;

out:
    pStrmData->strmBuffReadBits = bs.pos;
    pStrmData->cache[0] = bs.c0;
    pStrmData->cache[1] = bs.c1;
    return(status);

}

