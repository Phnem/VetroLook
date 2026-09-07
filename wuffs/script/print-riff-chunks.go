// Copyright 2026 The Wuffs Authors.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT

//go:build ignore
// +build ignore

package main

// print-riff-chunks.go prints a RIFF's chunks' position and type.
//
// ANI, AVI, WAV and WEBP files are all RIFF files.
//
// Usage: go run print-riff-chunks.go foo.webp

import (
	"flag"
	"fmt"
	"os"
)

func main() {
	if err := main1(); err != nil {
		os.Stderr.WriteString(err.Error() + "\n")
		os.Exit(1)
	}
}

func main1() error {
	flag.Parse()
	args := flag.Args()
	if len(args) != 1 {
		return fmt.Errorf("usage: progname filename.riff")
	}
	src, err := os.ReadFile(args[0])
	if err != nil {
		return err
	}

	pos := 0
	if len(src) < 12 {
		return posError(pos, len(src))
	} else if len(src) > 0x7FFF_FFFF {
		return fmt.Errorf("RIFF file is too large")
	}
	chunkLen := int64(u32le(src[pos+4 : pos+8]))
	if int64(len(src)-8) < chunkLen {
		return posError(0, len(src))
	}
	isWEBP = string(src[8:12]) == "WEBP"
	fmt.Printf("pos: 0x%08X %10d +8, len: 0x%08X %10d, dep: %d, %s\n",
		0, 0, chunkLen, chunkLen, 0, clean(src[8:12]))

	pos, err = do(src, 1, 12)
	if err != nil {
		return err
	}

	fmt.Printf("pos: 0x%08X %10d                                         ~~~~\n",
		pos, pos)
	return nil
}

func do(src []byte, depth int, pos int) (int, error) {
	for pos < len(src) {
		if pos > (len(src) - 8) {
			return 0, posError(pos, len(src))
		}

		n := int64(u32le(src[pos+4 : pos+8]))
		isWEBPANMF := isWEBP && (depth == 1) && (string(src[pos:pos+4]) == "ANMF")
		fmt.Printf("pos: 0x%08X %10d +8, len: 0x%08X %10d, dep: %d, %s\n",
			pos, pos, n, n, depth, clean(src[pos:pos+4]))
		pos += 8
		if (n & 1) != 0 {
			n++
		}

		if int64(len(src)-pos) < n {
			return 0, posError(pos, len(src))

		} else if isWEBPANMF && (n > 16) {
			pos1, err := do(src[:int64(pos)+n], depth+1, pos+16)
			if err != nil {
				return 0, err
			}
			pos = pos1

		} else {
			pos += int(n)
		}
	}
	return pos, nil
}

var isWEBP bool

func posError(pos int, lenSrc int) error {
	return fmt.Errorf("bad RIFF, pos: 0x%08X %10d, len(src): 0x%08X %10d",
		pos, pos, lenSrc, lenSrc)
}

func clean(b []byte) string {
	a := [4]byte{}
	for i, c := range b[:4] {
		if (c < '!') || ('~' < c) {
			c = '.'
		}
		a[i] = c
	}
	return string(a[:4])
}

func u32le(b []byte) uint32 {
	return (uint32(b[0]) << 0) |
		(uint32(b[1]) << 8) |
		(uint32(b[2]) << 16) |
		(uint32(b[3]) << 24)
}
