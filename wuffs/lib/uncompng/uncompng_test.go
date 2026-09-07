// Copyright 2024 The Wuffs Authors.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT

package uncompng

import (
	"bytes"
	"compress/zlib"
	"hash/crc32"
	"image"
	"image/draw"
	"image/png"
	"io"
	"os"
	"testing"
)

func encodeImage(w io.Writer, src image.Image) error {
	e := Encoder{}
	b := src.Bounds()

	switch src := src.(type) {
	case *image.Gray:
		return e.Encode(w, Depth8, ColorTypeGray, b.Dx(), b.Dy(), src.Pix, src.Stride)

	case *image.Gray16:
		return e.Encode(w, Depth16, ColorTypeGray, b.Dx(), b.Dy(), src.Pix, src.Stride)

	case *image.RGBA:
		if src.Opaque() {
			return e.Encode(w, Depth8, ColorTypeRGBX, b.Dx(), b.Dy(), src.Pix, src.Stride)
		}

	case *image.RGBA64:
		if src.Opaque() {
			return e.Encode(w, Depth16, ColorTypeRGBX, b.Dx(), b.Dy(), src.Pix, src.Stride)
		}

	case *image.NRGBA:
		if src.Opaque() {
			return e.Encode(w, Depth8, ColorTypeRGBX, b.Dx(), b.Dy(), src.Pix, src.Stride)
		} else {
			return e.Encode(w, Depth8, ColorTypeNRGBA, b.Dx(), b.Dy(), src.Pix, src.Stride)
		}

	case *image.NRGBA64:
		if src.Opaque() {
			return e.Encode(w, Depth16, ColorTypeRGBX, b.Dx(), b.Dy(), src.Pix, src.Stride)
		} else {
			return e.Encode(w, Depth16, ColorTypeNRGBA, b.Dx(), b.Dy(), src.Pix, src.Stride)
		}
	}

	tmp := image.NewNRGBA(b)
	draw.Draw(tmp, b, src, b.Min, draw.Src)
	return e.Encode(w, Depth8, ColorTypeNRGBA, b.Dx(), b.Dy(), tmp.Pix, tmp.Stride)
}

func getPix(m image.Image) []byte {
	switch m := m.(type) {
	case *image.Gray:
		return m.Pix
	case *image.Gray16:
		return m.Pix
	case *image.RGBA:
		return m.Pix
	case *image.RGBA64:
		return m.Pix
	case *image.NRGBA:
		return m.Pix
	case *image.NRGBA64:
		return m.Pix
	}
	return nil
}

func TestRoundTrip(tt *testing.T) {
	testCases := []string{
		"36.png",
		"49.png",
		"bricks-color.png",
		"bricks-gray.png",
		"harvesters.png",
		"hibiscus.primitive.png",
		"hibiscus.regular.png",
		"hippopotamus.masked-with-muybridge.png",
		"hippopotamus.regular.png",
	}

	for _, tc := range testCases {
		testRoundTrip(tt, tc)
	}
}

func testRoundTrip(tt *testing.T, basename string) {
	src, err := os.Open("../../test/data/" + basename)
	if err != nil {
		tt.Errorf("%q: os.Open: %v", basename, err)
		return
	}
	defer src.Close()

	img0, err := png.Decode(src)
	if err != nil {
		tt.Errorf("%q: png.Decode #0: %v", basename, err)
		return
	}

	buf := &bytes.Buffer{}
	err = encodeImage(buf, img0)
	if err != nil {
		tt.Errorf("%q: encodeImage: %v", basename, err)
		return
	}

	img1, err := png.Decode(buf)
	if err != nil {
		tt.Errorf("%q: png.Decode #1: %v", basename, err)
		return
	}

	rect1, rect0 := img1.Bounds(), img0.Bounds()
	if rect1 != rect0 {
		tt.Errorf("%q: rect1: got %v, want %v", basename, rect1, rect0)
		return
	}

	pix1, pix0 := getPix(img1), getPix(img0)
	if pix1 == nil {
		tt.Errorf("%q: pix1 was nil", basename)
		return
	} else if !bytes.Equal(pix1, pix0) {
		tt.Errorf("%q: pix1 differed from pix0", basename)
		return
	}
}

func TestNoAllocation(tt *testing.T) {
	enc := Encoder{}
	pix := make([]byte, 4*3*5)
	got := testing.AllocsPerRun(100, func() {
		enc.Encode(io.Discard, Depth8, ColorTypeNRGBA, 3, 5, pix, 4*3)
	})
	if got != 0 {
		tt.Errorf("AllocsPerRun: got %v, want 0", got)
		return
	}
}

type lookingForSeparateIENDChunkBuffer struct {
	buf  bytes.Buffer
	seen bool
}

