// Copyright 2026 The Wuffs Authors.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT

// ----------------

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BASIC_HANDSUM_DECODE_CONFIG__STATIC_FUNCTIONS
#define BASIC_HANDSUM_DECODE_IMPLEMENTATION
#include "../snippet/basic-handsum-decode.c"

// ----

// Hard-code a color=3, quality-4 Handsum image file, known in this repo as
// test/data/mona-lisa.21x32.c3q4.handsum
const uint8_t g_src_ptr__mona_lisa_21_32_c3q4_handsum[] = {
    0xfe, 0xd7, 0x7a, 0x87, 0x76, 0x68, 0x87, 0x75, 0x89, 0x77, 0x8c, 0xb7,
    0xc7, 0x9b, 0x78, 0x64, 0x5a, 0x98, 0xb4, 0x79, 0xe6, 0x75, 0x57, 0x8a,
    0x84, 0x88, 0x03, 0x86, 0x7a, 0x68, 0x87, 0x76, 0x68, 0xf4, 0x55, 0xe2,
    0x68, 0x9a, 0x89, 0xf4, 0x3b, 0x94, 0x75, 0x6a, 0xb8, 0xf8, 0x32, 0x68,
    0x55, 0x6a, 0x78, 0x55, 0x48, 0xac, 0x53, 0x6b, 0xf6, 0x93, 0x3b, 0x78,
    0x83, 0x25, 0x99, 0x92, 0x29, 0xb6, 0x66, 0x6a, 0x9a, 0x81, 0x19, 0x97,
    0x92, 0x2c, 0xb9, 0xd5, 0x3f, 0x88, 0x82, 0x48, 0xa6, 0x91, 0x19, 0x98,
    0x71, 0x38, 0x96, 0xa3, 0x26, 0x79, 0x81, 0x29, 0x97, 0x91, 0x1a, 0x98,
    0x81, 0x19, 0x97, 0x95, 0x58, 0xbb, 0x86, 0x57, 0x98, 0x96, 0x5a, 0x9e,
    0x26, 0x6c, 0x99, 0x87, 0x76, 0x4a, 0x78, 0x84, 0x6a, 0xa9, 0x67, 0x08,
    0x78, 0x94, 0x88, 0x88, 0x97, 0x45, 0xa8, 0x86, 0x78, 0x89, 0xa5, 0x85,
    0xba, 0x86, 0x58, 0x8a, 0x9a, 0xa8, 0x89, 0x9a, 0xa7, 0x79, 0xa9, 0xc8,
    0x99, 0x8b, 0x87,
};
const size_t g_src_len__mona_lisa_21_32_c3q4_handsum = 147;

// ----

int  //
main(int argc, char** argv) {
  basic_handsum_decode__pixel_buffer pixbuf;
  int result = basic_handsum_decode__decode(
      &pixbuf, g_src_ptr__mona_lisa_21_32_c3q4_handsum,
      g_src_len__mona_lisa_21_32_c3q4_handsum);

  if (result != 0) {
    fprintf(stderr, "basic_handsum_decode__decode failed, result code: %d\n",
            result);
    return 1;

  } else if ((pixbuf.width < 1) || (pixbuf.width > 16) ||  //
             (pixbuf.height < 1) || (pixbuf.height > 16)) {
    fprintf(stderr,
            "basic_handsum_decode__decode produced invalid dimensions\n");
    return 1;
  }

  // Print pixbuf to stdout in the NIE image format (doc/spec/nie-spec.md).
  //
  // For test/data/mona-lisa.21x32.c3q4.handsum, the sha256sum of the output
  // should be 9749f54780a135710a50014fa827a04128e614903c48467fe8cf3e0b4ebeff62.
  //
  // This can be compared to Wuffs' full-featured (not just the c3q4 that
  // snippet/basic-handsum-decode.c supports) Handsum decoder implementation:
  //
  // clang-format off
  //
  // ./build-example.sh example/convert-to-nia
  // ./gen/bin/example-convert-to-nia -1 test/data/mona-lisa.21x32.c3q4.handsum | sha256sum
  //
  // clang-format on
  //
  // The example/imageviewer and example/stb-imagedumper programs can also view
  // NIE files directly.

  uint8_t out[1040];
  out[0x0] = 0x6E;
  out[0x1] = 0xC3;
  out[0x2] = 0xAF;
  out[0x3] = 0x45;
  out[0x4] = 0xFF;
  out[0x5] = 0x62;
  out[0x6] = 0x6E;
  out[0x7] = 0x34;
  out[0x8] = pixbuf.width;
  out[0x9] = 0x00;
  out[0xA] = 0x00;
  out[0xB] = 0x00;
  out[0xC] = pixbuf.height;
  out[0xD] = 0x00;
  out[0xE] = 0x00;
  out[0xF] = 0x00;

  size_t n = 4 * pixbuf.width * pixbuf.height;
  memcpy(&out[16], &pixbuf.bgra_pixels[0], n);
  if (write(1, out, 16 + n) < 0) {
    return 1;
  }
  return 0;
}
