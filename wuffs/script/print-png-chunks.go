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

// print-png-chunks.go prints a PNG's chunks' position and type.
//
// Usage: go run print-png-chunks.go foo.png

import (
	"flag"
	"fmt"
	"hash/crc32"
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
		return fmt.Errorf("usage: progname filename.png")
	}
	src, err := os.ReadFile(args[0])
	if err != nil {
		return err
	}

	pos := 0
	if len(src) < 8 {
		return posError(pos, len(src))
	} else if len(src) > 0x7FFF_FFFF {
		return fmt.Errorf("PNG file is too large")
	}

	pos = 8
	for pos < len(src) {
		if pos > (len(src) - 8) {
			return posError(pos, len(src))
		}

		pos0 := pos
		n := int64(u32be(src[pos : pos+4]))
		name := clean(src[pos+4 : pos+8])
		pos += 8

		if int64(len(src)-pos) < n {
			return posError(pos, len(src))
		}
		checksumHave := crc32.ChecksumIEEE(src[pos-4 : pos+int(n)])
		pos += int(n)

		if int64(len(src)-pos) < 4 {
			return posError(pos, len(src))
		}
		checksumWant := u32be(src[pos : pos+4])
		pos += 4

		badChecksum := ""
		if checksumHave != checksumWant {
			badChecksum = " (bad crc32)"
		}
		fmt.Printf("pos: 0x%08X %10d +8, len: 0x%08X %10d +4, %s%s\n",
			pos0, pos0, n, n, name, badChecksum)
	}

	fmt.Printf("pos: 0x%08X %10d                                    ~~~~\n",
		pos, pos)
	return nil
}

func posError(pos int, lenSrc int) error {
	return fmt.Errorf("bad PNG, pos: 0x%08X %10d, len(src): 0x%08X %10d",
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

func u32be(b []byte) uint32 {
	return (uint32(b[0]) << 24) |
		(uint32(b[1]) << 16) |
		(uint32(b[2]) << 8) |
		(uint32(b[3]) << 0)
}