func (z *lookingForSeparateIENDChunkBuffer) Write(b []byte) (int, error) {
	const iendChunk = "\x00\x00\x00\x00IEND\xAE\x42\x60\x82"
	if (len(b) == len(iendChunk)) && (string(b) == iendChunk) {
		z.seen = true
	}
	return z.buf.Write(b)
}

func TestWriteSeparateIENDChunk(tt *testing.T) {
	lfsicb := lookingForSeparateIENDChunkBuffer{}
	rect0 := image.Rect(0, 0, 65470, 1)
	img0 := image.NewGray(rect0)
	if err := encodeImage(&lfsicb, img0); err != nil {
		tt.Fatalf("encodeImage: %v", err)
	}
	img1, err := png.Decode(&lfsicb.buf)
	if err != nil {
		tt.Fatalf("png.Decode: %v", err)
	}
	rect1 := img1.Bounds()
	if rect1 != rect0 {
		tt.Fatalf("rect1: got %v, want %v", rect1, rect0)
	}
	if !lfsicb.seen {
		tt.Fatalf("have not seen a separate IEND chunk")
	}
}

// TestAnimationEncoder recreates the "Example NIA File" animation from
// doc/spec/nie-spec.md, but in APNG format, not NIA format:
//
// This animated image is 3 pixels wide and 2 pixels high. It consists of 20
// frames, being 10 loops of 2 frames. The total animation time of a single
// loop is 3 seconds, so the 10 loops will take 30 seconds. The first frame is
// a crude approximation to the French flag (blue, white and red) and is shown
// for 1 second. The next frame is a crude approximation to the Italian flag
// (green, white and red) and is shown for (3 - 1) seconds (i.e., 2 seconds).
//
// This test just hard-codes the expected golden output (in APNG format).
// Saving the output to a file instead, and then transforming its contents via
// convert-to-nia, should recover the same bytes (in NIA format) as given in
// doc/spec/nie-spec.md.
func TestAnimationEncoderSmall(tt *testing.T) {
	want := "" +
		"\x89PNG\x0D\x0A\x1A\x0A" + // PNG magic signature.
		"" +
		"\x00\x00\x00\x0DIHDR" + // Chunk length and type.
		"\x00\x00\x00\x03" + // Width.
		"\x00\x00\x00\x02" + // Height.
		"\x08\x02\x00\x00\x00" + // Depth and other fields.
		"\x12\x16\xF1\x4D" + // Chunk checksum (CRC-32).
		"" +
		"\x00\x00\x00\x08acTL" + // Chunk length and type.
		"\x00\x00\x00\x02" + // 2 frames.
		"\x00\x00\x00\x0A" + // 10 plays.
		"\x13\x58\x7A\x6E" + // Chunk checksum (CRC-32).
		"" + //
		"\x00\x00\x00\x1AfcTL" + // Chunk length and type.
		"\x00\x00\x00\x00" + // Animation sequence number.
		"\x00\x00\x00\x03" + // Width.
		"\x00\x00\x00\x02" + // Height.
		"\x00\x00\x00\x00" + // XOffset.
		"\x00\x00\x00\x00" + // YOffset.
		"\x03\xE8\x03\xE8" + // Delay (numerator = 1000, denominator = 1000).
		"\x00\x00" + // DisposeOp, BlendOp.
		"\x24\x35\x20\x78" + // Chunk checksum (CRC-32).
		"" + //
		"\x00\x00\x00\x1FIDAT" + // Chunk length and type.
		"\x78\x01\x01\x14\x00\xEB\xFF" + // ZLIB, final block, 20 bytes.
		"\x00" + // PNG row filter.
		"\x00\x00\xFF" + // RGB pixel.
		"\xFF\xFF\xFF" + // RGB pixel.
		"\xFF\x00\x00" + // RGB pixel.
		"\x00" + // PNG row filter.
		"\x00\x00\xFF" + // RGB pixel.
		"\xFF\xFF\xFF" + // RGB pixel.
		"\xFF\x00\x00" + // RGB pixel.
		"\x63\xB0\x09\xF7" + // ZLIB checksum (Adler32).
		"\xDD\xD2\x78\x8A" + // Chunk checksum (CRC-32).
		"" + //
		"\x00\x00\x00\x1AfcTL" + // Chunk length and type.
		"\x00\x00\x00\x01" + // Animation sequence number.
		"\x00\x00\x00\x03" + // Width.
		"\x00\x00\x00\x02" + // Height.
		"\x00\x00\x00\x00" + // XOffset.
		"\x00\x00\x00\x00" + // YOffset.
		"\x07\xD0\x03\xE8" + // Delay (numerator = 2000, denominator = 1000).
		"\x00\x00" + // DisposeOp, BlendOp.
		"\xB5\x86\x7B\xFD" + // Chunk checksum (CRC-32).
		"" + //
		"\x00\x00\x00\x23fdAT" + // Chunk length and type.
		"\x00\x00\x00\x02" + // Animation sequence number.
		"\x78\x01\x01\x14\x00\xEB\xFF" + // ZLIB, final block, 20 bytes.
		"\x00" + // PNG row filter.
		"\x00\xFF\x00" + // RGB pixel.
		"\xFF\xFF\xFF" + // RGB pixel.
		"\xFF\x00\x00" + // RGB pixel.
		"\x00" + // PNG row filter.
		"\x00\xFF\x00" + // RGB pixel.
		"\xFF\xFF\xFF" + // RGB pixel.
		"\xFF\x00\x00" + // RGB pixel.
		"\x65\xAE\x09\xF7" + // ZLIB checksum (Adler32).
		"\xFA\x44\x2A\xBE" + // Chunk checksum (CRC-32).
		"" + //
		"\x00\x00\x00\x00IEND" + // Chunk length and type.
		"\xAE\x42\x60\x82" + // Chunk checksum (CRC-32).
		""

	const bytesPerPixel = 4
	const width = 3
	const height = 2
	const numFrames = 2
	const numPlays = 10

	buf := bytes.Buffer{}
	e := AnimationEncoder{}

	pixs := [numFrames][bytesPerPixel * width * height]byte{{
		0x00, 0x00, 0xFF, 0xFF, // Blue.
		0xFF, 0xFF, 0xFF, 0xFF, // White.
		0xFF, 0x00, 0x00, 0xFF, // Red.

		0x00, 0x00, 0xFF, 0xFF, // Blue.
		0xFF, 0xFF, 0xFF, 0xFF, // White.
		0xFF, 0x00, 0x00, 0xFF, // Red.

	}, {
		0x00, 0xFF, 0x00, 0xFF, // Green.
		0xFF, 0xFF, 0xFF, 0xFF, // White.
		0xFF, 0x00, 0x00, 0xFF, // Red.

		0x00, 0xFF, 0x00, 0xFF, // Green.
		0xFF, 0xFF, 0xFF, 0xFF, // White.
		0xFF, 0x00, 0x00, 0xFF, // Red.
	}}

	delayMillis := [numFrames]uint16{1000, 2000}

	if err := e.EncodeHeader(&buf, Depth8, ColorTypeRGBX, width, height, numFrames, numPlays); err != nil {
		tt.Fatalf("EncodeHeader: %v", err)
	}

	for f := range pixs {
		if err := e.EncodeFrame(&buf, pixs[f][:], bytesPerPixel*width, delayMillis[f], 1000); err != nil {
			tt.Fatalf("EncodeFrame(%d): %v", f, err)
		}
	}

	asBytes := buf.Bytes()
	checkAPNGConsistency(tt, asBytes)

	if got := string(asBytes); got != want {
		for i := range min(len(got), len(want)) {
			if got[i] != want[i] {
				tt.Fatalf("compare-to-golden: at byte %d\ngot  % 02X\nwant % 02X", i, got, want)
			}
		}
		tt.Fatalf("compare-to-golden:\ngot  % 02X\nwant % 02X", got, want)
	}
}

