// Copyright 2024 The Wuffs Authors.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT

// ----------------

/*
This test program is typically run indirectly, by the "wuffs test" or "wuffs
bench" commands. These commands take an optional "-mimic" flag to check that
Wuffs' output mimics (i.e. exactly matches) other libraries' output, such as
giflib for GIF, libpng for PNG, etc.

To manually run this test:

for CC in clang gcc; do
  $CC -std=c99 -Wall -Werror vp8.c && ./a.out
  rm -f a.out
done

Each edition should print "PASS", amongst other information, and exit(0).

Add the "wuffs mimic cflags" (everything after the colon below) to the C
compiler flags (after the .c file) to run the mimic tests.

To manually run the benchmarks, replace "-Wall -Werror" with "-O3" and replace
the first "./a.out" with "./a.out -bench". Combine these changes with the
"wuffs mimic cflags" to run the mimic benchmarks.
*/

// ¿ wuffs mimic cflags: -DWUFFS_MIMIC

// Wuffs ships as a "single file C library" or "header file library" as per
// https://github.com/nothings/stb/blob/master/docs/stb_howto.txt
//
// To use that single file as a "foo.c"-like implementation, instead of a
// "foo.h"-like header, #define WUFFS_IMPLEMENTATION before #include'ing or
// compiling it.
#define WUFFS_IMPLEMENTATION

// Defining the WUFFS_CONFIG__MODULE* macros are optional, but it lets users of
// release/c/etc.c choose which parts of Wuffs to build. That file contains the
// entire Wuffs standard library, implementing a variety of codecs and file
// formats. Without this macro definition, an optimizing compiler or linker may
// very well discard Wuffs code for unused codecs, but listing the Wuffs
// modules we use makes that process explicit. Preprocessing means that such
// code simply isn't compiled.
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__VP8

// If building this program in an environment that doesn't easily accommodate
// relative includes, you can use the script/inline-c-relative-includes.go
// program to generate a stand-alone C file.
#include "../../../release/c/wuffs-unsupported-snapshot.c"
#include "../testlib/testlib.c"
#ifdef WUFFS_MIMIC
// No mimic library.
#endif

// ---------------- VP8 Tests

void  //
initialize_decoder_test_state(wuffs_vp8__decoder* dec) {
  // Initialize the top and left luma (Y) pixels to the digits of pi.

  dec->private_impl.f_yuv_cache[0x00][0x07] = 0x31;
  dec->private_impl.f_yuv_cache[0x00][0x08] = 0x41;
  dec->private_impl.f_yuv_cache[0x00][0x09] = 0x59;
  dec->private_impl.f_yuv_cache[0x00][0x0A] = 0x26;
  dec->private_impl.f_yuv_cache[0x00][0x0B] = 0x53;
  dec->private_impl.f_yuv_cache[0x00][0x0C] = 0x58;
  dec->private_impl.f_yuv_cache[0x00][0x0D] = 0x97;
  dec->private_impl.f_yuv_cache[0x00][0x0E] = 0x93;
  dec->private_impl.f_yuv_cache[0x00][0x0F] = 0x23;
  dec->private_impl.f_yuv_cache[0x00][0x10] = 0x84;
  dec->private_impl.f_yuv_cache[0x00][0x11] = 0x62;
  dec->private_impl.f_yuv_cache[0x00][0x12] = 0x64;
  dec->private_impl.f_yuv_cache[0x00][0x13] = 0x33;
  dec->private_impl.f_yuv_cache[0x00][0x14] = 0x83;
  dec->private_impl.f_yuv_cache[0x00][0x15] = 0x27;
  dec->private_impl.f_yuv_cache[0x00][0x16] = 0x95;
  dec->private_impl.f_yuv_cache[0x00][0x17] = 0x02;

  dec->private_impl.f_yuv_cache[0x01][0x07] = 0x88;
  dec->private_impl.f_yuv_cache[0x02][0x07] = 0x41;
  dec->private_impl.f_yuv_cache[0x03][0x07] = 0x97;
  dec->private_impl.f_yuv_cache[0x04][0x07] = 0x16;
  dec->private_impl.f_yuv_cache[0x05][0x07] = 0x93;
  dec->private_impl.f_yuv_cache[0x06][0x07] = 0x99;
  dec->private_impl.f_yuv_cache[0x07][0x07] = 0x37;
  dec->private_impl.f_yuv_cache[0x08][0x07] = 0x51;
  dec->private_impl.f_yuv_cache[0x09][0x07] = 0x05;
  dec->private_impl.f_yuv_cache[0x0A][0x07] = 0x82;
  dec->private_impl.f_yuv_cache[0x0B][0x07] = 0x09;
  dec->private_impl.f_yuv_cache[0x0C][0x07] = 0x74;
  dec->private_impl.f_yuv_cache[0x0D][0x07] = 0x94;
  dec->private_impl.f_yuv_cache[0x0E][0x07] = 0x45;
  dec->private_impl.f_yuv_cache[0x0F][0x07] = 0x92;
  dec->private_impl.f_yuv_cache[0x10][0x07] = 0x30;

  // Initialize the top and left chroma-blue (U) pixels to the digits of e.

  dec->private_impl.f_yuv_cache[0x11][0x07] = 0x27;
  dec->private_impl.f_yuv_cache[0x11][0x08] = 0x18;
  dec->private_impl.f_yuv_cache[0x11][0x09] = 0x28;
  dec->private_impl.f_yuv_cache[0x11][0x0A] = 0x18;
  dec->private_impl.f_yuv_cache[0x11][0x0B] = 0x28;
  dec->private_impl.f_yuv_cache[0x11][0x0C] = 0x45;
  dec->private_impl.f_yuv_cache[0x11][0x0D] = 0x90;
  dec->private_impl.f_yuv_cache[0x11][0x0E] = 0x45;
  dec->private_impl.f_yuv_cache[0x11][0x0F] = 0x23;

  dec->private_impl.f_yuv_cache[0x12][0x07] = 0x53;
  dec->private_impl.f_yuv_cache[0x13][0x07] = 0x60;
  dec->private_impl.f_yuv_cache[0x14][0x07] = 0x28;
  dec->private_impl.f_yuv_cache[0x15][0x07] = 0x74;
  dec->private_impl.f_yuv_cache[0x16][0x07] = 0x71;
  dec->private_impl.f_yuv_cache[0x17][0x07] = 0x35;
  dec->private_impl.f_yuv_cache[0x18][0x07] = 0x26;
  dec->private_impl.f_yuv_cache[0x19][0x07] = 0x62;
}

