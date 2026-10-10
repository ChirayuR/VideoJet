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
          DecodeLevelPrefix
          DecodeTotalZeros
          DecodeRunBefore
          DecodeResidualBlockCavlc

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "h264bsd_cavlc.h"
#include "h264bsd_util.h"

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
 * after the first 1. */

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

/* following four macros are used to handle stream buffer "cache" in the CAVLC
 * decoding function */

/* macro to initialize stream buffer cache, fills the buffer (32 bits) */
#define BUFFER_INIT(value, bits) \
{ \
    bits = 32; \
    value = h264bsdShowBits32(pStrmData); \
}

/* macro to read numBits bits from the buffer, bits will be written to
 * outVal. Refills the buffer if not enough bits left */
#define BUFFER_SHOW(value, bits, outVal, numBits) \
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

/* macro to flush numBits bits from the buffer */
#define BUFFER_FLUSH(value, bits, numBits) \
{ \
    value <<= (numBits); \
    bits -= (numBits); \
}

/* macro to read and flush  numBits bits from the buffer, bits will be written
 * to outVal. Refills the buffer if not enough bits left */
#define BUFFER_GET(value, bits, outVal, numBits) \
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

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

static u32 DecodeCoeffToken(u32 bits, u32 nc);

static u32 CoeffTokenLong(const u16 (*table)[8], u32 bits);

static u32 DecodeLevelPrefix(u32 bits);

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

    Function: DecodeLevelPrefix

        Functional description:
          Function to decode level_prefix information field from the stream

        Inputs:
          u32 bits      next 16 stream bits

        Outputs:
          u32  level_prefix information field or VLC_NOT_FOUND

------------------------------------------------------------------------------*/

u32 DecodeLevelPrefix(u32 bits)
{

/* Code */

    ASSERT(bits < 0x10000);

    /* more than 15 zeros encountered which is an error */
    if (!bits)
        return(VLC_NOT_FOUND);

    /* leading zeros of the 16 bits; __builtin_clz is NSAU on Xtensa */
    return((u32)__builtin_clz(bits) - 16);

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

    Function: DecodeResidualBlockCavlc

        Functional description:
          Function to decode one CAVLC coded block. This corresponds to
          syntax elements residual_block_cavlc() in the standard.

        Inputs:
          pStrmData             pointer to stream data structure
          nc                    nC value
          maxNumCoeff           maximum number of residual coefficients

        Outputs:
          coeffLevel            stores decoded coefficient levels

        Returns:
          numCoeffs             on bits [4,11] if successful
          coeffMap              on bits [16,31] if successful, this is bit map
                                where each bit indicates if the corresponding
                                coefficient was zero (0) or non-zero (1)
          HANTRO_NOK            end of stream or error in stream

------------------------------------------------------------------------------*/

u32 h264bsdDecodeResidualBlockCavlc(
  strmData_t *pStrmData,
  i32 *coeffLevel,
  i32 nc,
  u32 maxNumCoeff)
{

/* Variables */

    u32 i, tmp, totalCoeff, trailingOnes, suffixLength, levelPrefix;
    u32 levelSuffix, zerosLeft, bit;
    i32 level[16];
    u32 run[16];
    /* stream "cache" */
    u32 bufferValue;
    u32 bufferBits;

/* Code */

    ASSERT(pStrmData);
    ASSERT(coeffLevel);
    ASSERT(nc > -2);
    ASSERT(maxNumCoeff == 4 || maxNumCoeff == 15 || maxNumCoeff == 16);
    ASSERT(VLC_NOT_FOUND != END_OF_STREAM);

    /* assume that coeffLevel array has been "cleaned" by caller */

    BUFFER_INIT(bufferValue, bufferBits);

    /*lint -e774 disable lint warning on always false comparison */
    BUFFER_SHOW(bufferValue, bufferBits, bit, 16);
    /*lint +e774 */
    tmp = DecodeCoeffToken(bit, (u32)nc);
    if (!tmp)
        return(HANTRO_NOK);
    BUFFER_FLUSH(bufferValue, bufferBits, LENGTH_TC(tmp));

    totalCoeff = TOTAL_COEFF(tmp);
    if (totalCoeff > maxNumCoeff)
        return(HANTRO_NOK);
    trailingOnes = TRAILING_ONES(tmp);

    if (totalCoeff != 0)
    {
        i = 0;
        /* nonzero coefficients: +/- 1 */
        if (trailingOnes)
        {
            BUFFER_GET(bufferValue, bufferBits, bit, trailingOnes);
            tmp = 1 << (trailingOnes - 1);
            for (; tmp; i++)
            {
                level[i] = bit & tmp ? -1 : 1;
                tmp >>= 1;
            }
        }

        /* other levels */
        if (totalCoeff > 10 && trailingOnes < 3)
            suffixLength = 1;
        else
            suffixLength = 0;

        for (; i < totalCoeff; i++)
        {
            BUFFER_SHOW(bufferValue, bufferBits, bit, 16);
            levelPrefix = DecodeLevelPrefix(bit);
            if (levelPrefix == VLC_NOT_FOUND)
                return(HANTRO_NOK);
            BUFFER_FLUSH(bufferValue, bufferBits, levelPrefix+1);

            if (levelPrefix < 14)
                tmp = suffixLength;
            else if (levelPrefix == 14)
            {
                tmp = suffixLength ? suffixLength : 4;
            }
            else
            {
                /* setting suffixLength to 1 here corresponds to adding 15
                 * to levelCode value if levelPrefix == 15 and
                 * suffixLength == 0 */
                if (!suffixLength)
                    suffixLength = 1;
                tmp = 12;
            }

            if (suffixLength)
                levelPrefix <<= suffixLength;

            if (tmp)
            {
                BUFFER_GET(bufferValue, bufferBits, levelSuffix, tmp);
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

        /* zero runs */
        if (totalCoeff < maxNumCoeff)
        {
            BUFFER_SHOW(bufferValue, bufferBits, bit,9);
            zerosLeft = DecodeTotalZeros(bit, totalCoeff,
                                        (u32)(maxNumCoeff == 4));
            if (!zerosLeft)
                return(HANTRO_NOK);
            BUFFER_FLUSH(bufferValue, bufferBits, LENGTH(zerosLeft));
            zerosLeft = INFO(zerosLeft);
        }
        else
            zerosLeft = 0;

        for (i = 0; i < totalCoeff - 1; i++)
        {
            if (zerosLeft > 0)
            {
                BUFFER_SHOW(bufferValue, bufferBits, bit,11);
                tmp = DecodeRunBefore(bit, zerosLeft);
                if (!tmp)
                    return(HANTRO_NOK);
                BUFFER_FLUSH(bufferValue, bufferBits, LENGTH(tmp));
                run[i] = INFO(tmp);
                zerosLeft -= run[i]++;
            }
            else
            {
                run[i] = 1;
            }
        }

        /* combining level and run, levelSuffix variable used to hold coeffMap,
         * i.e. bit map indicating which coefficients had non-zero value. */

        /*lint -esym(771,level,run) level and run are always initialized */
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