func TestAnimationEncoderLarge(tt *testing.T) {
	srcFile, err := os.Open("../../test/data/hibiscus.regular.png")
	if err != nil {
		tt.Fatalf("os.Open: %v", err)
	}
	defer srcFile.Close()

	srcImage, err := png.Decode(srcFile)
	if err != nil {
		tt.Fatalf("png.Decode: %v", err)
	}
	srcRGBA := srcImage.(*image.RGBA)
	if got, want := srcRGBA.Bounds(), image.Rect(0, 0, 312, 442); got != want {
		tt.Fatalf("Bounds: got %v, want %v", got, want)
	}
	srcPix := srcRGBA.Pix

	// Make a copy of srcPix. PNG alpha is non-premul and Go's RGBA alpha is
	// premul. The fact that Go's png.Decode returned RGBA (instead of NRGBA)
	// means that all of the alpha values should be 0xFF, so that converting
	// srcPix (as RGBA) to srcPixAsNRGBA can just be a memcpy.
	srcPixAsNRGBA := append([]byte(nil), srcPix...)

	buf := bytes.Buffer{}
	e := AnimationEncoder{}

	const bytesPerPixel = 4
	const width = 312
	const height = 442
	const numFrames = 5
	const numPlays = 0
	delayMillis := [numFrames]uint16{300, 500, 600, 400, 200}

	if err := e.EncodeHeader(&buf, Depth8, ColorTypeNRGBA, width, height, numFrames, numPlays); err != nil {
		tt.Fatalf("EncodeHeader: %v", err)
	}

	for f, delay := range delayMillis {
		if err := e.EncodeFrame(&buf, srcPix, bytesPerPixel*width, delay, 1000); err != nil {
			tt.Fatalf("EncodeFrame(%d): %v", f, err)
		}

		// Over time, set the R, G, B, A channel values to 0x80.
		if f < 4 {
			for i := f; i < len(srcPix); i += 4 {
				srcPix[i] = 0x80
			}
		}
	}

	asBytes := buf.Bytes()
	checkAPNGConsistency(tt, asBytes)

	if got, want := crc32.ChecksumIEEE(asBytes), uint32(0x73B25773); got != want {
		tt.Fatalf("did not get golden-output hash")
	}

	// png.Decode from Go's standard library doesn't recover all of the
	// animation's frames, but it should recover the first frame.
	//
	// We also passed ColorTypeNRGBA (not ColorTypeRGBX) to EncodeHeader, so
	// the decoded image is *image.NRGBA, not *image.RGBA.
	dstImage, err := png.Decode(bytes.NewReader(asBytes))
	if err != nil {
		tt.Fatalf("png.Decode: %v", err)
	}
	dstNRGBA := dstImage.(*image.NRGBA)
	if !bytes.Equal(dstNRGBA.Pix, srcPixAsNRGBA) {
		tt.Fatal("first frame pixels: round-trip failed")
	}

	// Change false to true, for manual inspection in a viewer that supports
	// APNG, such as a web browser.
	if false {
		os.WriteFile("/tmp/TestAnimationEncoderLarge.png", asBytes, 0644)
	}
}

