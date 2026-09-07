/*
 * Spatial extension coefficient export - global instance definition
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "spatial_ext_coeff.h"

/* Force-export these symbols from the avcodec shared library so mpv's af_hrtf
 * can resolve them at runtime.
 *
 * Windows needs dllexport.  ELF needs two separate things and missing either
 * one leaves the symbol invisible to dlsym(): default visibility (FFmpeg
 * compiles with -fvisibility=hidden), and an entry in libavcodec/libavcodec.v,
 * whose "local: *" clause hides everything that isn't av*.  The version script
 * side can't be expressed here — the Linux build adds those three names to it.
 */
#if defined(_WIN32)
#  define SPATIAL_EXPORT __declspec(dllexport)
#elif defined(__ELF__) && (defined(__GNUC__) || defined(__clang__))
#  define SPATIAL_EXPORT __attribute__((visibility("default")))
#else
#  define SPATIAL_EXPORT
#endif

SPATIAL_EXPORT SpatialExtCoeff   g_spatial_ext_coeff   = {0};
SPATIAL_EXPORT SpatialExtObjMeta g_spatial_ext_objmeta = {0};
SPATIAL_EXPORT ObjCodingMixData  g_objcoding_data      = {0};