const char*  //
test_wuffs_vp8_decode_inverse_dct_full() {
  CHECK_FOCUS(__func__);

  wuffs_vp8__decoder dec = {0};

  // Initialize the coefficients to the digits of phi.

  dec.private_data.f_mb_coeffs[0][0x00] = 0x16;
  dec.private_data.f_mb_coeffs[0][0x01] = 0x18;
  dec.private_data.f_mb_coeffs[0][0x02] = 0x03;
  dec.private_data.f_mb_coeffs[0][0x03] = 0x39;
  dec.private_data.f_mb_coeffs[0][0x04] = 0x88;
  dec.private_data.f_mb_coeffs[0][0x05] = 0x74;
  dec.private_data.f_mb_coeffs[0][0x06] = 0x98;
  dec.private_data.f_mb_coeffs[0][0x07] = 0x94;
  dec.private_data.f_mb_coeffs[0][0x08] = 0x84;
  dec.private_data.f_mb_coeffs[0][0x09] = 0x82;
  dec.private_data.f_mb_coeffs[0][0x0A] = 0x04;
  dec.private_data.f_mb_coeffs[0][0x0B] = 0x58;
  dec.private_data.f_mb_coeffs[0][0x0C] = 0x68;
  dec.private_data.f_mb_coeffs[0][0x0D] = 0x43;
  dec.private_data.f_mb_coeffs[0][0x0E] = 0x36;
  dec.private_data.f_mb_coeffs[0][0x0F] = 0x56;

  memset(dec.private_impl.f_yuv_cache, 0xC0,
         sizeof(dec.private_impl.f_yuv_cache));

  const uint32_t w0 = 0xFFACFBBC;
  const uint32_t w1 = 0x92ABA8C6;
  const uint32_t w2 = 0xABB6C1C6;
  const uint32_t w3 = 0x8FDFC5A7;

  wuffs_vp8__decoder__inverse_dct_full(&dec, 8, 1, 0);

  uint32_t h0 = wuffs_base__peek_u32be__no_bounds_check(
      &dec.private_impl.f_yuv_cache[1][8]);
  uint32_t h1 = wuffs_base__peek_u32be__no_bounds_check(
      &dec.private_impl.f_yuv_cache[2][8]);
  uint32_t h2 = wuffs_base__peek_u32be__no_bounds_check(
      &dec.private_impl.f_yuv_cache[3][8]);
  uint32_t h3 = wuffs_base__peek_u32be__no_bounds_check(
      &dec.private_impl.f_yuv_cache[4][8]);

  if ((h0 != w0) || (h1 != w1) || (h2 != w2) || (h3 != w3)) {
    RETURN_FAIL(
        "\nhave %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32   //
        "\nwant %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32,  //
        h0, h1, h2, h3, w0, w1, w2, w3);
  }
  return NULL;
}