func checkAPNGConsistency(tt *testing.T, pngEncodedBytes []byte) {
	if !bytes.HasPrefix(pngEncodedBytes, []byte("\x89PNG\x0D\x0A\x1A\x0A")) {
		tt.Fatalf("bad PNG magic signature")
	}

	forEachChunk := func(f func(chunk []byte)) {
		for b := pngEncodedBytes[8:]; len(b) > 0; {
			if len(b) < 12 {
				tt.Fatalf("bad chunk structure")
			}
			n := int64(u32be(b))
			if (n + 12) > int64(len(b)) {
				tt.Fatalf("bad chunk structure")
			}
			f(b[:n+12])
			b = b[n+12:]
		}
	}

	forEachChunk(func(chunk []byte) {
		checksum0 := u32be(chunk[len(chunk)-4:])
		checksum1 := crc32.ChecksumIEEE(chunk[4 : len(chunk)-4])
		if checksum0 != checksum1 {
			tt.Fatalf("bad chunk checksum")
		}
	})

	nextSeqNum := uint32(0)
	forEachChunk(func(chunk []byte) {
		switch u32be(chunk[4:]) {
		case 0x6663544C, 0x66644154: // "fcTL"be, "fdAT"be.
			got := u32be(chunk[8:])
			want := nextSeqNum
			nextSeqNum++
			if got != want {
				tt.Fatalf("bad animation sequence number")
			}
		}
	})

	idatlikeData := [][]byte{}
	forEachChunk(func(chunk []byte) {
		dat := []byte(nil)

		switch u32be(chunk[4:]) {
		case 0x6663544C: // "fcTL"be.
			idatlikeData = append(idatlikeData, nil)
		case 0x66644154: // "fdAT"be.
			dat = chunk[12 : len(chunk)-4]
		case 0x49444154: // "IDAT"be.
			dat = chunk[8 : len(chunk)-4]
		}

		if dat != nil {
			last := len(idatlikeData) - 1
			idatlikeData[last] = append(idatlikeData[last], dat...)
		}
	})

	decompressedLen := int64(0)
	for i, dat := range idatlikeData {
		r, err := zlib.NewReader(bytes.NewReader(dat))
		if err != nil {
			tt.Fatalf("IDAT/fdAT chunk data was not ZLIB-formatted")
		}
		defer r.Close()
		if n, err := io.Copy(io.Discard, r); err != nil {
			tt.Fatalf("IDAT/fdAT chunk data was not ZLIB-formatted")
		} else if i == 0 {
			decompressedLen = n
		} else if decompressedLen != n {
			tt.Fatalf("IDAT/fdAT chunk data has inconsistent lengths")
		}
	}
}

func u32be(b []byte) uint32 {
	return (uint32(b[0]) << 24) |
		(uint32(b[1]) << 16) |
		(uint32(b[2]) << 8) |
		(uint32(b[3]) << 0)
}
