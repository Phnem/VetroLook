// Copyright 2026 The Wuffs Authors.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT

package handsum_test

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"image/png"
	"log"
	"os"

	"github.com/google/wuffs/lib/handsum"
)

func Example_decode() {
	srcBytes, err := base64.URLEncoding.DecodeString("" +
		"_tdrdX33KcrhdVU1hFi3h2mtY0qqd2dswzeX9jmJckmUxD15c1Vqk1iGdjmYoknlZDl5dmmZ" +
		"xEczhliahXRDpC3JFoiItjAtQ2vJpjBsUziLiXivgimZqDq6skhwhkxIsimmragKTLa-_Hmn" +
		"C5rf_acAvXR4zebYyq-Qc2SVtVnIA_mX9zVlEmn_Ul9pIzwHaFBb")
	if err != nil {
		log.Fatalf("base64.URLEncoding.DecodeString: %v", err)
	}

	img, err := handsum.Decode(bytes.NewReader(srcBytes))
	if err != nil {
		log.Fatalf("handsum.Decode: %v", err)
	}
	fmt.Printf("%T %v\n", img, img.Bounds())

	// Output:
	// *image.RGBA (0,0)-(16,12)
}

func Example_encode() {
	f, err := os.Open("../../test/data/bricks-color.png")
	if err != nil {
		log.Fatalf("os.Open: %v", err)
	}
	defer f.Close()

	srcImage, err := png.Decode(f)
	if err != nil {
		log.Fatalf("png.Decode: %v", err)
	}

	buf := &bytes.Buffer{}
	err = handsum.Encode(buf, srcImage, nil)
	if err != nil {
		log.Fatalf("handsum.Encode: %v", err)
	}

	s := base64.URLEncoding.EncodeToString(buf.Bytes())
	for ; len(s) > 72; s = s[72:] {
		fmt.Println(s[:72])
	}
	fmt.Println(s)

	// Output:
	// _tdrdX33KcrhdVU1hFi3h2mtY0qqd2dswzeX9jmJckmUxD15c1Vqk1iGdjmYoknlZDl5dmmZ
	// xEczhliahXRDpC3JFoiItjAtQ2vJpjBsUziLiXivgimZqDq6skhwhkxIsimmragKTLa-_Hmn
	// C5rf_acAvXR4zebYyq-Qc2SVtVnIA_mX9zVlEmn_Ul9pIzwHaFBb
}