const char*  //
test_wuffs_vp8_decode_inverse_wht() {
  CHECK_FOCUS(__func__);

  wuffs_vp8__decoder dec = {0};

  // Initialize the coefficients to the digits of phi.

  dec.private_data.f_mb_coeffs[24][0x00] = 0x16;
  dec.private_data.f_mb_coeffs[24][0x01] = 0x18;
  dec.private_data.f_mb_coeffs[24][0x02] = 0x03;
  dec.private_data.f_mb_coeffs[24][0x03] = 0x39;
  dec.private_data.f_mb_coeffs[24][0x04] = 0x88;
  dec.private_data.f_mb_coeffs[24][0x05] = 0x74;
  dec.private_data.f_mb_coeffs[24][0x06] = 0x98;
  dec.private_data.f_mb_coeffs[24][0x07] = 0x94;
  dec.private_data.f_mb_coeffs[24][0x08] = 0x84;
  dec.private_data.f_mb_coeffs[24][0x09] = 0x82;
  dec.private_data.f_mb_coeffs[24][0x0A] = 0x04;
  dec.private_data.f_mb_coeffs[24][0x0B] = 0x58;
  dec.private_data.f_mb_coeffs[24][0x0C] = 0x68;
  dec.private_data.f_mb_coeffs[24][0x0D] = 0x43;
  dec.private_data.f_mb_coeffs[24][0x0E] = 0x36;
  dec.private_data.f_mb_coeffs[24][0x0F] = 0x56;

  const uint32_t wants[16] = {
      0x00A5, 0x0011, 0x001C, 0xFFF2, 0xFFFF, 0xFFDF, 0xFFF5, 0x0006,
      0xFFC3, 0xFFF3, 0x0002, 0x0001, 0xFFCE, 0x0016, 0x0007, 0xFFEB,
  };

  wuffs_vp8__decoder__inverse_wht(&dec);

  for (int b = 0; b < 16; b++) {
    uint16_t have = dec.private_data.f_mb_coeffs[b][0];
    uint16_t want = wants[b];
    if (have != want) {
      RETURN_FAIL("b=0x%02X: have 0x%04X, want 0x%04X", b, have, want);
    }
  }

  return NULL;
}

const char*  //
test_wuffs_vp8_decode_predict_uv8() {
  CHECK_FOCUS(__func__);

  const uint64_t test_cases[13][8] = {
      {0x4444444444444444, 0x4444444444444444, 0x4444444444444444,
       0x4444444444444444, 0x4444444444444444, 0x4444444444444444,
       0x4444444444444444, 0x4444444444444444},
      {0x4454445471BC714F, 0x516151617EC97E5C, 0x1929192946914624,
       0x6575657592DD9270, 0x627262728FDA8F6D, 0x26362636539E5331,
       0x17271727448F4422, 0x5363536380CB805E},
      {0x1828182845904523, 0x1828182845904523, 0x1828182845904523,
       0x1828182845904523, 0x1828182845904523, 0x1828182845904523,
       0x1828182845904523, 0x1828182845904523},
      {0x5353535353535353, 0x6060606060606060, 0x2828282828282828,
       0x7474747474747474, 0x7171717171717171, 0x3535353535353535,
       0x2626262626262626, 0x6262626262626262},

      {0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0},

      {0x5050505050505050, 0x5050505050505050, 0x5050505050505050,
       0x5050505050505050, 0x5050505050505050, 0x5050505050505050,
       0x5050505050505050, 0x5050505050505050},
      {0x3838383838383838, 0x3838383838383838, 0x3838383838383838,
       0x3838383838383838, 0x3838383838383838, 0x3838383838383838,
       0x3838383838383838, 0x3838383838383838},
      {0x8080808080808080, 0x8080808080808080, 0x8080808080808080,
       0x8080808080808080, 0x8080808080808080, 0x8080808080808080,
       0x8080808080808080, 0x8080808080808080},
  };

  wuffs_vp8__decoder dec = {0};
  for (int mode = 0; mode < 13; mode++) {
    uint64_t w0 = test_cases[mode][0];
    uint64_t w1 = test_cases[mode][1];
    uint64_t w2 = test_cases[mode][2];
    uint64_t w3 = test_cases[mode][3];
    uint64_t w4 = test_cases[mode][4];
    uint64_t w5 = test_cases[mode][5];
    uint64_t w6 = test_cases[mode][6];
    uint64_t w7 = test_cases[mode][7];

    if (w0 == 0) {
      continue;
    }

    initialize_decoder_test_state(&dec);

    wuffs_vp8__decoder__predict_uv8(&dec, 0, mode);

    uint64_t h0 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x12][8]);
    uint64_t h1 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x13][8]);
    uint64_t h2 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x14][8]);
    uint64_t h3 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x15][8]);
    uint64_t h4 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x16][8]);
    uint64_t h5 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x17][8]);
    uint64_t h6 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x18][8]);
    uint64_t h7 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x19][8]);

    if ((h0 != w0) || (h1 != w1) || (h2 != w2) || (h3 != w3) ||  //
        (h4 != w4) || (h5 != w5) || (h6 != w6) || (h7 != w7)) {
      RETURN_FAIL(
          "mode=%d:"                                                         //
          "\nhave %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64  //
          " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64        //
          "\nwant %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64  //
          " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64,       //
          mode, h0, h1, h2, h3, h4, h5, h6, h7, w0, w1, w2, w3, w4, w5, w6, w7);
    }
  }

  return NULL;
}

