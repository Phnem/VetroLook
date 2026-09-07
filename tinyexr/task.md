 S1 - CRITICAL: heap OOB write when a reader entry point is called twice on a malformed file

 src/exr_reader.c:539, memset(np, 0, sizeof(*np)) writes one exr_int_part (464 B) past the end of r->parts.

 Root cause: parts capacity lives in a local int32_t cap in exr_reader_parse_header, while r->parts / r->num_parts persist across
 calls. Failure paths do r->num_parts++ (so exr_reader_close frees the partial part) without growing the array, and r->parsed is only
 set on success. A second call re-enters with num_parts=2, local cap=0, so num_parts == cap is false, no realloc happens, and np =
 &r->parts[2] lands exactly on the block boundary.

 ```
   READ of size 464 -> WRITE of size 464 at 0 bytes after 928-byte region
     #1 exr_reader_parse_header src/exr_reader.c:539
     #2 exr_reader_num_blocks   src/exr_reader.c:1563
 ```

 Repro (ordinary API sequence, 184-byte file, no fuzzer needed):

 ```c
   exr_reader_open_memory(buf, n, NULL, &r);
   exr_reader_read_part(r, 0, &out);   /* -> EXR_ERROR_CORRUPT, leaves num_parts > cap */
   exr_reader_num_blocks(r, 0, &nb);   /* -> OOB write */
 ```

 -O2 non-sanitized build: malloc(): invalid size (unsorted) + core dump. Every public entry point (read_part, read_scanlines,
 read_tile, num_blocks, block_info, decode_block, decode_deep_*) calls parse_header, so any app that retries or queries state after
 an error is exposed. Streaming hosts that re-issue a call after supply() hit it directly. A retry also leaks the previous attr list
 / channel array (re-allocated unconditionally).

 Fix: add int32_t parts_cap to struct exr_reader; latch the parse outcome (r->parse_rc) so a failed parse returns the same error
 instead of re-parsing; if you want retries to actually work, reset part state (exr_header_free + exr_free(parts), num_parts=0) at
 the top of a retry.

 ────────────────────────────────────────────────────────────────────────────────

 S2 - HIGH: unbounded image allocation (DoS, fuzz OOM)

 Artifact: /tmp/fz/oom-5de6bc6d... (604 KB) requests 13.22 GB in one calloc.
 dataWindow = (1,11008,130820,65280) -> 7.1 Gpx, 1 channel HALF = 130820x54273x2. Both axes pass EXR_MAX_DIMENSION (1<<20 per axis),
 so the per-axis cap never fires; worst case is ~16 TB. read_part materializes whole planes with calloc+memset, so with overcommit
 this is an OOM kill rather than an error return.

 Fix: a total-pixel/byte budget checked before allocating planes (exr_context_set_max_image_bytes(), generous default, plus
 EXR_ERROR_LIMIT_EXCEEDED), mirroring libpng user-warnings/limits. exr_reader_decode_block is already bounded per block, so this is a
 read-part-only gap. Also consider an EXR_MAX_FILE_SIZE in exr_stdio.c, which slurps the whole file with no cap.

 ────────────────────────────────────────────────────────────────────────────────

 S3 - HIGH: sanitizer gates do not fail on sanitizer output

 1. SAN = -fsanitize=address,undefined has no -fno-sanitize-recover=all. UBSan prints runtime error: and exits 0. Verified:
    ./build/fuzz_replay /tmp/bad/tile_xsize_7fffffff.exr prints two UB reports and returns 0, so make fuzz-corpus and make test-c are
    green with UB present.
 2. make fuzz-corpus SAN=... relinks but does not recompile build/test-*.o (no flag dependency), and -fsanitize-recover is baked in
    per object. My first "strict" build silently passed for exactly this reason. Add a flags stamp file as an order-only
    prerequisite, or document make clean when changing SAN.
 3. make fuzz-corpus runs ... asakusa.exr deepscanline.exr, but the file is data/deepscanline.exr: the log says skip (open failed):
    deepscanline.exr and still exits 0. A missing corpus entry must be fatal.
 4. make test-c hardcodes ASAN_OPTIONS=detect_leaks=0, so unit-test error paths get no leak checking (only fuzz-corpus runs LSan).

 Verified safe to tighten: the full regression corpus (all 23 files, including the extension-less poc-*) is clean under a
 properly-built -fno-sanitize-recover=all replay, so turning it on costs nothing today.

 ────────────────────────────────────────────────────────────────────────────────

 S4 - MEDIUM: tile sizes unvalidated -> signed overflow UB

 ```
   src/exr_reader.c:412  int nx = (p->width + tx - 1) / tx   /* 64 + 2147483647 */
   src/exr_reader.c:987  nxt  = (p->width + tx - 1) / tx
   src/exr_reader.c:397  tiled_level_size() same expression
 ```

 tile_x_size / tile_y_size are read as uint32_t and only checked <= 0 after an (int) cast. 0x7FEFFFFF..0x7FFFFFFF overflows the
 ceil-div; nxt can come back 0 or negative (currently survives only because downstream idx < base + cnt and if (nxt <= 0) guards
 happen to be strict).
 Fix: validate in interpret_header - power of two, 1 <= size <= (1<<26) (all real tiled EXR use 16..256) - and/or do the ceil-div in
 64-bit. Requiring power-of-two matches every real producer but is a compat decision; the 64-bit fix alone is behaviour-preserving.

 S5 - MEDIUM: writer performs no header validation -> SIGFPE and NULL memcpy

 ┌───────────────────┬──────────────────────────────────────────────┬────────────────────────────────────────────────────┐
 │ Location          │ Input                                        │ Result                                             │
 ├───────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────────────────┤
 │ exr_writer.c:1343 │ data_window INT_MIN..INT_MAX                 │ signed overflow UB in max_x - min_x + 1            │
 ├───────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────────────────┤
 │ exr_writer.c:239  │ y_sampling == 0                              │ division by zero (SIGFPE) in gather_scanline_block │
 ├───────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────────────────┤
 │ exr_writer.c:243  │ channel never set via exr_writer_set_channel │ memcpy(NULL, ...) -> SEGV at -O2                   │
 └───────────────────┴──────────────────────────────────────────────┴────────────────────────────────────────────────────┘

 The reader validates all of this in interpret_header; the writer has no equivalent (add_part checks nothing: no window sanity, no
 num_channels > 0, no sampling >= 1, no name length vs long_names). Add a shared exr_header_validate() used by both sides - it also
 guards the "my output is accepted by OpenEXR" property.

 S6 - MEDIUM: fuzz harness misses half the public API

 test/fuzzer/fuzz_v3.c never calls exr_reader_read_scanlines, exr_reader_read_tile (so mipmap/ripmap level math is unfuzzed),
 exr_reader_decode_deep_counts / _decode_deep_samples, or exr_block_extract_channel; drain_blocks explicitly skips bi.is_deep, and
 the WOULD_BLOCK suspend/resume loop is never exercised (the source always answers). It also allocates with the default allocator, so
 hostile headers turn libFuzzer itself into an OOM abort (F3 artifact) instead of exercising the OOM error paths.

 Proposal, already prototyped and working (/tmp/fuzz_ext.c, builds clean, finds S1-class paths):
 - derive y_start / y_count / tile / channel indices from fuzz bytes
 - deep two-step with a capped sample total; block_extract_channel sized via exr_num_samples
 - a source returning WOULD_BLOCK every 3rd call, driving the pending/supply resume loop
 - a budgeted exr_allocator (refuses > N bytes): stops fuzz OOM noise and systematically covers every if (!p) return OUT_OF_MEMORY
   path, which is where leaks and double frees hide. A stricter variant - fail the Nth allocation, sweep N - is cheap and matches the
   "error paths must leave outputs owning nothing" rule.
 - call parse_header twice per reader (idempotency check; this is what surfaces S1)
 - add an EXR token dictionary (libFuzzer's recommended dictionary already spits out chunkCount, dataWindow, ...)
 - add the malformed corpus: my 48-case generator (/tmp/gen_bad.py) covers tile sizes, chunkCount abuse, sampling 0/neg/huge, wrapped
   windows, duplicate/short required attributes, multipart flag vs type mismatches, version-flag abuse, truncation ladder. All 48
   currently pass, so they are free regression coverage.

 S7 - LOW: API-level gaps

 - exr_reader_supply ignores pending.offset/pending.size and appends blindly at filled: a host that feeds bytes out of order silently
   corrupts the buffer instead of erroring.
 - exr_block_extract_channel has no dst_size; it bounds-checks the source only. The header comment tells callers to size dst with
   exr_num_samples(), which is not public (declared only in src/exr_internal.h) - so callers must hand-roll the formula that decides
   the write extent. Either export it or add a dst_size parameter.
 - interpret_header uses the magic at->data[0] > 12 for the compression bound instead of the enum max.

 S8 - LOW: duplicated geometry helpers, inconsistent clamping

 Three copies of the same math: nsamp/floordiv64 (exr_reader.c:609,614), num_samples/floordiv (exr_core.c:111,118),
 exr_num_samples/exr_floordiv (exr_internal.h:494,499). Sampling is clamped defensively in exr_convert.c / exr_mip.c (< 1 ? 1 :) but
 used raw with % ys in exr_b44.c, exr_piz.c, exr_pxr24.c, exr_codec paths - safe today only because exr_parse_chlist rejects <= 0.
 Consolidate on the exr_internal.h inlines plus one normalize step, so the invariant has a single owner.

 S9 - LOW: two arithmetic sites bypass the exr_add_ovf / exr_mul_ovf convention

 src/exr_deep.c:193 total += prev; (unchecked 64-bit accumulate of per-chunk sample totals) and src/exr_deep.c:176-179 exr_malloc(a,
 need * sizeof(int32_t)). Practically unreachable, but AGENTS.md makes checked arithmetic a hard rule and this is the kind of site a
 future cap change turns live.

 S10 - LOW: thread-safety coverage

 make test-c-tsan dies under TSan's own __tsan_func_entry with par_worker (src/exr_thread.c:106) on the frame below - the known
 glibc-TSan vs C11 <threads.h> gap the Makefile comment mentions. Net effect: zero race coverage, no TSan job in CI, and a target
 that looks like a gate but cannot run. A plain pthread_create path behind EXR_USE_THREADS (the Apple dispatch path already exists)
 fixes it.
 Separately, exr_simd_init() (exr_cpu.c:142) and cpu_caps_cached() (:68) use non-atomic static int done/ready. Internal parallel
 paths are fine because callers "warm the vtbl" first (and PIZ deliberately keeps its workspace off the threaded path), but two
 application threads making their first library call concurrently can observe a half-populated vtable. Cheap fix: statically
 initialize the vtable to the scalar kernels so an upgrade race can never expose a NULL slot, and make the done flag an atomic
 exchange.

 S11 - LOW: exr_stdio.c uses long offsets

 long sz = ftell(fp) and fseek(fp, (long)off, SEEK_SET) are 32-bit on LLP64/32-bit: a >2 GB file cannot be read, and a >2 GB streamed
 write seeks to a truncated offset, producing a silently corrupt file. Use ftello/fseeko (_fseeki64 on MSVC).
