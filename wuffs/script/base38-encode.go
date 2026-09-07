// Copyright 2020 The Wuffs Authors.
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

// base38-encode.go prints the base38 encoding of each argument.
//
// Usage: go run base38-encode.go gif json text/html/..

import (
	"fmt"
	"os"
	"strings"

	"github.com/google/wuffs/lib/base38"
)

func main() {
	if err := main1(); err != nil {
		os.Stderr.WriteString(err.Error() + "\n")
		os.Exit(1)
	}
}

func main1() error {
	args := os.Args
	if len(args) > 1 {
		for _, arg := range args[1:] {
			if len(arg) <= 4 {
				arg := (strings.ToLower(arg) + "....")[:4]
				if code, ok := base38.Encode(arg); ok {
					code0 := fmt.Sprintf("0x%06X", code)
					code1 := fmt.Sprintf("0x%08X", code<<10)
					fmt.Printf("%s    code: %s %s    code<<10: %s %s\n",
						arg, code0, underscore4(code0), code1, underscore4(code1))
					continue
				}

			} else if len(arg) == 7 {
				// Assume that arg is of the form "aaaa/bb".
				arg0 := arg[0:4]
				arg1 := ".." + arg[5:7]
				c0, ok0 := base38.Encode(arg0)
				c1, ok1 := base38.Encode(arg1)
				if ok0 && ok1 {
					c := (c0 << 11) | c1
					code := fmt.Sprintf("0x%08X", c)
					fmt.Printf("%s\t%10d\t%s\n", underscore4(code), c, arg)
					continue
				}

			} else if len(arg) == 12 {
				if (arg[4] == '/') && (arg[7] == '/') {
					// Assume that arg is of the form "aaaa/bb/cccc".
					arg0 := arg[0:4]
					arg1 := ".." + arg[5:7]
					arg2 := arg[8:12]
					c0, ok0 := base38.Encode(arg0)
					c1, ok1 := base38.Encode(arg1)
					c2, ok2 := base38.Encode(arg2)
					if ok0 && ok1 && ok2 {
						c := (uint64(c0) << 32) | (uint64(c1) << 21) | uint64(c2)
						code := fmt.Sprintf("0x%014X", c)
						fmt.Printf("%s\t%16d\t%s\n", underscore8(code), c, arg)
						continue
					}

				} else if (arg[4] == '/') && (arg[9] == '/') {
					// Assume that arg is of the form "aaaa/bbbb/cc".
					arg0 := arg[0:4]
					arg1 := arg[5:9]
					arg2 := ".." + arg[10:12]
					c0, ok0 := base38.Encode(arg0)
					c1, ok1 := base38.Encode(arg1)
					c2, ok2 := base38.Encode(arg2)
					if ok0 && ok1 && ok2 {
						c := (uint64(c0) << 32) | (uint64(c1) << 11) | uint64(c2)
						code := fmt.Sprintf("0x%014X", c)
						fmt.Printf("%s\t%16d\t%s\n", underscore8(code), c, arg)
						continue
					}
				}

			} else if len(arg) == 14 {
				// Assume that arg is of the form "aaaa/bbbb/cccc".
				arg0 := arg[0:4]
				arg1 := arg[5:9]
				arg2 := arg[10:14]
				c0, ok0 := base38.Encode(arg0)
				c1, ok1 := base38.Encode(arg1)
				c2, ok2 := base38.Encode(arg2)
				if ok0 && ok1 && ok2 {
					c := (uint64(c0) << 42) | (uint64(c1) << 21) | uint64(c2)
					code := fmt.Sprintf("0x%016X", c)
					fmt.Printf("%s\t%19d\t%s\n", underscore8(code), c, arg)
					continue
				}
			}

			fmt.Printf("%s -\n", arg)
		}
	}
	return nil
}

func underscore4(s string) string {
	if n := len(s) - 4; n > 0 {
		return s[:n] + "_" + s[n:]
	}
	return s
}

func underscore8(s string) string {
	if n := len(s) - 8; n > 0 {
		return s[:n] + "_" + s[n:]
	}
	return s
}