const char*  //
test_wuffs_vp8_decode_predict_y16() {
  CHECK_FOCUS(__func__);

  const uint64_t test_cases[13][9] = {

      {0x5A5A5A5A5A5A5A5A, 0x5A5A5A5A5A5A5A5A, 0x5A5A5A5A5A5A5A5A,
       0x5A5A5A5A5A5A5A5A, 0x5A5A5A5A5A5A5A5A, 0x5A5A5A5A5A5A5A5A,
       0x5A5A5A5A5A5A5A5A, 0x5A5A5A5A5A5A5A5A, 0x5A5A5A5A5A5A5A5A},
      {0x98B07DAAAFEEEA7A, 0x5169366368A7A333, 0xA7BF8CB9BEFDF989,
       0x263E0B383D7C7808, 0xA3BB88B5BAF9F585, 0xA9C18EBBC0FFFB8B,
       0x475F2C595E9D9929, 0x6179467378B7B343, 0x152D00272C6B6700},
      {0x4159265358979323, 0x4159265358979323, 0x4159265358979323,
       0x4159265358979323, 0x4159265358979323, 0x4159265358979323,
       0x4159265358979323, 0x4159265358979323, 0x4159265358979323},
      {0x8888888888888888, 0x4141414141414141, 0x9797979797979797,
       0x1616161616161616, 0x9393939393939393, 0x9999999999999999,
       0x3737373737373737, 0x5151515151515151, 0x0505050505050505},

      {0, 0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, 0},

      {0x5D5D5D5D5D5D5D5D, 0x5D5D5D5D5D5D5D5D, 0x5D5D5D5D5D5D5D5D,
       0x5D5D5D5D5D5D5D5D, 0x5D5D5D5D5D5D5D5D, 0x5D5D5D5D5D5D5D5D,
       0x5D5D5D5D5D5D5D5D, 0x5D5D5D5D5D5D5D5D, 0x5D5D5D5D5D5D5D5D},
      {0x5757575757575757, 0x5757575757575757, 0x5757575757575757,
       0x5757575757575757, 0x5757575757575757, 0x5757575757575757,
       0x5757575757575757, 0x5757575757575757, 0x5757575757575757},
      {0x8080808080808080, 0x8080808080808080, 0x8080808080808080,
       0x8080808080808080, 0x8080808080808080, 0x8080808080808080,
       0x8080808080808080, 0x8080808080808080, 0x8080808080808080},
  };

  wuffs_vp8__decoder dec = {0};
  for (int mode = 0; mode < 13; mode++) {
    uint64_t w0 = test_cases[mode][0];
    uint64_t w1 = test_cases[mode][1];
    uint64_t w2 = test_cases[mode][2];
    uint64_t w3 = test_cases[mode][3];
    uint64_t w4 = test_cases[mode][4];
    uint64_t w5 = test_cases[mode][5];
    uint64_t w6 = test_cases[mode][6];
    uint64_t w7 = test_cases[mode][7];
    uint64_t w8 = test_cases[mode][8];

    if (w0 == 0) {
      continue;
    }

    initialize_decoder_test_state(&dec);

    wuffs_vp8__decoder__predict_y16(&dec, mode);

    uint64_t h0 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x01][8]);
    uint64_t h1 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x02][8]);
    uint64_t h2 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x03][8]);
    uint64_t h3 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x04][8]);
    uint64_t h4 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x05][8]);
    uint64_t h5 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x06][8]);
    uint64_t h6 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x07][8]);
    uint64_t h7 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x08][8]);
    uint64_t h8 = wuffs_base__peek_u64be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[0x09][8]);

    if ((h0 != w0) || (h1 != w1) || (h2 != w2) || (h3 != w3) ||  //
        (h4 != w4) || (h5 != w5) || (h6 != w6) || (h7 != w7) || (h8 != w8)) {
      RETURN_FAIL(
          "mode=%d:"                                                         //
          "\nhave %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64  //
          " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64        //
          " %016" PRIX64                                                     //
          "\nwant %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64  //
          " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64        //
          " %016" PRIX64,                                                    //
          mode, h0, h1, h2, h3, h4, h5, h6, h7, h8,                          //
          w0, w1, w2, w3, w4, w5, w6, w7, w8);
    }
  }

  return NULL;
}

