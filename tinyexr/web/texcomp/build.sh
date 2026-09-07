#!/bin/sh
# Build the texcomp / tir browser demo (WASM).
#
# Links the four pure-C11 tools plus the TinyEXR v3 core:
#   src/           EXR decode (so HDR sources load straight from openexr-images)
#   tools/resize   tir      content-aware resize
#   tools/texcomp  texcomp  block compression + decompression
#   tools/texpipe  texpipe  mip chains, containers (KTX2/DDS)
#   tools/envmap   envmap   equirect -> cubemap / octahedral
#
# Requires the Emscripten SDK on PATH (override with EMCC=...).
#   ./build.sh          # baseline
#   SIMD=1 ./build.sh   # -msimd128 (faster ASTC/BC7 encode in-browser)
set -e

cd "$(dirname "$0")"
ROOT=../..
EMCC="${EMCC:-emcc}"
OPT="${OPT:--O3}"

SIMD_FLAGS=""
if [ "${SIMD:-0}" = "1" ]; then
  SIMD_FLAGS="-msimd128"
  echo "building with -msimd128"
fi

INC="-I$ROOT/include -I$ROOT/src -I$ROOT/deps/zstd \
     -I$ROOT/tools/resize/include \
     -I$ROOT/tools/texcomp/include \
     -I$ROOT/tools/texpipe/include \
     -I$ROOT/tools/envmap/include"

# EXR core (no stdio needed in the browser) + zstd/libdeflate for the
# compressed EXRs that openexr-images actually ships.
SRC="texcomp_web.c \
     $ROOT/deps/zstd/tinyexr_zstd.c \
     $(ls $ROOT/src/*.c | grep -v exr_gpu_cuda | grep -v exr_vk_vulkan) \
     $(ls $ROOT/tools/resize/src/*.c | grep -v tir_cli) \
     $(ls $ROOT/tools/texcomp/src/*.c | grep -v texcomp_cli) \
     $(ls $ROOT/tools/texpipe/src/*.c | grep -v texpipe_cli) \
     $(ls $ROOT/tools/envmap/src/*.c | grep -v envmap_cli)"

EXPORTS="['_tcw_load_exr','_tcw_load_rgba8','_tcw_src_width','_tcw_src_height',\
'_tcw_src_is_hdr','_tcw_work_width','_tcw_work_height','_tcw_resize','_tcw_use_source',\
'_tcw_compress','_tcw_decode','_tcw_psnr','_tcw_normal_angular_error','_tcw_preview',\
'_tcw_blocks_size','_tcw_blocks_ptr','_tcw_container_size','_tcw_container_ptr',\
'_tcw_encode_ms','_tcw_decode_ms','_tcw_message','_tcw_backend','_tcw_codec_is_hdr',\
'_tcw_build_mips','_tcw_mip_width','_tcw_mip_height','_tcw_select_mip',\
'_tcw_project_env','_tcw_normal_from_height','_tcw_write_container','_tcw_ktx2_info',\
'_malloc','_free']"

RUNTIME="['HEAPU8','HEAPF32','HEAP32','HEAPU32','UTF8ToString','ccall','cwrap']"

# TC_NO_THREADS: the encoders' C11 <threads.h> path is not available here.
$EMCC $OPT $SIMD_FLAGS -std=c11 -w \
  -DTC_NO_THREADS=1 -DTINYEXR_USE_THREAD=0 \
  $INC $SRC \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1MB \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS="$RUNTIME" \
  -o texcomp_web.mjs

echo
echo "Built texcomp_web.mjs + texcomp_web.wasm"
ls -lh texcomp_web.wasm
echo "Serve with:  python3 -m http.server   (then open http://localhost:8000/)"
