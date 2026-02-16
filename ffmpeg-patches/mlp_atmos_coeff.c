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

/* Force-export these symbols from avcodec DLL so mpv can import them */
#ifdef _WIN32
__declspec(dllexport)
#endif
SpatialExtCoeff g_spatial_ext_coeff = {0};

#ifdef _WIN32
__declspec(dllexport)
#endif
SpatialExtObjMeta g_spatial_ext_objmeta = {0};

#ifdef _WIN32
__declspec(dllexport)
#endif
ObjCodingMixData g_objcoding_data = {0};
