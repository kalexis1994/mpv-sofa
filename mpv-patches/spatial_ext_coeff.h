/*
 * Spatial extension coefficient and object position export
 *
 * Exports two data structures from the lossless HD audio decoder:
 * 1. Rematrix coefficients (for position estimation / audio rendering)
 * 2. Object metadata positions (parsed from the extension frame after substreams)
 *
 * Coordinate system:
 *   X: -1.0 = full left,  +1.0 = full right
 *   Y: -1.0 = front,      +1.0 = back
 *   Z: -1.0 = below,      +1.0 = above
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef AVCODEC_SPATIAL_EXT_COEFF_H
#define AVCODEC_SPATIAL_EXT_COEFF_H

#include <stdint.h>
#include <stdatomic.h>

#define SPATIAL_EXT_MAX_MATRICES 15
#define SPATIAL_EXT_MAX_CHANNELS 16
#define SPATIAL_EXT_MAX_OBJECTS  16

typedef struct {
    /* Current interpolated seed coefficients per matrix, 2.18 fixed-point */
    int32_t coeff[SPATIAL_EXT_MAX_MATRICES][SPATIAL_EXT_MAX_CHANNELS];
    uint8_t out_ch[SPATIAL_EXT_MAX_MATRICES];   /* destination channel per matrix */
    int     num_matrices;
    int     max_channel;                        /* max bed channel index */
    uint64_t bed_mask;                          /* bed substream's AVChannel mask */
    int8_t  bed_shift;                          /* bed output_shift (for height scaling) */
    _Atomic int updated;                        /* set by decoder, cleared by reader */
} SpatialExtCoeff;

typedef struct {
    float x, y, z;      /* position in [-1, +1] */
    float size;          /* object size 0..1 (0 = point source) */
    int8_t gain_db;      /* gain in dB, -128 = muted */
    uint8_t active;      /* 1 if object is active */
} SpatialExtObjectPos;

typedef struct {
    SpatialExtObjectPos objects[SPATIAL_EXT_MAX_OBJECTS];
    int     num_objects;          /* total objects (bed-as-objects + dynamic) */
    int     num_bed_objects;      /* how many are bed (fixed speaker positions) */
    int     num_dynamic_objects;  /* how many are dynamic (can move) */
    uint32_t sample_offset;       /* sample offset within access unit */
    uint32_t ramp_duration;       /* interpolation ramp in samples */
    _Atomic int updated;          /* set by decoder, cleared by reader */
} SpatialExtObjMeta;

/* -----------------------------------------------------------------------
 * Object coding (immersive audio) mixing matrix — exported from decoder,
 * consumed by af_hrtf.
 * Per ETSI TS 103 420: reconstructs object audio from 5.1 downmix via QMF
 * ----------------------------------------------------------------------- */

#define OBJCODING_MAX_OBJECTS   16
#define OBJCODING_MAX_CHANNELS   7
#define OBJCODING_MAX_BANDS     23
#define OBJCODING_QMF_SUBBANDS  64
#define OBJCODING_MAX_TIMESLOTS 24  /* 1536 samples / 64 */

typedef struct {
    /* Fully interpolated mixing matrix: [obj][timeslot][channel][subband] */
    float mix_mtx[OBJCODING_MAX_OBJECTS][OBJCODING_MAX_TIMESLOTS][OBJCODING_MAX_CHANNELS][OBJCODING_QMF_SUBBANDS];
    int   num_objects;
    int   num_channels;       /* 5 or 7 (excl. LFE) */
    int   num_timeslots;      /* typically 24 */
    float clipgain;           /* clip gain factor */
    _Atomic int updated;
} ObjCodingMixData;

/* -----------------------------------------------------------------------
 * Pre-rematrix residual audio export — decoded residual channels from
 * TrueHD Atmos substream 3, BEFORE the interpolating rematrix is applied.
 * These contain the "pure" object/height audio without bed contamination
 * or rematrix coefficient discontinuities.
 *
 * Lock-free SPSC ring buffer: decoder writes, HRTF filter reads.
 * Each residual channel corresponds to a thd_channel_order index that
 * is NOT in the bed mask (typically TFL=index 6, TFR=index 7).
 * ----------------------------------------------------------------------- */

#define SPATIAL_RESIDUAL_MAX_CH   4     /* max residual channels (typically 2: TFL/TFR) */
#define SPATIAL_RESIDUAL_BUF_SIZE 2048  /* ring buffer size (power of 2, ~42ms at 48kHz) */
#define SPATIAL_RESIDUAL_BUF_MASK (SPATIAL_RESIDUAL_BUF_SIZE - 1)

typedef struct {
    /* Ring buffer: float samples, interleaved per residual channel */
    float buf[SPATIAL_RESIDUAL_MAX_CH][SPATIAL_RESIDUAL_BUF_SIZE];
    _Atomic uint32_t write_pos;           /* next write position (mod BUF_SIZE) */
    _Atomic uint32_t read_pos;            /* next read position (mod BUF_SIZE) */
    int     num_channels;                 /* number of residual channels (typically 2) */
    int     output_shift;                 /* right-shift applied during int32→float conversion */
    _Atomic int ready;                    /* set to 1 once first write happens */
} SpatialResidualBuf;

/* Plain extern for use within avcodec. External consumers (mpv) should
 * declare with __declspec(dllimport) on Windows. */
extern SpatialExtCoeff    g_spatial_ext_coeff;
extern SpatialExtObjMeta  g_spatial_ext_objmeta;
extern ObjCodingMixData   g_objcoding_data;
extern SpatialResidualBuf g_spatial_residual;

#endif /* AVCODEC_SPATIAL_EXT_COEFF_H */
