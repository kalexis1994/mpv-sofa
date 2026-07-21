#!/usr/bin/env bash
# Build the HaloSound DSP to WebAssembly, WITH libmysofa statically linked so
# real SOFA HRTF filtering happens on the client (not the dirac-ITD stub).
#
# libmysofa is compiled from source (github.com/hoene/libmysofa) because the
# WASM build has no filesystem: we load SOFA from a memory buffer via
# mysofa_open_data. Three headers that CMake normally generates are provided
# in $MYSOFA_GEN (config.h, mysofa_export.h) plus a force-included endian shim
# (WASM is little-endian; libmysofa's portable_endian.h has no wasm branch).
#
# Usage:  ./build-wasm.sh /path/to/libmysofa   (default: ~/libmysofa)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DSP="$SCRIPT_DIR/dsp"
MYSOFA_ROOT="${1:-$HOME/libmysofa}"
MS="$MYSOFA_ROOT/src"
GEN="$SCRIPT_DIR/dsp/mysofa-gen"   # generated headers + shim live here (tracked)

if ! command -v emcc >/dev/null 2>&1; then
    echo "emcc not found — run: source \$EMSDK/emsdk_env.sh" >&2
    exit 1
fi
if [ ! -d "$MS/hrtf" ]; then
    echo "libmysofa source not found at $MYSOFA_ROOT" >&2
    echo "clone it: git clone --depth 1 https://github.com/hoene/libmysofa \"$MYSOFA_ROOT\"" >&2
    exit 1
fi

MYSOFA_SRCS=(
    "$MS"/hrtf/cache.c "$MS"/hrtf/check.c "$MS"/hrtf/easy.c "$MS"/hrtf/interpolate.c
    "$MS"/hrtf/kdtree.c "$MS"/hrtf/lookup.c "$MS"/hrtf/loudness.c "$MS"/hrtf/minphase.c
    "$MS"/hrtf/neighbors.c "$MS"/hrtf/reader.c "$MS"/hrtf/resample.c "$MS"/hrtf/spherical.c
    "$MS"/hrtf/tools.c
    "$MS"/hdf/btree.c "$MS"/hdf/dataobject.c "$MS"/hdf/fractalhead.c "$MS"/hdf/gcol.c
    "$MS"/hdf/gunzip.c "$MS"/hdf/superblock.c
    "$MS"/resampler/speex_resampler.c
)

DSP_SRCS=(
    "$DSP"/src/hrtf_engine.c "$DSP"/src/hrtf_convolver.c "$DSP"/src/freeverb.c
    "$DSP"/src/er_spatial.c "$DSP"/src/sofa_loader.c "$DSP"/src/channel_layouts.c
    "$DSP"/pffft/pffft.c "$DSP"/pffft/pffft_common.c "$DSP"/pffft/fftpack.c
)

COMMON=(
    -O3 -ffast-math -DHAVE_MYSOFA=1 -DOUTSIDE_SPEEX=1 -DRANDOM_PREFIX=mysofa
    -include "$GEN/wasm_endian_shim.h"
    -I"$DSP/src" -I"$DSP/pffft" -I"$MS/hrtf" -I"$MS/hdf" -I"$MS/resampler" -I"$GEN"
    -sUSE_ZLIB=1
    -sWASM=1 -sMODULARIZE=1 -sEXPORT_NAME=HaloSoundDSP
    '-sEXPORTED_FUNCTIONS=["_halo_create","_halo_destroy","_halo_load_sofa","_halo_set_layout","_halo_set_speaker_pos","_halo_set_room","_halo_set_room_preset","_halo_process","_halo_get_num_channels","_malloc","_free"]'
    '-sEXPORTED_RUNTIME_METHODS=["ccall","cwrap","HEAPF32","HEAPU8"]'
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=134217728
    -sNO_EXIT_RUNTIME=1 '-sENVIRONMENT=web,worker' -sFILESYSTEM=0 -sASSERTIONS=0
)

echo "=== SIMD build ==="
emcc "${COMMON[@]}" -msimd128 -msse -Di386=1 \
    "${DSP_SRCS[@]}" "${MYSOFA_SRCS[@]}" \
    -o "$SCRIPT_DIR/app/wasm/halosound_dsp_simd.js"

echo "=== scalar build (fallback) ==="
emcc "${COMMON[@]}" -DPFFFT_SCALVEC_ENABLED=1 \
    "${DSP_SRCS[@]}" "${MYSOFA_SRCS[@]}" \
    -o "$SCRIPT_DIR/app/wasm/halosound_dsp.js"

echo "=== done ==="
ls -la "$SCRIPT_DIR/app/wasm/"*.wasm