const char*  //
test_wuffs_vp8_decode_predict_y4() {
  CHECK_FOCUS(__func__);

  const uint32_t test_cases[10][4] = {
      {0x51515151, 0x51515151, 0x51515151, 0x51515151},
      {0x98B07DAA, 0x51693663, 0xA7BF8CB9, 0x263E0B38},
      {0x43463E49, 0x43463E49, 0x43463E49, 0x43463E49},
      {0x61616161, 0x68686868, 0x61616161, 0x36363636},
      {0x4B43463E, 0x614B4346, 0x68614B43, 0x6168614B},
      {0x394D403D, 0x4B43463E, 0x61394D40, 0x684B4346},
      {0x463E4967, 0x3E496786, 0x49678678, 0x6786783F},
      {0x4D403D56, 0x463E4967, 0x403D5686, 0x3E496778},
      {0x5D4B4346, 0x65615D4B, 0x6C686561, 0x57616C68},
      {0x65686C61, 0x6C615736, 0x57361616, 0x16161616},
  };

  wuffs_vp8__decoder dec = {0};
  for (int mode = 0; mode < 10; mode++) {
    uint32_t w0 = test_cases[mode][0];
    uint32_t w1 = test_cases[mode][1];
    uint32_t w2 = test_cases[mode][2];
    uint32_t w3 = test_cases[mode][3];

    initialize_decoder_test_state(&dec);

    dec.private_impl.f_mb_subblock_modes[0] = mode;
    wuffs_vp8__decoder__predict_y4(&dec, 0);

    uint32_t h0 = wuffs_base__peek_u32be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[1][8]);
    uint32_t h1 = wuffs_base__peek_u32be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[2][8]);
    uint32_t h2 = wuffs_base__peek_u32be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[3][8]);
    uint32_t h3 = wuffs_base__peek_u32be__no_bounds_check(
        &dec.private_impl.f_yuv_cache[4][8]);

    if ((h0 != w0) || (h1 != w1) || (h2 != w2) || (h3 != w3)) {
      RETURN_FAIL(
          "mode=%d:"                                                      //
          "\nhave %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32   //
          "\nwant %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32,  //
          mode, h0, h1, h2, h3, w0, w1, w2, w3);
    }
  }
  return NULL;
}

// ---------------- Mimic Tests

#ifdef WUFFS_MIMIC

// No mimic tests.

#endif  // WUFFS_MIMIC

// ---------------- VP8 Benches

// No VP8 benches.

// ---------------- Mimic Benches

#ifdef WUFFS_MIMIC

// No mimic benches.

#endif  // WUFFS_MIMIC

// ---------------- Manifest

proc g_tests[] = {

    test_wuffs_vp8_decode_inverse_dct_full,
    test_wuffs_vp8_decode_inverse_wht,
    test_wuffs_vp8_decode_predict_uv8,
    test_wuffs_vp8_decode_predict_y16,
    test_wuffs_vp8_decode_predict_y4,

#ifdef WUFFS_MIMIC

// No mimic tests.

#endif  // WUFFS_MIMIC

    NULL,
};

proc g_benches[] = {

// No VP8 benches.

#ifdef WUFFS_MIMIC

// No mimic benches.

#endif  // WUFFS_MIMIC

    NULL,
};

int  //
main(int argc, char** argv) {
  g_proc_package_name = "std/vp8";
  return test_main(argc, argv, g_tests, g_benches);
}
