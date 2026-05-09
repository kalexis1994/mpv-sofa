/*
 * af_hrtf.c - HRTF binaural spatialization audio filter for mpv
 *
 * Takes multichannel audio (5.1, 7.1, 7.1.4, or spatial objects) and
 * outputs stereo binaural audio using HRTF convolution.
 *
 * Uses libmysofa for SOFA/HRIR loading and PFFFT for fast convolution.
 *
 * This file goes into mpv-src/audio/filter/af_hrtf.c
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static FILE *hrtf_dbg = NULL;
static int hrtf_dbg_count = 0;
/* Debug logging DISABLED for real-time playback to avoid I/O-induced
 * audio glitches.  Set HRTF_ENABLE_DBG=1 to re-enable at compile time. */
#define HRTF_ENABLE_DBG 0
/* NOTE: remaining micro-saturation in HRTF is from overlap-add convolver
 * accumulation across multiple channels at peak moments.  The limiter
 * operates post-sum but internal float overflow can occur during the
 * per-channel convolution+accumulation loop.  Fix: apply per-channel
 * pre-attenuation before convolution based on channel count. */
#define HRTF_DBG(...) do { \
    if (HRTF_ENABLE_DBG) { \
        if (!hrtf_dbg) hrtf_dbg = fopen("hrtf_debug.txt", "w"); \
        if (hrtf_dbg) { fprintf(hrtf_dbg, __VA_ARGS__); fflush(hrtf_dbg); } \
    } \
} while(0)

#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "filters/f_autoconvert.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_option.h"

#include <mysofa.h>
#include "pffft.h"

/* Spatial extension coefficient export from lossless HD decoder (libavcodec).
 * The global lives in avcodec-62.dll; we resolve it at runtime
 * via GetProcAddress to avoid import library issues. */
#include "spatial_ext_coeff.h"
#include "objcoding_qmf.h"
#ifdef _WIN32
#include <windows.h>
#endif
static SpatialExtCoeff *g_spatial_coeff_ptr = NULL;
static SpatialExtObjMeta *g_spatial_objmeta_ptr = NULL;
static ObjCodingMixData *g_objcoding_data_ptr = NULL;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define HRTF_BLOCK_SIZE      256
#define HRTF_MAX_HRIR_LEN    768   // Must accommodate actual HRIR (e.g. 558 samples)
#define HRTF_SS_MAX_CHANNELS 16   // SharedState array size — must match host app
#define HRTF_MAX_CHANNELS    32   // internal max (bed + JOC objects)
#define HRTF_FFT_SIZE        (HRTF_BLOCK_SIZE + HRTF_MAX_HRIR_LEN - 1)
// Round up to next power of 2 for FFT (must be >= HRTF_FFT_SIZE)
#define HRTF_FFT_N           1024

#define HRTF_CROSSFADE_LEN   1024
#define HEIGHT_FADE_LEN      256   // height channel degraded fade (one HRTF block)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* BYPASS MODE: Set to 1 to skip ALL HRTF convolution and output a simple
 * stereo downmix.  If the "object audio bugging out" problem persists with
 * bypass=1, the root cause is upstream (decoder / mpv audio chain / WASAPI),
 * NOT in this filter's DSP. */
#define HRTF_BYPASS_MODE 0

#define AIR_ABS_FACTOR 0.1f

#define ER_NUM_TAPS     6
#define ER_MAX_DELAY    8192   // ~170ms at 48kHz
#define SPEED_OF_SOUND  343.0f

// Ambisonic early-reflections constants.  Mirrors Steam Audio's
// AmbisonicsPanningEffect (core/src/core/ambisonics_panning_effect.cpp) and
// AmbisonicsBinauralEffect precompute (core/src/core/hrtf_database.cpp).
// Order 3 → 16 SH channels, 32 SH-HRIR convolutions per block.  Steam Audio
// runs at order 3 by default; the 24-point Sloane t-design (t=7) has enough
// resolution to faithfully decode up to order 3 via SAD.
#define AMBI_ORDER        3
#define AMBI_NUM_CH       ((AMBI_ORDER + 1) * (AMBI_ORDER + 1))   // 16 for order 3
#define AMBI_NUM_SPEAKERS 24   // Sloane t-design, http://neilsloane.com/sphdesigns/dim3/des.3.24.7.txt

// ---------------------------------------------------------------------------
// Shared state (for communication with host app visualizer)
// ---------------------------------------------------------------------------

// Minimal inline version — the host app can also include SharedState.h
// If building standalone within mpv, we define what we need here.
typedef struct {
    float azimuth;
    float elevation;
    float distance;
} HrtfSpeakerPos;

// ---------------------------------------------------------------------------
// Shared state (must match host app's SharedState.h layout exactly)
// ---------------------------------------------------------------------------

#define HRTF_SS_MAX_OBJECTS 128

typedef struct {
    // Written by audio filter, read by UI
    _Atomic int32_t  num_channels;
    _Atomic int32_t  num_bed_channels;
    _Atomic float    channel_rms[HRTF_SS_MAX_CHANNELS];
    _Atomic float    channel_peak[HRTF_SS_MAX_CHANNELS];
    _Atomic int64_t  frame_counter;
    _Atomic double   current_pts;
    _Atomic int32_t  sample_rate;
    _Atomic int32_t  active;

    _Atomic int32_t  num_objects;
    _Atomic float    object_x[HRTF_SS_MAX_OBJECTS];
    _Atomic float    object_y[HRTF_SS_MAX_OBJECTS];
    _Atomic float    object_z[HRTF_SS_MAX_OBJECTS];
    _Atomic float    object_gain[HRTF_SS_MAX_OBJECTS];
    _Atomic int32_t  object_active[HRTF_SS_MAX_OBJECTS];
    _Atomic int32_t  objects_changed;

    // Written by UI, read by audio filter
    HrtfSpeakerPos   speaker_pos[HRTF_SS_MAX_CHANNELS];
    _Atomic int32_t  speaker_pos_changed;

    char             sofa_path[512];
    _Atomic int32_t  sofa_path_changed;

    _Atomic float    master_volume;
    _Atomic int32_t  hrtf_enabled;

    _Atomic int32_t  reverb_enabled;
    _Atomic float    reverb_decay;
    _Atomic float    reverb_wet;
    _Atomic float    reverb_damping;
    _Atomic float    reverb_predelay;
    _Atomic int32_t  reverb_changed;

    _Atomic float    room_width;
    _Atomic float    room_depth;
    _Atomic float    room_height;
    _Atomic float    room_absorption;
    _Atomic int32_t  room_changed;
    _Atomic float    room_gain;           // per-room volume compensation (1.0 = unity)

    _Atomic int32_t  test_tone_channel;
    _Atomic int32_t  test_tone_active;

    _Atomic int32_t  mute_bed;
    _Atomic int32_t  mute_objects;

    _Atomic float    er_level;           // 0..1  wet level for ambisonic early reflections
    _Atomic float    crossfeed;          // -0.3..0.3 binaural crossfeed amount
    _Atomic int32_t  channel_order_smpte; // 1 = swap SL/BL and SR/BR to fix Atmos content
    _Atomic int32_t  screen_baffling;     // 1 = HF shelf on FL/FR/FC (behind screen)
    _Atomic int32_t  front_pinna_boost;   // 1 = synthetic frontal pinna EQ on FL/FR/FC
    _Atomic float    bauer_crossfeed;     // 0..0.5 LF-only crossfeed for frontal grounding

    char             ir_file_path[512];   // path to convolution reverb IR (WAV)
    _Atomic int32_t  ir_changed;
    _Atomic float    ir_wet;              // 0..1

    _Atomic int32_t  near_field_comp;     // 1 = LF shelf boost for sources <1.5m
    _Atomic int32_t  direct_min_phase;    // 1 = min-phase preprocessing on direct HRIRs

    // Headphone EQ correction (AutoEQ ParametricEq.txt)
    char             hp_eq_path[512];
    _Atomic int32_t  hp_eq_changed;
    _Atomic int32_t  hp_eq_enabled;
} HrtfSharedState;

// ---------------------------------------------------------------------------
// Per-channel HRTF convolver + Schroeder reverb
// ---------------------------------------------------------------------------

#include "af_hrtf_convolver.h"
#include "af_hrtf_reverb.h"

// ---------------------------------------------------------------------------
// Early reflections (image-source method, 6 first-order taps)
// ---------------------------------------------------------------------------

// Early-reflections using an ambisonic bus (Steam Audio approach).
// Each tap encodes a delayed sample of a shared mono room-send into B-format
// with its real-SH basis evaluated at the image-source direction; the bus is
// decoded to binaural at the end of the block via precomputed SH-HRIR filters.
typedef struct {
    // Mono delay line (shared by all taps).
    float *delay_buf;
    int    delay_size;
    int    write_pos;

    // Per-tap definitions.
    int    num_taps;
    int    tap_delays[ER_NUM_TAPS];                         // delay in samples
    float  tap_gain[ER_NUM_TAPS];                            // absorption / distance
    float  tap_sh[ER_NUM_TAPS][AMBI_NUM_CH];                 // precomputed SH basis

    // B-format bus — one block-sized buffer per ambisonic channel (W,Y,Z,X for order 1).
    float *bus[AMBI_NUM_CH];

    // Ambisonic→binaural decoder: one HRIR-sized convolver per (SH channel, ear).
    // Filters are filled by compute_ambi_decoder_hrirs() from min-phase HRIRs
    // projected onto the 24-point Sloane t-design.
    HrtfConvolver dec_l[AMBI_NUM_CH];
    HrtfConvolver dec_r[AMBI_NUM_CH];

    int decoder_valid;   // 1 after compute_ambi_decoder_hrirs succeeds
    int initialized;
} EarlyReflections;

// Long-IR partitioned convolver and headphone-EQ biquad cascade.
#include "af_hrtf_pconv.h"
#include "af_hrtf_hp_eq.h"

// ---------------------------------------------------------------------------
// Filter options
// ---------------------------------------------------------------------------

struct hrtf_opts {
    char *sofa_path;
    int64_t shared_state_ptr;  // pointer to HrtfSharedState, cast to int64
};

// ---------------------------------------------------------------------------
// Filter private state
// ---------------------------------------------------------------------------

struct priv {
    struct hrtf_opts *opts;
    struct mp_pin *in_pin;
    struct mp_aframe_pool *out_pool;

    // libmysofa
    struct MYSOFA_EASY *sofa;
    int hrir_length;
    float *dfc_filter;   // diffuse-field compensation (freq domain, PFFFT format)
    int    dfc_valid;     // 1 if DFC filter computed

    // Per-channel convolvers (double-buffered)
    HrtfChannelPair channels[HRTF_MAX_CHANNELS];
    int num_channels;

    // Current speaker positions (from options or shared state)
    HrtfSpeakerPos speaker_pos[HRTF_MAX_CHANNELS];

    // Input accumulation buffer (per channel)
    float *input_accum[HRTF_MAX_CHANNELS];
    int input_accum_pos;

    // Output accumulation buffer (stereo)
    float *output_l;
    float *output_r;
    int output_avail;      // samples available in output buffers
    int output_read_pos;   // next sample to read from output buffers

    int sample_rate;
    int initialized;

    // Frame-boundary smoothing state for lossless HD spatial click suppression
    float prev_frame_tail[HRTF_MAX_CHANNELS];
    int   prev_frame_tail_valid;

    // Reverb state
    ReverbState reverb;

    // IR convolution reverb (stereo).  One PartitionedConvolver per ear.
    PartitionedConvolver ir_l;
    PartitionedConvolver ir_r;
    char ir_loaded_path[512];  // path currently in the convolvers (empty if none)

    // Headphone EQ correction (parametric biquad cascade)
    HpEqProfile hp_eq;
    char hp_eq_loaded_path[512];

    // Early reflections state
    EarlyReflections er;

    // Shared state for host app communication
    HrtfSharedState *shared;

    // Test tone state
    HrtfChannelPair test_tone_pair;
    float test_tone_phase;
    int test_tone_remaining;    // samples left to generate
    int test_tone_ch;           // channel index for spatialization
    int test_tone_inited;       // convolvers allocated?
    int test_tone_generated;    // total sine samples generated (for envelope)
    float test_tone_out_l[HRTF_BLOCK_SIZE];  // convolved residual L
    float test_tone_out_r[HRTF_BLOCK_SIZE];  // convolved residual R
    int test_tone_out_avail;    // residual samples available
    int test_tone_out_read;     // residual read position

    // Distance attenuation / air absorption
    float air_abs_state[HRTF_MAX_CHANNELS];  // one-pole lowpass state
    float screen_baffle_state[3];             // screen-baffle LP state for FL/FR/FC
    // Near-field compensation: per-ear, per-channel one-pole LP states for
    // the low-shelf boost applied to sources within ~1.5 m.
    float nf_lp_state_l[HRTF_MAX_CHANNELS];
    float nf_lp_state_r[HRTF_MAX_CHANNELS];
    // Frontal pinna EQ: peak ~4 kHz, notch ~8 kHz (two cascaded biquads).
    // Per-channel x[n-1], x[n-2], y[n-1], y[n-2] state for each biquad.
    float pinna_peak_state[3][4];             // [ch][x1,x2,y1,y2]
    float pinna_notch_state[3][4];
    // Bauer crossfeed: short stereo delay line + per-ear one-pole LP.
    // Delay ~14 samples at 48 kHz (~290 µs head width), LP fc ~700 Hz.
#define BAUER_DELAY_LEN 32
    float bauer_buf_l[BAUER_DELAY_LEN];
    float bauer_buf_r[BAUER_DELAY_LEN];
    int   bauer_pos;
    float bauer_lp_l;
    float bauer_lp_r;
    float min_dist;                           // minimum speaker distance
    float out_limiter_gain;                   // smoothed output limiter gain

    // Height channel continuity filter: smooths AU-boundary discontinuities
    // in TrueHD rematrix output (~1200 clicks/sec that sound as constant buzz).
    // Per-channel one-pole state tracks the signal; when a large jump occurs
    // at an AU boundary, the filter briefly engages to smooth the transition.
    float height_cont_state[HRTF_MAX_CHANNELS]; // continuity tracker (previous sample)
    int   height_cont_valid;                     // 1 after first sample processed

    // spatial object rendering
    float object_az[HRTF_MAX_CHANNELS];      // cached azimuth per object channel
    float object_el[HRTF_MAX_CHANNELS];      // cached elevation per object channel
    float object_dist[HRTF_MAX_CHANNELS];    // cached distance per object channel
    int num_bed_channels;                      // channels 0..N-1 are bed, rest are objects
    uint64_t last_bed_mask;                    // last applied bed mask (0 = not yet set)

    // Height channel dynamic positioning state
    int   height_ch_idx[HRTF_MAX_CHANNELS]; // channel indices that are height speakers
    int   num_height_channels;               // count of height channels
    float height_smooth_az[HRTF_MAX_CHANNELS]; // smoothed azimuth per height ch
    float height_smooth_el[HRTF_MAX_CHANNELS]; // smoothed elevation per height ch
    float height_smooth_dist[HRTF_MAX_CHANNELS]; // smoothed distance per height ch
    int   height_smooth_valid;                // 1 if smooth state initialized

    // Height channel jump detection diagnostic
    float height_prev_sample[HRTF_MAX_CHANNELS]; // last sample per height ch
    int   height_prev_valid;
    int   height_jump_count;      // total jumps detected
    int   height_block_count;     // total blocks processed

    // Bed subtraction: rematrix row indices for height output channels
    int   height_mat_idx[HRTF_MAX_CHANNELS]; // rematrix matrix index per height ch (-1=none)
    int   height_mat_valid;                   // 1 if rematrix mapping resolved

    // object coding (DD+ spatial) reconstruction state
    int objcoding_active;                           // 1 if object coding data available and reconstruction enabled
    ObjCodingQmfAnalysis  objcoding_ana[6];               // QMF analysis for 5.1 input (L,R,C,LFE,Ls,Rs)
    ObjCodingQmfSynthesis objcoding_syn[OBJCODING_MAX_OBJECTS]; // QMF synthesis per object
    float *objcoding_obj_buf[OBJCODING_MAX_OBJECTS];      // object PCM output buffers (HRTF_BLOCK_SIZE each)
    int objcoding_num_objects;                       // number of active object coding objects
    int objcoding_num_channels;                      // object coding input channels (5 or 7, excl LFE)
    int objcoding_ts_offset;                         // timeslot offset within current DD+ frame
    float objcoding_norm_scale;                      // smoothed energy normalization scale factor

    // JOC object HRTF positioning state
    float joc_smooth_az[OBJCODING_MAX_OBJECTS];      // smoothed azimuth per JOC object
    float joc_smooth_el[OBJCODING_MAX_OBJECTS];      // smoothed elevation per JOC object
    float joc_smooth_dist[OBJCODING_MAX_OBJECTS];    // smoothed distance per JOC object
    int   joc_smooth_valid;                          // 1 after first position update

    // OAMD ramp duration for adaptive crossfade/smoothing
    int   oamd_ramp_duration;    // last ramp_duration from OAMD (samples), 0=unknown

    // Height channel degraded mode fade state
    int   height_was_active;     // height channels had signal in previous update
    int   height_fade_remaining; // >0 = fade-out samples left, <0 = fade-in samples left

};


// ---------------------------------------------------------------------------
// Minimal WAV reader for loading impulse responses.
// Supports PCM 16/24/32-bit and IEEE-754 float 32-bit, mono or stereo.
// Returns 0 on success; the caller frees *out_l and *out_r.  Mono files are
// duplicated onto both channels.  If target_sr > 0 and the file's sample rate
// doesn't match, returns -2 (we don't resample; user should provide 48 kHz).
// ---------------------------------------------------------------------------
static uint32_t wav_rd_u32_le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint16_t wav_rd_u16_le(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static int wav_load_stereo(const char *path, int target_sr,
                           float **out_l, float **out_r, int *out_len) {
    *out_l = NULL; *out_r = NULL; *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f); return -1;
    }

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sr = 0;
    uint32_t data_size = 0;
    long data_offset = 0;

    for (;;) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, f) != 8) break;
        uint32_t csize = wav_rd_u32_le(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[40] = {0};
            uint32_t r = csize > sizeof(fmt) ? sizeof(fmt) : csize;
            if (fread(fmt, 1, r, f) != r) { fclose(f); return -1; }
            format   = wav_rd_u16_le(fmt + 0);
            channels = wav_rd_u16_le(fmt + 2);
            sr       = wav_rd_u32_le(fmt + 4);
            bits     = wav_rd_u16_le(fmt + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real format tag is in the GUID tail.
            if (format == 0xFFFE && csize >= 40) {
                format = wav_rd_u16_le(fmt + 24);
            }
            if (csize > r) fseek(f, csize - r, SEEK_CUR);
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_size = csize;
            data_offset = ftell(f);
            break;
        } else {
            fseek(f, csize, SEEK_CUR);
        }
        if (csize & 1) fseek(f, 1, SEEK_CUR);  // pad byte
    }

    if (data_size == 0 || channels < 1 || channels > 16 ||
        (format != 1 && format != 3) || bits == 0) {
        fclose(f); return -1;
    }
    if (target_sr > 0 && (int)sr != target_sr) {
        fclose(f); return -2;
    }

    int bps = bits / 8;
    int frame = bps * channels;
    int N = (int)(data_size / (uint32_t)frame);
    float *L = (float *)malloc((size_t)N * sizeof(float));
    float *R = (float *)malloc((size_t)N * sizeof(float));
    if (!L || !R) { free(L); free(R); fclose(f); return -1; }

    // Scratch buffer for one frame of per-channel floats; enables a simple
    // downmix from arbitrary channel counts (mono, stereo, or 4-channel
    // B-format ambisonic) to our stereo consumer.  For B-format we assume
    // FuMa order (W, X, Y, Z) — the Theatre@41 dataset and most Soundfield
    // ST450 captures follow this convention — and decode with a virtual
    // stereo mic pair:
    //     L = W + 0.5·Y
    //     R = W − 0.5·Y
    // This preserves the lateral imaging baked into the recording while
    // keeping the omnidirectional energy that carries the reverb tail.
    float ch_buf[16];

    fseek(f, data_offset, SEEK_SET);
    for (int i = 0; i < N; i++) {
        uint8_t buf[64];
        if (frame > (int)sizeof(buf)) { free(L); free(R); fclose(f); return -1; }
        if (fread(buf, 1, frame, f) != (size_t)frame) { N = i; break; }
        for (int c = 0; c < channels; c++) {
            const uint8_t *p = buf + c * bps;
            float v = 0.0f;
            if (format == 1) {
                if (bits == 16) {
                    int16_t s = (int16_t)(p[0] | (p[1] << 8));
                    v = (float)s / 32768.0f;
                } else if (bits == 24) {
                    int32_t s = (int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24));
                    s >>= 8;
                    v = (float)s / 8388608.0f;
                } else if (bits == 32) {
                    int32_t s = (int32_t)wav_rd_u32_le(p);
                    v = (float)s / 2147483648.0f;
                } else {
                    free(L); free(R); fclose(f); return -1;
                }
            } else if (format == 3 && bits == 32) {
                memcpy(&v, p, 4);
            } else {
                free(L); free(R); fclose(f); return -1;
            }
            ch_buf[c] = v;
        }

        float l = 0.0f, r = 0.0f;
        if (channels == 1) {
            l = r = ch_buf[0];
        } else if (channels == 2) {
            l = ch_buf[0]; r = ch_buf[1];
        } else if (channels == 4) {
            // FuMa B-format: W, X, Y, Z
            float W = ch_buf[0];
            float Y = ch_buf[2];
            l = W + 0.5f * Y;
            r = W - 0.5f * Y;
        } else {
            // Unknown multichannel — naive L/R downmix from first two only.
            l = ch_buf[0]; r = ch_buf[1];
        }
        L[i] = l;
        R[i] = r;
    }
    fclose(f);

    // Normalise so the peak is ~0.9 — B-format decoding via virtual mic pair
    // can leave headroom unused and wouldn't convolve to a useful level.
    float peak = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = fabsf(L[i]); if (a > peak) peak = a;
        float b = fabsf(R[i]); if (b > peak) peak = b;
    }
    if (peak > 1e-9f && peak < 0.9f) {
        float g = 0.9f / peak;
        for (int i = 0; i < N; i++) { L[i] *= g; R[i] *= g; }
    }

    *out_l = L; *out_r = R; *out_len = N;
    return 0;
}

// Load any WAV into up to 4 separate channel buffers (FuMa order if 4ch).
// Returns 0 on success.  Caller frees out[i] for each i in [0, *out_channels).
// Used for 4-channel B-format IRs that we want to decode via SH-HRIR basis
// instead of folding into stereo at load time.
static int wav_load_n(const char *path, int target_sr,
                      float *out[4], int *out_channels, int *out_len) {
    for (int i = 0; i < 4; i++) out[i] = NULL;
    *out_channels = 0; *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f); return -1;
    }

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sr = 0;
    uint32_t data_size = 0;
    long data_offset = 0;

    for (;;) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, f) != 8) break;
        uint32_t csize = wav_rd_u32_le(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[40] = {0};
            uint32_t r = csize > sizeof(fmt) ? sizeof(fmt) : csize;
            if (fread(fmt, 1, r, f) != r) { fclose(f); return -1; }
            format   = wav_rd_u16_le(fmt + 0);
            channels = wav_rd_u16_le(fmt + 2);
            sr       = wav_rd_u32_le(fmt + 4);
            bits     = wav_rd_u16_le(fmt + 14);
            if (format == 0xFFFE && csize >= 40)
                format = wav_rd_u16_le(fmt + 24);
            if (csize > r) fseek(f, csize - r, SEEK_CUR);
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_size = csize;
            data_offset = ftell(f);
            break;
        } else {
            fseek(f, csize, SEEK_CUR);
        }
        if (csize & 1) fseek(f, 1, SEEK_CUR);
    }

    if (data_size == 0 || channels < 1 || channels > 4 ||
        (format != 1 && format != 3) || bits == 0) {
        fclose(f); return -1;
    }
    if (target_sr > 0 && (int)sr != target_sr) {
        fclose(f); return -2;
    }

    int bps = bits / 8;
    int frame = bps * channels;
    int N = (int)(data_size / (uint32_t)frame);
    for (int c = 0; c < channels; c++)
        out[c] = (float *)malloc((size_t)N * sizeof(float));

    fseek(f, data_offset, SEEK_SET);
    uint8_t buf[64];
    for (int i = 0; i < N; i++) {
        if (frame > (int)sizeof(buf)) {
            for (int c = 0; c < channels; c++) { free(out[c]); out[c] = NULL; }
            fclose(f); return -1;
        }
        if (fread(buf, 1, frame, f) != (size_t)frame) { N = i; break; }
        for (int c = 0; c < channels; c++) {
            const uint8_t *p = buf + c * bps;
            float v = 0.0f;
            if (format == 1) {
                if (bits == 16) {
                    int16_t s = (int16_t)(p[0] | (p[1] << 8));
                    v = (float)s / 32768.0f;
                } else if (bits == 24) {
                    int32_t s = (int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24));
                    s >>= 8;
                    v = (float)s / 8388608.0f;
                } else if (bits == 32) {
                    int32_t s = (int32_t)wav_rd_u32_le(p);
                    v = (float)s / 2147483648.0f;
                } else {
                    for (int cc = 0; cc < channels; cc++) { free(out[cc]); out[cc] = NULL; }
                    fclose(f); return -1;
                }
            } else if (format == 3 && bits == 32) {
                memcpy(&v, p, 4);
            } else {
                for (int cc = 0; cc < channels; cc++) { free(out[cc]); out[cc] = NULL; }
                fclose(f); return -1;
            }
            out[c][i] = v;
        }
    }
    fclose(f);

    *out_channels = channels;
    *out_len = N;
    return 0;
}

// Direct (offline) time-domain convolution: out[n] = sum_k in[k] * kern[n-k].
// out must be sized in_len + kern_len - 1.  Used at IR-load time to bake the
// SH-HRIR decoder into a stereo binaural reverb IR; runtime cost is unchanged.
static void offline_convolve(const float *in, int in_len,
                             const float *kern, int kern_len,
                             float scale, float *out_accum) {
    if (scale == 0.0f) return;
    for (int k = 0; k < in_len; k++) {
        float v = in[k] * scale;
        if (v == 0.0f) continue;
        const float *ker = kern;
        float *dst = out_accum + k;
        for (int j = 0; j < kern_len; j++)
            dst[j] += v * ker[j];
    }
}

// ---------------------------------------------------------------------------
// Real spherical harmonics (orders 0–3, ACN ordering, full N3D normalisation).
//
// Coefficients match the Google spherical-harmonics library that Steam Audio
// wraps in core/src/core/sh/spherical_harmonics.cc.  Google's convention is
// +x forward, +y left, +z up — which is ALSO our audio convention (the same
// one libmysofa uses), so no coordinate transform is needed here.  We feed
// the SH basis directly with (x_audio, y_audio, z_audio).
//
// 16 ACN indices for order 3: i = l*(l+1) + m
//   0:(0,0)=W   1:(1,-1)=Y   2:(1,0)=Z   3:(1,1)=X
//   4:(2,-2)    5:(2,-1)     6:(2,0)     7:(2,1)     8:(2,2)
//   9:(3,-3)   10:(3,-2)    11:(3,-1)   12:(3,0)    13:(3,1)   14:(3,2)   15:(3,3)
// ---------------------------------------------------------------------------

static void sh_eval(float x, float y, float z, float out_sh[AMBI_NUM_CH]) {
    float xx = x * x, yy = y * y, zz = z * z;

    // Order 0
    out_sh[0]  = 0.282094791773878f;                       // 0.5 * sqrt(1/pi)

    // Order 1
    out_sh[1]  = 0.488602511902920f * y;                   // sqrt(3/(4pi))·y
    out_sh[2]  = 0.488602511902920f * z;                   // sqrt(3/(4pi))·z
    out_sh[3]  = 0.488602511902920f * x;                   // sqrt(3/(4pi))·x

    // Order 2
    out_sh[4]  = 1.092548430592079f * x * y;               // 0.5·sqrt(15/pi)·xy
    out_sh[5]  = 1.092548430592079f * y * z;               // 0.5·sqrt(15/pi)·yz
    out_sh[6]  = 0.315391565252520f * (-xx - yy + 2.0f * zz);
                                                            // 0.25·sqrt(5/pi)·(-x²-y²+2z²)
    out_sh[7]  = 1.092548430592079f * x * z;               // 0.5·sqrt(15/pi)·xz
    out_sh[8]  = 0.546274215296040f * (xx - yy);           // 0.25·sqrt(15/pi)·(x²-y²)

    // Order 3
    out_sh[9]  = 0.590043589926644f * y * (3.0f * xx - yy);
                                                            // 0.25·sqrt(35/(2pi))·y(3x²-y²)
    out_sh[10] = 2.890611442640554f * x * y * z;           // 0.5·sqrt(105/pi)·xyz
    out_sh[11] = 0.457045799464466f * y * (4.0f * zz - xx - yy);
                                                            // 0.25·sqrt(21/(2pi))·y(4z²-x²-y²)
    out_sh[12] = 0.373176332590115f * z * (2.0f * zz - 3.0f * xx - 3.0f * yy);
                                                            // 0.25·sqrt(7/pi)·z(2z²-3x²-3y²)
    out_sh[13] = 0.457045799464466f * x * (4.0f * zz - xx - yy);
                                                            // 0.25·sqrt(21/(2pi))·x(4z²-x²-y²)
    out_sh[14] = 1.445305721320277f * z * (xx - yy);       // 0.25·sqrt(105/pi)·z(x²-y²)
    out_sh[15] = 0.590043589926644f * x * (xx - 3.0f * yy);
                                                            // 0.25·sqrt(35/(2pi))·x(x²-3y²)
}

// 24-point spherical t-design (t=7), coordinates lifted verbatim from Steam
// Audio's AmbisonicsPanningEffect::kVirtualSpeakers.  Steam Audio stores
// these in its own internal world frame: +x=right, +y=up, +z=back
// (see core/src/core/sh.cpp:55-57, the Steam→Google converter is
// (-z, -x, y)).  We must convert each entry to our audio frame before use.
static const float kVirtualSpeakers[AMBI_NUM_SPEAKERS][3] = {
    { .8662468181078206f,  .4225186537611115f,  .2666354015167047f},
    { .8662468181078206f, -.4225186537611115f, -.2666354015167047f},
    { .8662468181078206f,  .2666354015167047f, -.4225186537611115f},
    { .8662468181078206f, -.2666354015167047f,  .4225186537611115f},
    {-.8662468181078206f,  .4225186537611115f, -.2666354015167047f},
    {-.8662468181078206f, -.4225186537611115f,  .2666354015167047f},
    {-.8662468181078206f,  .2666354015167047f,  .4225186537611115f},
    {-.8662468181078206f, -.2666354015167047f, -.4225186537611115f},
    { .2666354015167047f,  .8662468181078206f,  .4225186537611115f},
    {-.2666354015167047f,  .8662468181078206f, -.4225186537611115f},
    {-.4225186537611115f,  .8662468181078206f,  .2666354015167047f},
    { .4225186537611115f,  .8662468181078206f, -.2666354015167047f},
    {-.2666354015167047f, -.8662468181078206f,  .4225186537611115f},
    { .2666354015167047f, -.8662468181078206f, -.4225186537611115f},
    { .4225186537611115f, -.8662468181078206f,  .2666354015167047f},
    {-.4225186537611115f, -.8662468181078206f, -.2666354015167047f},
    { .4225186537611115f,  .2666354015167047f,  .8662468181078206f},
    {-.4225186537611115f, -.2666354015167047f,  .8662468181078206f},
    { .2666354015167047f, -.4225186537611115f,  .8662468181078206f},
    {-.2666354015167047f,  .4225186537611115f,  .8662468181078206f},
    { .4225186537611115f, -.2666354015167047f, -.8662468181078206f},
    {-.4225186537611115f,  .2666354015167047f, -.8662468181078206f},
    { .2666354015167047f,  .4225186537611115f, -.8662468181078206f},
    {-.2666354015167047f, -.4225186537611115f, -.8662468181078206f},
};

// Convert a Steam Audio world-space direction (+x=right, +y=up, +z=back) to
// our audio frame (+x=front, +y=left, +z=up) and return (azimuth_deg,
// elevation_deg) suitable for get_hrir_for_position.
//   x_audio  (front) = -z_steam   (negate back → front)
//   y_audio  (left)  = -x_steam   (negate right → left)
//   z_audio  (up)    =  y_steam
static void steam_speaker_to_audio_az_el(const float s[3],
                                         float *az_deg, float *el_deg) {
    float xa = -s[2];
    float ya = -s[0];
    float za =  s[1];
    float r = sqrtf(xa*xa + ya*ya + za*za);
    if (r < 1e-9f) r = 1e-9f;
    float az = atan2f(ya, xa);           // audio az: 0 = front (+x), +pi/2 = left (+y)
    float el = asinf(za / r);            // audio el: 0 = ear level, +pi/2 = up (+z)
    *az_deg = az * 180.0f / (float)M_PI;
    *el_deg = el * 180.0f / (float)M_PI;
}

// Same conversion, returning the audio-space unit vector so we can feed it to
// sh_eval() directly (in our frame = Google-SH frame).
static void steam_speaker_to_audio_xyz(const float s[3],
                                       float *xa, float *ya, float *za) {
    *xa = -s[2];
    *ya = -s[0];
    *za =  s[1];
}

// Minimum-phase conversion via the cepstral method, matching Steam Audio's
// HRTFDatabase::convertToMinimumPhase() (core/src/core/hrtf_database.cpp:695).
// Uses PFFFT so the HRIR length must be a supported PFFFT size; we round
// internally to HRTF_FFT_N.  Input: len-sample HRIR.  Output: len-sample
// minimum-phase HRIR.  Both have the same magnitude spectrum.
static void hrir_to_minimum_phase(const float *hrir_in, int len, float *hrir_out) {
    const int N = HRTF_FFT_N;
    const float kMinMagThresh = 1e-5f;

    PFFFT_Setup *fft = pffft_new_setup(N, PFFFT_REAL);
    float *td   = pffft_aligned_malloc(N * sizeof(float));
    float *fd   = pffft_aligned_malloc(N * sizeof(float));
    float *work = pffft_aligned_malloc(N * sizeof(float));

    // 1. FFT of HRIR.
    memset(td, 0, N * sizeof(float));
    memcpy(td, hrir_in, (len < N ? len : N) * sizeof(float));
    pffft_transform_ordered(fft, td, fd, work, PFFFT_FORWARD);

    // 2. Log|H(f)|, thresholded (avoid log(0) near notches).
    // PFFFT ordered real format: [DC, Nyquist, re1, im1, re2, im2, ...].
    float maxmag = 1e-30f;
    float mag0 = fabsf(fd[0]);
    float magN = fabsf(fd[1]);
    if (mag0 > maxmag) maxmag = mag0;
    if (magN > maxmag) maxmag = magN;
    for (int k = 2; k < N; k += 2) {
        float m = sqrtf(fd[k]*fd[k] + fd[k+1]*fd[k+1]);
        if (m > maxmag) maxmag = m;
    }
    float thr = kMinMagThresh * maxmag;

    float *logmag = pffft_aligned_malloc(N * sizeof(float));
    memset(logmag, 0, N * sizeof(float));
    logmag[0] = logf(mag0 > thr ? mag0 : thr);
    logmag[1] = logf(magN > thr ? magN : thr);
    for (int k = 2; k < N; k += 2) {
        float m = sqrtf(fd[k]*fd[k] + fd[k+1]*fd[k+1]);
        if (m < thr) m = thr;
        float lm = logf(m);
        logmag[k]     = lm;
        logmag[k + 1] = 0.0f;           // zero imaginary (we'll Hilbert via fold)
    }

    // 3. IFFT of log|H| → real cepstrum.
    pffft_transform_ordered(fft, logmag, td, work, PFFFT_BACKWARD);
    float inv_n = 1.0f / (float)N;
    for (int i = 0; i < N; i++) td[i] *= inv_n;

    // 4. Fold causal part (minimum-phase cepstrum): keep c[0], double c[1..N/2-1],
    //    zero the anti-causal half.  This is the Hilbert-transform-in-cepstrum step.
    //    Matches Steam Audio's fold with numSamples even.
    float *cepstrum = pffft_aligned_malloc(N * sizeof(float));
    cepstrum[0] = td[0];
    cepstrum[N / 2] = td[N / 2];
    for (int k = 1; k < N / 2; k++) {
        cepstrum[k]           = td[k] + td[N - k];   // causal half doubled
        cepstrum[N - k]       = 0.0f;                // anti-causal zeroed
    }
    // Note: the above assumes even N (HRTF_FFT_N=1024 is even).

    // 5. FFT of folded cepstrum.
    pffft_transform_ordered(fft, cepstrum, fd, work, PFFFT_FORWARD);

    // 6. exp() — we need complex exp of the folded-cepstrum spectrum.
    //    fd is real-input FFT output: [DC(real), Nyq(real), re, im, re, im, ...].
    //    For DC and Nyquist (pure-real bins) exp is just expf().  For other bins
    //    (a + j b) → exp(a)·(cos b + j sin b).
    float *expfd = pffft_aligned_malloc(N * sizeof(float));
    expfd[0] = expf(fd[0]);
    expfd[1] = expf(fd[1]);
    for (int k = 2; k < N; k += 2) {
        float a = fd[k];
        float b = fd[k + 1];
        float ea = expf(a);
        expfd[k]     = ea * cosf(b);
        expfd[k + 1] = ea * sinf(b);
    }

    // 7. IFFT → minimum-phase time-domain HRIR.
    pffft_transform_ordered(fft, expfd, td, work, PFFFT_BACKWARD);
    for (int i = 0; i < len; i++) hrir_out[i] = td[i] * inv_n;

    pffft_aligned_free(logmag);
    pffft_aligned_free(cepstrum);
    pffft_aligned_free(expfd);
    pffft_aligned_free(td);
    pffft_aligned_free(fd);
    pffft_aligned_free(work);
    pffft_destroy_setup(fft);
}

// Forward decl: needs sofa+dfc to already be ready.
static void get_hrir_for_position(struct priv *p, float azimuth, float elevation,
                                  float distance, float *hrir_l, float *hrir_r);

// Precompute SH-HRIR basis filters for the ambisonic→binaural decoder.
// Matches HRTFDatabase::precomputeAmbisonicsHRTFs (core/src/core/hrtf_database.cpp:479):
// for each SH channel i = (l, m) in ACN order and each ear, accumulate over the
// 24-point Sloane t-design:
//     basis_i[ear] += (4π/N) · SH_i(speaker_dir) · HRIR_minphase(speaker_dir)[ear]
// Then load the resulting time-domain impulse responses into the decoder
// convolvers (conv_set_hrir handles zero-pad + FFT to PFFFT Z-format).
static void compute_ambi_decoder_hrirs(struct priv *p) {
    EarlyReflections *er = &p->er;
    if (!er->initialized || !p->sofa || p->hrir_length <= 0) return;

    const int len = p->hrir_length;
    const float w = (4.0f * (float)M_PI) / (float)AMBI_NUM_SPEAKERS;

    float *basis_l[AMBI_NUM_CH];
    float *basis_r[AMBI_NUM_CH];
    for (int i = 0; i < AMBI_NUM_CH; i++) {
        basis_l[i] = calloc(len, sizeof(float));
        basis_r[i] = calloc(len, sizeof(float));
    }

    float *ir_l   = calloc(len, sizeof(float));
    float *ir_r   = calloc(len, sizeof(float));
    float *mph_l  = calloc(len, sizeof(float));
    float *mph_r  = calloc(len, sizeof(float));

    for (int s = 0; s < AMBI_NUM_SPEAKERS; s++) {
        const float *sv = kVirtualSpeakers[s];

        // Sample HRIRs at this direction using the existing path (DFC + ITD).
        float az_deg, el_deg;
        steam_speaker_to_audio_az_el(sv, &az_deg, &el_deg);
        get_hrir_for_position(p, az_deg, el_deg, 1.0f, ir_l, ir_r);

        // Remove ITD/excess phase before SH projection (Steam Audio step).
        hrir_to_minimum_phase(ir_l, len, mph_l);
        hrir_to_minimum_phase(ir_r, len, mph_r);

        // SH basis at this virtual speaker direction (ACN order, N3D norm).
        // kVirtualSpeakers are in Steam Audio world space; convert to audio
        // (== Google) frame before evaluating the basis.
        float xa, ya, za;
        steam_speaker_to_audio_xyz(sv, &xa, &ya, &za);
        float sh[AMBI_NUM_CH];
        sh_eval(xa, ya, za, sh);

        for (int i = 0; i < AMBI_NUM_CH; i++) {
            float weight = w * sh[i];
            for (int k = 0; k < len; k++) {
                basis_l[i][k] += weight * mph_l[k];
                basis_r[i][k] += weight * mph_r[k];
            }
        }
    }

    // Push each SH-HRIR basis into its convolver.
    for (int i = 0; i < AMBI_NUM_CH; i++) {
        convolver_set_hrir(&er->dec_l[i], basis_l[i], len);
        convolver_set_hrir(&er->dec_r[i], basis_r[i], len);
        er->dec_l[i].valid = 1;
        er->dec_r[i].valid = 1;
    }
    er->decoder_valid = 1;

    for (int i = 0; i < AMBI_NUM_CH; i++) {
        free(basis_l[i]);
        free(basis_r[i]);
    }
    free(ir_l);
    free(ir_r);
    free(mph_l);
    free(mph_r);

    HRTF_DBG("SH-HRIR decoder: %d virtual speakers, order=%d, channels=%d\n",
             AMBI_NUM_SPEAKERS, AMBI_ORDER, AMBI_NUM_CH);
}

// ---------------------------------------------------------------------------
// Early reflections — ambisonic bus (encoder + binaural decoder)
// ---------------------------------------------------------------------------

static void er_init(EarlyReflections *er) {
    memset(er, 0, sizeof(*er));
    er->delay_size = ER_MAX_DELAY;
    er->delay_buf = calloc(ER_MAX_DELAY, sizeof(float));
    for (int i = 0; i < AMBI_NUM_CH; i++) {
        er->bus[i] = calloc(HRTF_BLOCK_SIZE, sizeof(float));
        convolver_init(&er->dec_l[i]);
        convolver_init(&er->dec_r[i]);
    }
    er->initialized = 1;
}

static void er_update(EarlyReflections *er, float width, float depth,
                      float height, float absorption, int sample_rate) {
    if (!er->initialized) return;

    // Listener: centered width, 2/3 depth, ear height 1.2 m.
    float ear_height = 1.2f;
    float listener_depth_frac = 2.0f / 3.0f;

    // 6 first-order image sources, in audio coords:
    //   +x = front, +y = left, +z = up.
    // Direction = unit vector from listener to image source.
    // Extra path length = 2 × perpendicular distance from listener to wall.
    struct { float extra; float x, y, z; } taps[ER_NUM_TAPS] = {
        { width,                                    0.0f,  1.0f,  0.0f}, // left wall
        { width,                                    0.0f, -1.0f,  0.0f}, // right wall
        { 2.0f * ear_height,                        0.0f,  0.0f, -1.0f}, // floor
        { 2.0f * (height - ear_height),             0.0f,  0.0f,  1.0f}, // ceiling
        { 2.0f * (listener_depth_frac * depth),     1.0f,  0.0f,  0.0f}, // front wall
        { 2.0f * ((1.0f - listener_depth_frac) * depth), -1.0f, 0.0f, 0.0f}, // back wall
    };

    er->num_taps = ER_NUM_TAPS;
    for (int i = 0; i < ER_NUM_TAPS; i++) {
        int d = (int)(taps[i].extra / SPEED_OF_SOUND * (float)sample_rate + 0.5f);
        if (d < 1) d = 1;
        if (d >= ER_MAX_DELAY) d = ER_MAX_DELAY - 1;
        er->tap_delays[i] = d;
        er->tap_gain[i] = (1.0f - absorption) / (1.0f + taps[i].extra);

        sh_eval(taps[i].x, taps[i].y, taps[i].z, er->tap_sh[i]);
    }
}

// One-block process: encode mono room-send into ambisonic bus, convolve each
// SH channel with its precomputed SH-HRIR, sum into binaural out.
// The decoder's convolver already applies the per-order max-rE scalar implicitly
// (it's baked into the precomputed basis for order 1 with uniform weighting).
static void er_process_ambi(EarlyReflections *er, const float *mono_in,
                            float *out_l, float *out_r, int n) {
    if (!er->initialized || !er->decoder_valid || er->num_taps <= 0) return;

    // 1. Encode: for each sample of the block, write mono_in into the delay
    //    line and read each tap's delayed sample into W/Y/Z/X buses.
    for (int i = 0; i < AMBI_NUM_CH; i++)
        memset(er->bus[i], 0, n * sizeof(float));

    for (int i = 0; i < n; i++) {
        er->delay_buf[er->write_pos] = mono_in[i];
        for (int t = 0; t < er->num_taps; t++) {
            int rp = er->write_pos - er->tap_delays[t];
            if (rp < 0) rp += er->delay_size;
            float s = er->delay_buf[rp] * er->tap_gain[t];
            er->bus[0][i] += s * er->tap_sh[t][0];
            er->bus[1][i] += s * er->tap_sh[t][1];
            er->bus[2][i] += s * er->tap_sh[t][2];
            er->bus[3][i] += s * er->tap_sh[t][3];
        }
        if (++er->write_pos >= er->delay_size) er->write_pos = 0;
    }

    // 2. Decode: each bus × its SH-HRIR → sum to stereo output.
    float tmp_l[HRTF_BLOCK_SIZE];
    float tmp_r[HRTF_BLOCK_SIZE];
    int block = n < HRTF_BLOCK_SIZE ? n : HRTF_BLOCK_SIZE;

    for (int i = 0; i < AMBI_NUM_CH; i++) {
        convolver_process(&er->dec_l[i], er->bus[i], tmp_l);
        convolver_process(&er->dec_r[i], er->bus[i], tmp_r);
        for (int k = 0; k < block; k++) {
            out_l[k] += tmp_l[k];
            out_r[k] += tmp_r[k];
        }
    }
}

static void er_clear(EarlyReflections *er) {
    if (!er->initialized) return;
    if (er->delay_buf) memset(er->delay_buf, 0, er->delay_size * sizeof(float));
    for (int i = 0; i < AMBI_NUM_CH; i++) {
        if (er->bus[i]) memset(er->bus[i], 0, HRTF_BLOCK_SIZE * sizeof(float));
    }
    er->write_pos = 0;
}

static void er_destroy(EarlyReflections *er) {
    free(er->delay_buf);
    for (int i = 0; i < AMBI_NUM_CH; i++) {
        free(er->bus[i]);
        convolver_destroy(&er->dec_l[i]);
        convolver_destroy(&er->dec_r[i]);
    }
    memset(er, 0, sizeof(*er));
}

// ---------------------------------------------------------------------------
// SOFA HRIR loading
// ---------------------------------------------------------------------------

/* Compute diffuse-field equalization filter from SOFA data.
 * The DFC removes per-SOFA spectral coloration so that all SOFA
 * files produce consistent spatial positioning regardless of the
 * measurement head/dummy used.
 *
 * Process:
 * 1. Sample HRIRs at N uniformly distributed directions
 * 2. Compute average power spectrum across all directions and ears
 * 3. Invert it → DFC filter (applied to each HRIR on retrieval)
 *
 * Result stored in p->dfc_filter_l/r (frequency domain, PFFFT format) */
static void compute_diffuse_field_eq(struct priv *p) {
    if (!p->sofa || p->hrir_length <= 0)
        return;

    int len = p->hrir_length;
    int fft_n = HRTF_FFT_N;

    /* Allocate accumulator for average power spectrum */
    float *avg_power = pffft_aligned_malloc(fft_n * sizeof(float));
    float *tmp_td = pffft_aligned_malloc(fft_n * sizeof(float));
    float *tmp_fd = pffft_aligned_malloc(fft_n * sizeof(float));
    float *work = pffft_aligned_malloc(fft_n * sizeof(float));
    PFFFT_Setup *fft = pffft_new_setup(fft_n, PFFFT_REAL);

    memset(avg_power, 0, fft_n * sizeof(float));

    /* Sample directions: azimuth every 30°, elevation -30° to 60° every 30° */
    float *ir_l = calloc(len, sizeof(float));
    float *ir_r = calloc(len, sizeof(float));
    int n_dirs = 0;

    for (float el = -30.0f; el <= 60.0f; el += 30.0f) {
        for (float az = 0.0f; az < 360.0f; az += 30.0f) {
            float az_rad = az * (float)(M_PI / 180.0);
            float el_rad = el * (float)(M_PI / 180.0);
            float x = cosf(el_rad) * cosf(az_rad);
            float y = cosf(el_rad) * sinf(az_rad);
            float z = sinf(el_rad);
            float dl = 0, dr = 0;

            mysofa_getfilter_float(p->sofa, x, y, z, ir_l, ir_r, &dl, &dr);

            /* Accumulate power spectrum for both ears */
            for (int ear = 0; ear < 2; ear++) {
                float *ir = (ear == 0) ? ir_l : ir_r;
                memset(tmp_td, 0, fft_n * sizeof(float));
                memcpy(tmp_td, ir, len * sizeof(float));
                pffft_transform(fft, tmp_td, tmp_fd, work, PFFFT_FORWARD);

                /* Accumulate |H(f)|^2 (power) */
                /* PFFFT real format: [DC, f1_re, f1_im, f2_re, f2_im, ..., Nyquist] */
                avg_power[0] += tmp_fd[0] * tmp_fd[0]; /* DC */
                avg_power[1] += tmp_fd[1] * tmp_fd[1]; /* Nyquist */
                for (int k = 2; k < fft_n; k += 2) {
                    float re = tmp_fd[k];
                    float im = tmp_fd[k + 1];
                    avg_power[k] += re * re + im * im;
                    avg_power[k + 1] += re * re + im * im; /* same power for re/im slot */
                }
            }
            n_dirs++;
        }
    }

    /* Average and compute inverse sqrt (= magnitude inverse) */
    float scale_dirs = 1.0f / (float)(n_dirs * 2); /* n_dirs × 2 ears */
    p->dfc_filter = pffft_aligned_malloc(fft_n * sizeof(float));

    /* DC and Nyquist */
    float dc_avg = avg_power[0] * scale_dirs;
    float ny_avg = avg_power[1] * scale_dirs;
    p->dfc_filter[0] = (dc_avg > 1e-12f) ? 1.0f / sqrtf(dc_avg) : 1.0f;
    p->dfc_filter[1] = (ny_avg > 1e-12f) ? 1.0f / sqrtf(ny_avg) : 1.0f;

    for (int k = 2; k < fft_n; k += 2) {
        float pwr = avg_power[k] * scale_dirs;
        float inv = (pwr > 1e-12f) ? 1.0f / sqrtf(pwr) : 1.0f;
        /* Clamp to prevent extreme boost at low-energy frequencies */
        if (inv > 10.0f) inv = 10.0f;
        p->dfc_filter[k] = inv;
        p->dfc_filter[k + 1] = inv;
    }

    p->dfc_valid = 1;
    HRTF_DBG("DFC computed: %d directions, filter[0]=%.3f filter[512]=%.3f\n",
              n_dirs, p->dfc_filter[0],
              fft_n > 512 ? p->dfc_filter[512] : 0.0f);

    pffft_destroy_setup(fft);
    pffft_aligned_free(avg_power);
    pffft_aligned_free(tmp_td);
    pffft_aligned_free(tmp_fd);
    pffft_aligned_free(work);
    free(ir_l);
    free(ir_r);
}

static int load_sofa(struct priv *p, const char *path) {
    int filter_length = 0;
    int err;

    if (p->sofa) {
        mysofa_close(p->sofa);
        p->sofa = NULL;
    }
    if (p->dfc_filter) {
        pffft_aligned_free(p->dfc_filter);
        p->dfc_filter = NULL;
        p->dfc_valid = 0;
    }

    p->sofa = mysofa_open(path, (float)p->sample_rate, &filter_length, &err);
    if (!p->sofa || err != MYSOFA_OK) {
        HRTF_DBG("SOFA LOAD FAILED: path=%s err=%d\n", path, err);
        p->sofa = NULL;
        return -1;
    }

    p->hrir_length = filter_length;
    HRTF_DBG("SOFA loaded OK: path=%s hrir_len=%d\n", path, filter_length);

    /* Compute diffuse-field equalization for consistent spatial image */
    compute_diffuse_field_eq(p);

    return 0;
}

static float sample_hrir_lagrange4(const float *src, int len, float pos)
{
    if (pos < 0.0f || pos >= (float)len)
        return 0.0f;

    int base = (int)floorf(pos);
    float frac = pos - (float)base;
    int idx0 = base - 1;
    int idx1 = base;
    int idx2 = base + 1;
    int idx3 = base + 2;

    float s0 = (idx0 >= 0 && idx0 < len) ? src[idx0] : 0.0f;
    float s1 = (idx1 >= 0 && idx1 < len) ? src[idx1] : 0.0f;
    float s2 = (idx2 >= 0 && idx2 < len) ? src[idx2] : 0.0f;
    float s3 = (idx3 >= 0 && idx3 < len) ? src[idx3] : 0.0f;

    float c0 = -frac * (frac - 1.0f) * (frac - 2.0f) / 6.0f;
    float c1 = (frac + 1.0f) * (frac - 1.0f) * (frac - 2.0f) / 2.0f;
    float c2 = -(frac + 1.0f) * frac * (frac - 2.0f) / 2.0f;
    float c3 = (frac + 1.0f) * frac * (frac - 1.0f) / 6.0f;

    return c0 * s0 + c1 * s1 + c2 * s2 + c3 * s3;
}

static void shift_hrir_fractional(const float *src, int len, float delay_samples,
                                  float *dst)
{
    memset(dst, 0, len * sizeof(float));
    if (delay_samples < 0.0f)
        delay_samples = 0.0f;

    for (int i = 0; i < len; i++) {
        float src_pos = (float)i - delay_samples;
        dst[i] = sample_hrir_lagrange4(src, len, src_pos);
    }
}

static void get_hrir_for_position(struct priv *p, float azimuth, float elevation,
                                  float distance, float *hrir_l, float *hrir_r) {
    if (!p->sofa) {
        memset(hrir_l, 0, p->hrir_length * sizeof(float));
        memset(hrir_r, 0, p->hrir_length * sizeof(float));
        return;
    }

    // Convert spherical to cartesian for libmysofa
    // libmysofa expects: x=front, y=left, z=up
    float az_rad = azimuth * (float)(M_PI / 180.0);
    float el_rad = elevation * (float)(M_PI / 180.0);

    float x = distance * cosf(el_rad) * cosf(az_rad);
    float y = distance * cosf(el_rad) * sinf(az_rad);
    float z = distance * sinf(el_rad);

    float coords[3] = {x, y, z};
    float delay_l = 0, delay_r = 0;

    float *ir_l = calloc(p->hrir_length, sizeof(float));
    float *ir_r = calloc(p->hrir_length, sizeof(float));

    mysofa_getfilter_float(p->sofa, coords[0], coords[1], coords[2],
                           ir_l, ir_r, &delay_l, &delay_r);

    int len = p->hrir_length;

    /* Apply diffuse-field equalization: multiply HRIR spectrum by DFC filter.
     * This removes per-SOFA spectral coloration, making all SOFA files
     * produce consistent spatial positioning. */
    if (p->dfc_valid && p->dfc_filter) {
        int fft_n = HRTF_FFT_N;
        float *td = pffft_aligned_malloc(fft_n * sizeof(float));
        float *fd = pffft_aligned_malloc(fft_n * sizeof(float));
        float *work = pffft_aligned_malloc(fft_n * sizeof(float));
        PFFFT_Setup *fft = pffft_new_setup(fft_n, PFFFT_REAL);

        for (int ear = 0; ear < 2; ear++) {
            float *ir = (ear == 0) ? ir_l : ir_r;

            memset(td, 0, fft_n * sizeof(float));
            memcpy(td, ir, len * sizeof(float));
            pffft_transform(fft, td, fd, work, PFFFT_FORWARD);

            fd[0] *= p->dfc_filter[0];
            fd[1] *= p->dfc_filter[1];
            for (int k = 2; k < fft_n; k += 2) {
                float g = p->dfc_filter[k];
                fd[k]     *= g;
                fd[k + 1] *= g;
            }

            pffft_transform(fft, fd, td, work, PFFFT_BACKWARD);
            float inv_n = 1.0f / (float)fft_n;
            for (int i = 0; i < len; i++)
                ir[i] = td[i] * inv_n;
        }

        pffft_destroy_setup(fft);
        pffft_aligned_free(td);
        pffft_aligned_free(fd);
        pffft_aligned_free(work);
    }

    /* One-time diagnostic: print coords + delays for first 16 HRIR lookups
     * so we can verify the SOFA coordinate convention. For FC (az=0) the
     * delays should be equal (source on median plane). */
    {
        static int sofa_diag_count = 0;
        if (sofa_diag_count < 16) {
            HRTF_DBG("SOFA diag #%d: az=%.1f el=%.1f dist=%.2f -> xyz=(%.3f,%.3f,%.3f) "
                      "delay_L=%.6f delay_R=%.6f\n",
                      sofa_diag_count, azimuth, elevation, distance,
                      x, y, z, delay_l, delay_r);
            sofa_diag_count++;
        }
    }

    // Apply ITD with fractional-sample precision so the HRIR onset keeps the
    // fine time-of-arrival cues that are lost when delays are quantized.
    float min_delay = delay_l < delay_r ? delay_l : delay_r;
    float dl = (delay_l - min_delay) * p->sample_rate;
    float dr = (delay_r - min_delay) * p->sample_rate;

    float max_shift = (float)(p->hrir_length / 4);  // safety clamp
    if (dl < 0.0f) dl = 0.0f;
    if (dr < 0.0f) dr = 0.0f;
    if (dl > max_shift) dl = max_shift;
    if (dr > max_shift) dr = max_shift;

    // Optional minimum-phase preprocessing.  With ITD already applied as a
    // separate fractional shift (below), removing the residual phase from
    // the HRIR concentrates its energy near t=0 and equalises the phase
    // response across nearby SOFA directions — neighbouring HRIRs blend
    // without comb-filter artefacts.  Steam Audio does this by default.
    if (p->shared && atomic_load_explicit(&p->shared->direct_min_phase,
                                           memory_order_relaxed)) {
        float *mp = pffft_aligned_malloc(len * sizeof(float));
        hrir_to_minimum_phase(ir_l, len, mp);
        memcpy(ir_l, mp, len * sizeof(float));
        hrir_to_minimum_phase(ir_r, len, mp);
        memcpy(ir_r, mp, len * sizeof(float));
        pffft_aligned_free(mp);
    }

    // Taper only the very end of the HRIR tail. The previous 40% fade-out was
    // removing useful pinna/room cues and flattening the image.
    {
        int fade_len = (int)(len * 0.125f);
        if (fade_len < 4)
            fade_len = len < 4 ? len : 4;
        int fade_start = len - fade_len;
        int fade_denom = fade_len > 1 ? (fade_len - 1) : 1;
        for (int i = fade_start; i < len; i++) {
            float t = (float)(i - fade_start) / (float)fade_denom;
            float w = 0.5f * (1.0f + cosf(t * (float)M_PI));  // 1 → 0
            ir_l[i] *= w;
            ir_r[i] *= w;
        }
    }

    shift_hrir_fractional(ir_l, len, dl, hrir_l);
    shift_hrir_fractional(ir_r, len, dr, hrir_r);

    free(ir_l);
    free(ir_r);
}

// Update convolvers for a given channel with new HRIR
// crossfade=1: real-time update with adaptive equal-power crossfade
// crossfade=0: direct set on active slot (init/reload, no crossfade)
//
// No rate-limit: the default crossfade is now one block (256 samples ≈ 5 ms)
// so any new position arriving from OAMD/sidecar always lands inside the next
// audio block, giving smooth motion for fast-moving Atmos objects.  When a
// new update interrupts an in-flight crossfade we still swap convolver slots
// — the previous in-flight target becomes the "from" side of the next
// crossfade.  This produces a brief sample-level discontinuity at the swap
// moment, but it scales with the HRIR delta between adjacent positions: tiny
// for smooth motion (the dominant case), and masked by the audio change for
// teleports.
static void update_channel_hrir_ex(struct priv *p, int ch, int crossfade) {
    HrtfChannelPair *pair = &p->channels[ch];

    float *hrir_l = calloc(p->hrir_length, sizeof(float));
    float *hrir_r = calloc(p->hrir_length, sizeof(float));

    get_hrir_for_position(p, p->speaker_pos[ch].azimuth,
                          p->speaker_pos[ch].elevation,
                          p->speaker_pos[ch].distance,
                          hrir_l, hrir_r);

    if (crossfade && pair->left[pair->active_idx].valid) {
        int active = pair->active_idx;
        int next = 1 - active;

        convolver_set_hrir(&pair->left[next], hrir_l, p->hrir_length);
        convolver_set_hrir(&pair->right[next], hrir_r, p->hrir_length);

        // Do NOT copy overlap from the active convolver.  The overlap was
        // generated by the OLD HRIR.  Mixing it with the NEW HRIR's
        // convolution output produces an incoherent sum that sounds like
        // spatial corruption (the "bugged object audio" artifact).
        //
        // convolver_set_hrir() already zeroed the new slot's overlap, so
        // the new convolver starts with a clean slate.  The crossfade below
        // (process_block) keeps running the OLD convolver with its correct
        // overlap, so the old HRIR's tail rings out naturally through the
        // fade.  The new convolver ramps in from silence — no discontinuity.

        /* Adaptive crossfade length from OAMD ramp_duration.
         * Short ramp (Atmos with explicit metadata)  → matched short crossfade.
         * Unknown ramp (non-Atmos / sidecar updates) → one block (smooth
         * per-block updates on continuously-moving objects). */
        int xfade = p->oamd_ramp_duration;
        if (xfade <= 0) xfade = HRTF_BLOCK_SIZE;
        if (xfade < 64) xfade = 64;
        if (xfade > HRTF_CROSSFADE_LEN) xfade = HRTF_CROSSFADE_LEN;
        pair->crossfade_total = xfade;
        pair->crossfade_remaining = xfade;
        pair->active_idx = next;
    } else {
        // Direct set on active slot (init/reload or first-time setup)
        int slot = pair->active_idx;
        convolver_set_hrir(&pair->left[slot], hrir_l, p->hrir_length);
        convolver_set_hrir(&pair->right[slot], hrir_r, p->hrir_length);
    }

    free(hrir_l);
    free(hrir_r);
}

static void update_channel_hrir(struct priv *p, int ch) {
    update_channel_hrir_ex(p, ch, 1);  // default: with crossfade
}

// ---------------------------------------------------------------------------
// Speaker position mapping from channel layout
// ---------------------------------------------------------------------------

// Map mp_speaker_id to (azimuth, elevation) in SOFA convention
// (azimuth: 0=front, 90=left, -90=right, 180=back)
static void speaker_id_to_position(int speaker_id, float *az, float *el) {
    switch (speaker_id) {
    case MP_SPEAKER_ID_FL:   *az =  30.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_FR:   *az = -30.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_FC:   *az =   0.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_LFE:  *az =   0.0f; *el = -30.0f; break;
    case MP_SPEAKER_ID_BL:   *az = 135.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_BR:   *az =-135.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_FLC:  *az =  15.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_FRC:  *az = -15.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_BC:   *az = 180.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_SL:   *az =  90.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_SR:   *az = -90.0f; *el =   0.0f; break;
    case MP_SPEAKER_ID_TC:   *az =   0.0f; *el =  90.0f; break;
    case MP_SPEAKER_ID_TFL:  *az =  45.0f; *el =  45.0f; break;
    case MP_SPEAKER_ID_TFC:  *az =   0.0f; *el =  45.0f; break;
    case MP_SPEAKER_ID_TFR:  *az = -45.0f; *el =  45.0f; break;
    case MP_SPEAKER_ID_TBL:  *az = 135.0f; *el =  45.0f; break;
    case MP_SPEAKER_ID_TBC:  *az = 180.0f; *el =  45.0f; break;
    case MP_SPEAKER_ID_TBR:  *az =-135.0f; *el =  45.0f; break;
    case MP_SPEAKER_ID_SDL:  *az =  90.0f; *el =  45.0f; break;
    case MP_SPEAKER_ID_SDR:  *az = -90.0f; *el =  45.0f; break;
    case MP_SPEAKER_ID_NA:
        // Silent / padding channel.  mpv uses this for unused slots in a
        // chmap (e.g. Atmos streams that pad out to a fixed channel count).
        // Park it out of the frontal stage; OAMD or objcoding metadata will
        // override the position if the channel actually carries an object.
        *az = 180.0f; *el = 60.0f;
        break;
    default:
        // Unknown speaker ID — park it high up and behind (180°, 60°) so it
        // doesn't contaminate the FC/frontal stage where (0°, 0°) lives.
        // Log so we can add the ID to the switch above.
        fprintf(stderr,
                "[af_hrtf] WARNING: unknown speaker id %d → placing at "
                "(180°, 60°); consider adding it to speaker_id_to_position\n",
                speaker_id);
        *az = 180.0f; *el = 60.0f;
        break;
    }
}

// Set speaker positions from the decoder's bed_mask (FFmpeg AV_CH bitmask).
// The output channel order matches the bit order: lowest set bit = ch0, etc.
// This is authoritative — it comes directly from the lossless HD stream header.
static int is_top_speaker(int bit) {
    return bit == MP_SPEAKER_ID_TC  || bit == MP_SPEAKER_ID_TFL ||
           bit == MP_SPEAKER_ID_TFC || bit == MP_SPEAKER_ID_TFR ||
           bit == MP_SPEAKER_ID_TBL || bit == MP_SPEAKER_ID_TBC ||
           bit == MP_SPEAKER_ID_TBR || bit == MP_SPEAKER_ID_TSL ||
           bit == MP_SPEAKER_ID_TSR;
}

static int init_speaker_positions_from_bed_mask(struct priv *p, uint64_t bed_mask) {
    /* Compute speaker distances from room dimensions.
     * Listener at center width, 2/3 depth from front wall.
     * Speakers placed according to cinema/home theater standards. */
    float room_w = 6.5f, room_d = 5.0f, room_h = 2.7f;
    if (p->shared) {
        float rw = atomic_load_explicit(&p->shared->room_width, memory_order_relaxed);
        float rd = atomic_load_explicit(&p->shared->room_depth, memory_order_relaxed);
        float rh = atomic_load_explicit(&p->shared->room_height, memory_order_relaxed);
        if (rw > 0) room_w = rw;
        if (rd > 0) room_d = rd;
        if (rh > 0) room_h = rh;
    }
    /* Listener position: center width, 2/3 depth from front */
    float listen_depth = room_d * (2.0f / 3.0f);
    /* Distance from listener to front wall (screen) */
    float d_front = listen_depth;
    /* Distance from listener to side walls */
    float d_side = room_w * 0.5f;
    /* Distance from listener to back wall */
    float d_back = room_d - listen_depth;
    /* Distance from listener to front speakers (at screen, offset ±30°) */
    float d_screen = sqrtf(d_front * d_front + (d_side * 0.5f) * (d_side * 0.5f));
    /* Subwoofer: below screen, on floor */
    float d_sub = sqrtf(d_front * d_front + 1.2f * 1.2f); /* 1.2m below ear level */

    int ch_idx = 0;
    int bed_count = 0;
    int height_count = 0;

    HRTF_DBG("init_speaker_positions_from_bed_mask: mask=0x%llx [",
             (unsigned long long)bed_mask);

    // Iterate set bits in order (bit position = AV_CHAN id = mp_speaker_id)
    for (int bit = 0; bit < 64 && ch_idx < HRTF_MAX_CHANNELS; bit++) {
        if (!(bed_mask & (1ULL << bit)))
            continue;
        float az, el;
        speaker_id_to_position(bit, &az, &el);
        /* Assign distance based on speaker type and room geometry */
        float d;
        switch (bit) {
        case MP_SPEAKER_ID_FL:  case MP_SPEAKER_ID_FR:
        case MP_SPEAKER_ID_FC:  case MP_SPEAKER_ID_FLC:
        case MP_SPEAKER_ID_FRC:
            d = d_front;    /* Front speakers at screen distance */
            break;
        case MP_SPEAKER_ID_LFE:
            d = d_sub;      /* Sub: front wall + below ear level */
            break;
        case MP_SPEAKER_ID_SL:  case MP_SPEAKER_ID_SR:
            d = d_side;     /* Sides at wall distance */
            break;
        case MP_SPEAKER_ID_BL:  case MP_SPEAKER_ID_BR:
        case MP_SPEAKER_ID_BC:
            d = d_back + 0.5f;  /* Surrounds just behind listener */
            break;
        default:
            /* Height speakers: at ceiling distance */
            d = sqrtf(d_front * d_front + (room_h - 1.2f) * (room_h - 1.2f));
            break;
        }
        p->speaker_pos[ch_idx] = (HrtfSpeakerPos){az, el, d};
        HRTF_DBG(" ch%d=bit%d(%.0f,%.0f)%s", ch_idx, bit, az, el,
                 is_top_speaker(bit) ? "[H]" : "");
        if (is_top_speaker(bit)) {
            if (height_count < HRTF_MAX_CHANNELS) {
                p->height_ch_idx[height_count] = ch_idx;
                height_count++;
            }
        } else {
            bed_count++;
        }
        ch_idx++;
    }

    p->num_height_channels = height_count;
    // Initialize smooth position state from default speaker positions
    for (int h = 0; h < height_count; h++) {
        int ci = p->height_ch_idx[h];
        p->height_smooth_az[h] = p->speaker_pos[ci].azimuth;
        p->height_smooth_el[h] = p->speaker_pos[ci].elevation;
        p->height_smooth_dist[h] = p->speaker_pos[ci].distance;
    }
    p->height_smooth_valid = 1;

    HRTF_DBG(" ] total=%d bed=%d height=%d\n", ch_idx, bed_count,
             ch_idx - bed_count);
    return bed_count;
}

// Check if a speaker ID is a known fixed-position channel (bed, not object)
static int is_known_speaker(int sid) {
    switch (sid) {
    case MP_SPEAKER_ID_FL:  case MP_SPEAKER_ID_FR:  case MP_SPEAKER_ID_FC:
    case MP_SPEAKER_ID_LFE: case MP_SPEAKER_ID_BL:  case MP_SPEAKER_ID_BR:
    case MP_SPEAKER_ID_FLC: case MP_SPEAKER_ID_FRC: case MP_SPEAKER_ID_BC:
    case MP_SPEAKER_ID_SL:  case MP_SPEAKER_ID_SR:  case MP_SPEAKER_ID_TC:
    case MP_SPEAKER_ID_TFL: case MP_SPEAKER_ID_TFC: case MP_SPEAKER_ID_TFR:
    case MP_SPEAKER_ID_TBL: case MP_SPEAKER_ID_TBC: case MP_SPEAKER_ID_TBR:
    case MP_SPEAKER_ID_SDL: case MP_SPEAKER_ID_SDR:
        return 1;
    default:
        return 0;
    }
}

// Fallback: set speaker positions from mpv chmap (may be wrong for spatial)
static void init_speaker_positions_from_chmap(struct priv *p,
                                              struct mp_chmap *chmap) {
    const float d = 2.0f;
    int n = chmap->num;
    if (n > HRTF_MAX_CHANNELS) n = HRTF_MAX_CHANNELS;

    // Count how many channels have known speaker IDs (= bed channels)
    int known_count = 0;
    HRTF_DBG("init_speaker_positions_from_chmap: %d channels [", n);
    for (int i = 0; i < n; i++) {
        int sid = chmap->speaker[i];
        float az, el;
        speaker_id_to_position(sid, &az, &el);
        p->speaker_pos[i] = (HrtfSpeakerPos){az, el, d};
        if (is_known_speaker(sid))
            known_count++;
        HRTF_DBG(" ch%d=spk%d(%.0f,%.0f)", i, sid, az, el);
    }
    HRTF_DBG(" ] known_bed=%d\n", known_count);

    // If all channels are known speakers (e.g. 7.1.4), treat them all as bed
    if (known_count == n)
        p->num_bed_channels = n;
}

// Swap speaker positions for channels whose audio payload is in SMPTE order
// (FL FR FC LFE SL SR BL BR ...) while the chmap/bed_mask reports them in
// WAVE/bit order (FL FR FC LFE BL BR SL SR ...).  This is the default for
// Dolby Atmos / DCI cinema content: FFmpeg reshuffles the layout mask into
// bit order but does not remap the audio samples, so channel 4 carries the
// "SL" waveform even though FFmpeg calls it BL.  Swapping the position of
// channels 4↔6 and 5↔7 (and 10↔10 no-op for TBL/TSL, kept for symmetry)
// puts the audio at the direction the content creator intended.
static void apply_smpte_channel_order(struct priv *p) {
    // 6.1 (7 channels): Dolby SMPTE order is FL FR FC LFE SL SR BC, but
    // FFmpeg's AV_CH_LAYOUT_6POINT1 normalises to FL FR FC LFE BC SL SR
    // (bit order 0,1,2,3,8,9,10).  If the audio payload is still in SMPTE
    // order, ch4 carries SL, ch5 carries SR, ch6 carries BC.  Rotate the
    // three tail positions one slot left so the channels land at the
    // direction their audio was intended for:
    //   speaker_pos[4] ← SL (90°)
    //   speaker_pos[5] ← SR (−90°)
    //   speaker_pos[6] ← BC (180°)
    if (p->num_channels == 7) {
        HrtfSpeakerPos t = p->speaker_pos[4];
        p->speaker_pos[4] = p->speaker_pos[5];
        p->speaker_pos[5] = p->speaker_pos[6];
        p->speaker_pos[6] = t;
        return;
    }
    if (p->num_channels >= 8) {
        HrtfSpeakerPos t;
        t = p->speaker_pos[4]; p->speaker_pos[4] = p->speaker_pos[6]; p->speaker_pos[6] = t;
        t = p->speaker_pos[5]; p->speaker_pos[5] = p->speaker_pos[7]; p->speaker_pos[7] = t;
    }
}

// Fallback: hardcoded 7.1.4 positions for Home Theater (6.5x5.0x2.7m)
static void init_speaker_positions(struct priv *p) {
    const float df = 3.33f;  // front: 2/3 of 5m depth
    const float ds = 3.25f;  // side: half of 6.5m width
    const float db = 2.17f;  // back: 1/3 of 5m depth + 0.5m
    const float dh = 3.6f;   // height: sqrt(3.33^2 + 1.5^2)
    const float dsub = 3.54f; // sub: sqrt(3.33^2 + 1.2^2)
    HrtfSpeakerPos defaults[] = {
        { 30.0f,  0.0f, df},    // 0: FL
        {-30.0f,  0.0f, df},    // 1: FR
        {  0.0f,  0.0f, df},    // 2: FC
        {  0.0f,-30.0f, dsub},  // 3: LFE (sub, below screen)
        {135.0f,  0.0f, db},    // 4: BL
        {-135.0f, 0.0f, db},    // 5: BR
        { 90.0f,  0.0f, ds},    // 6: SL
        {-90.0f,  0.0f, ds},    // 7: SR
        { 45.0f, 45.0f, dh},    // 8: TFL
        {-45.0f, 45.0f, dh},    // 9: TFR
        {135.0f, 45.0f, dh},    // 10: TBL
        {-135.0f,45.0f, dh},    // 11: TBR
    };

    int n = sizeof(defaults) / sizeof(defaults[0]);
    if (n > HRTF_MAX_CHANNELS) n = HRTF_MAX_CHANNELS;
    memcpy(p->speaker_pos, defaults, n * sizeof(HrtfSpeakerPos));
}

// ---------------------------------------------------------------------------
// Distance attenuation helpers
// ---------------------------------------------------------------------------

// Forward declaration (defined below)
static void update_min_dist(struct priv *p);

// Soft elevation floor: smoothly pulls elevation toward a minimum threshold
// without the hard discontinuity of a simple clamp.
// Below 20°: quadratic blend toward 15° floor.  At 20°+: no change.
static inline float soft_elevation_floor(float el) {
    if (el >= 20.0f) return el;
    float t = (20.0f - el) / 20.0f;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    float blend = t * t;
    return el * (1.0f - blend) + 15.0f * blend;
}

// Compute exponential smoothing alpha from OAMD ramp_duration.
// Short ramp → low alpha (fast tracking).  Long ramp → high alpha (heavy smooth).
// Update interval ~240 samples (5ms at 48kHz) = typical OAMD cadence.
static inline float compute_smooth_alpha(int ramp_duration) {
    const float UPDATE_INTERVAL = 240.0f;
    if (ramp_duration <= 0)
        return 0.85f;  // fallback when OAMD not available
    float ratio = (float)ramp_duration / UPDATE_INTERVAL;
    if (ratio < 0.5f)  return 0.0f;   // snap to target (fast object)
    if (ratio > 10.0f) return 0.95f;  // heavy smoothing (slow object)
    return (ratio - 0.5f) / 9.5f * 0.95f;
}

static void update_object_positions_from_coefficients(struct priv *p) {
    /* The spatial rematrix already renders all spatial effects (object panning,
     * height channels) into the 7.1 speaker feeds (channels 0-7).  The HRTF
     * filter virtualizes those fixed speaker positions.
     *
     * When the bed_mask first arrives from the decoder, apply it to set
     * correct speaker positions (the init-time check may have missed it
     * if the decoder hadn't processed the first spatial block yet). */
    if (!g_spatial_coeff_ptr)
        return;

    if (atomic_load(&g_spatial_coeff_ptr->updated)) {
        atomic_store(&g_spatial_coeff_ptr->updated, 0);

        uint64_t mask = g_spatial_coeff_ptr->bed_mask;
        /* Height speaker bits are now valid: the decoder ch_assign fix
         * ensures correct channel mapping for the spatial substream. */
        if (mask && mask != p->last_bed_mask) {
            HRTF_DBG("update_obj_coeff: bed_mask changed 0x%llx -> 0x%llx, reloading positions\n",
                     (unsigned long long)p->last_bed_mask, (unsigned long long)mask);
            int bed_count = init_speaker_positions_from_bed_mask(p, mask);
            p->num_bed_channels = bed_count;
            p->last_bed_mask = mask;
            if (p->shared) {
                for (int ch = 0; ch < p->num_channels && ch < HRTF_SS_MAX_CHANNELS; ch++)
                    p->shared->speaker_pos[ch] = p->speaker_pos[ch];
            }
            if (p->sofa) {
                int reload_count = bed_count > 0 ? bed_count : p->num_channels;
                for (int ch = 0; ch < reload_count && ch < HRTF_MAX_CHANNELS; ch++)
                    update_channel_hrir_ex(p, ch, 0);
                update_min_dist(p);
            }

            /* Resolve rematrix row indices for height channels.
             * out_ch[mat] tells us which output channel row 'mat' produces.
             * We match against height_ch_idx[] to find which matrix row
             * corresponds to each height speaker for bed subtraction. */
            if (p->num_height_channels > 0 && g_spatial_coeff_ptr->num_matrices > 0) {
                for (int h = 0; h < p->num_height_channels; h++)
                    p->height_mat_idx[h] = -1;

                for (int mat = 0; mat < g_spatial_coeff_ptr->num_matrices; mat++) {
                    int out = g_spatial_coeff_ptr->out_ch[mat];
                    for (int h = 0; h < p->num_height_channels; h++) {
                        if (out == p->height_ch_idx[h]) {
                            p->height_mat_idx[h] = mat;
                            HRTF_DBG("  height ch[%d] (ch_idx=%d) -> mat_row=%d\n",
                                     h, p->height_ch_idx[h], mat);
                        }
                    }
                }
                p->height_mat_valid = 1;
            }
        }
    }

    /* Detect degraded mode transitions: when num_matrices drops to 0,
     * height channels lose their spatial content.  Trigger a fade-out
     * so the convolver overlap tail rings out smoothly instead of
     * creating a hard discontinuity.  On recovery, trigger fade-in. */
    int height_active_now = (g_spatial_coeff_ptr->num_matrices > 0 &&
                             p->num_height_channels > 0);
    if (p->height_was_active && !height_active_now) {
        p->height_fade_remaining = HEIGHT_FADE_LEN;  /* positive = fade-out */
        HRTF_DBG("height degraded: starting fade-out (%d samples)\n", HEIGHT_FADE_LEN);
    } else if (!p->height_was_active && height_active_now && p->height_fade_remaining >= 0) {
        p->height_fade_remaining = -HEIGHT_FADE_LEN; /* negative = fade-in */
        HRTF_DBG("height restored: starting fade-in (%d samples)\n", HEIGHT_FADE_LEN);
    }
    p->height_was_active = height_active_now;
}

/** Read real-time OAMD object positions and compute gain-weighted centroid
 *  positioning for height channels.
 *
 *  Since TrueHD Atmos encodes 11 dynamic objects into only 2 residual channels
 *  (the rematrix produces speaker feeds, not individual object audio), we cannot
 *  separate individual objects. Instead we use the OAMD metadata to dynamically
 *  position the height channel HRTF at a gain-weighted centroid of all active
 *  objects. When only 1-2 objects dominate (others muted), the centroid converges
 *  to their exact positions for maximum spatial accuracy.
 *
 *  Also updates SharedState (object_x/y/z) for UI visualization. */
static void update_object_positions_from_objmeta(struct priv *p) {
    if (!g_spatial_objmeta_ptr)
        return;

    if (!atomic_load(&g_spatial_objmeta_ptr->updated))
        return;
    atomic_store(&g_spatial_objmeta_ptr->updated, 0);

    /* Read OAMD ramp_duration for adaptive crossfade/smoothing */
    if (g_spatial_objmeta_ptr->ramp_duration > 0)
        p->oamd_ramp_duration = (int)g_spatial_objmeta_ptr->ramp_duration;

    int num_obj = g_spatial_objmeta_ptr->num_objects;
    int num_bed = g_spatial_objmeta_ptr->num_bed_objects;
    if (num_obj <= 0 || num_obj > SPATIAL_EXT_MAX_OBJECTS) {
        if (p->shared) {
            atomic_store_explicit(&p->shared->num_objects, 0,
                                  memory_order_relaxed);
            for (int i = 0; i < HRTF_SS_MAX_OBJECTS; i++) {
                atomic_store_explicit(&p->shared->object_active[i], 0,
                                      memory_order_relaxed);
                atomic_store_explicit(&p->shared->object_gain[i], 0.0f,
                                      memory_order_relaxed);
            }
        }
        return;
    }
    if (num_bed < 0)
        num_bed = 0;
    if (num_bed > num_obj)
        num_bed = num_obj;

    float room_w = 6.5f, room_d = 5.0f, room_h = 2.7f;
    if (p->shared) {
        float rw = atomic_load_explicit(&p->shared->room_width, memory_order_relaxed);
        float rd = atomic_load_explicit(&p->shared->room_depth, memory_order_relaxed);
        float rh = atomic_load_explicit(&p->shared->room_height, memory_order_relaxed);
        if (rw > 0) room_w = rw;
        if (rd > 0) room_d = rd;
        if (rh > 0) room_h = rh;
    }

    { /* Debug: log ALL object metadata objects every ~100 calls */
        static int objmeta_recv_cnt = 0;
        if (++objmeta_recv_cnt % 100 == 1) {
            HRTF_DBG("objmeta recv[%d]: obj=%d bed=%d dyn=%d height_ch=%d\n",
                     objmeta_recv_cnt, num_obj, num_bed,
                     g_spatial_objmeta_ptr->num_dynamic_objects,
                     p->num_height_channels);
            for (int d = 0; d < num_obj && d < SPATIAL_EXT_MAX_OBJECTS; d++)
                HRTF_DBG("  [%d] act=%d x=%+.3f y=%+.3f z=%+.3f g=%d\n",
                         d, g_spatial_objmeta_ptr->objects[d].active,
                         g_spatial_objmeta_ptr->objects[d].x,
                         g_spatial_objmeta_ptr->objects[d].y,
                         g_spatial_objmeta_ptr->objects[d].z,
                         g_spatial_objmeta_ptr->objects[d].gain_db);
        }
    }

    int total_dyn = num_obj - num_bed;
    if (total_dyn < 0) total_dyn = 0;
    if (total_dyn > HRTF_SS_MAX_OBJECTS) total_dyn = HRTF_SS_MAX_OBJECTS;

    /* Reset SharedState object slots before populating */
    if (p->shared) {
        for (int i = 0; i < total_dyn; i++) {
            atomic_store_explicit(&p->shared->object_active[i], 0,
                                  memory_order_relaxed);
            atomic_store_explicit(&p->shared->object_gain[i], 0.0f,
                                  memory_order_relaxed);
        }
    }

    /* --- Phase 1: Update SharedState for UI + compute gain-weighted centroid --- */

    /* Centroid accumulators: separate for L and R height channels.
     * Objects on the left contribute more to L centroid and vice versa. */
    float wx_L = 0, wy_L = 0, wz_L = 0, wg_L = 0;
    float wx_R = 0, wy_R = 0, wz_R = 0, wg_R = 0;

    /* Dominant object gating: track top-gain objects globally and per L/R side.
     * Per-side dominant gives each height speaker its own best-match object,
     * improving spatial precision when objects are spread across the soundfield. */
    int dominant_count = 0;
    int dominant_idx = -1;
    float max_gain_db = -200.0f;

    int dominant_idx_L = -1, dominant_idx_R = -1;
    float max_gain_db_L = -200.0f, max_gain_db_R = -200.0f;

    for (int i = num_bed; i < num_obj && i < SPATIAL_EXT_MAX_OBJECTS; i++) {
        SpatialExtObjectPos *obj = &g_spatial_objmeta_ptr->objects[i];
        int obj_idx = i - num_bed;
        if (obj_idx < 0 || obj_idx >= HRTF_SS_MAX_OBJECTS)
            break;

        float dx = obj->x;
        float dy = obj->y;
        float dz = obj->z;

        /* Write to SharedState for UI visualization */
        if (p->shared) {
            atomic_store_explicit(&p->shared->object_x[obj_idx],
                                  (dx + 1.0f) * 0.5f, memory_order_relaxed);
            atomic_store_explicit(&p->shared->object_y[obj_idx],
                                  (dy + 1.0f) * 0.5f, memory_order_relaxed);
            atomic_store_explicit(&p->shared->object_z[obj_idx],
                                  dz, memory_order_relaxed);

            float gain_lin_vis = 0.0f;
            if (obj->active && obj->gain_db > -128) {
                gain_lin_vis = powf(10.0f, (float)obj->gain_db / 20.0f);
                if (gain_lin_vis > 1.0f) gain_lin_vis = 1.0f;
            }
            atomic_store_explicit(&p->shared->object_gain[obj_idx],
                                  gain_lin_vis, memory_order_relaxed);
            atomic_store_explicit(&p->shared->object_active[obj_idx],
                                  obj->active ? 1 : 0, memory_order_relaxed);
        }

        /* Skip inactive/muted objects for centroid calculation */
        if (!obj->active || obj->gain_db <= -128)
            continue;

        /* Dominant gating: count objects above -60 dB */
        if (obj->gain_db > -60) {
            dominant_count++;
            if (obj->gain_db > max_gain_db) {
                max_gain_db = obj->gain_db;
                dominant_idx = i;
            }
            /* Per-side dominant: objects with x < 0 contribute to left,
             * x > 0 to right, centered objects to both sides. */
            float w_L_test = fmaxf(0.0f, 0.5f - dx * 0.5f);
            float w_R_test = fmaxf(0.0f, 0.5f + dx * 0.5f);
            if (w_L_test > 0.3f && obj->gain_db > max_gain_db_L) {
                max_gain_db_L = obj->gain_db;
                dominant_idx_L = i;
            }
            if (w_R_test > 0.3f && obj->gain_db > max_gain_db_R) {
                max_gain_db_R = obj->gain_db;
                dominant_idx_R = i;
            }
        }

        /* Energy-weighted (gain²) centroid weighting: emphasizes loud
         * objects more strongly than linear gain weighting, giving
         * better spatial precision when one object dominates. */
        float gain_lin = powf(10.0f, (float)obj->gain_db / 20.0f);
        float gain = gain_lin * gain_lin;

        /* L/R panning weights based on horizontal position:
         * x=-1 (left) → w_L=1, w_R=0; x=+1 (right) → w_L=0, w_R=1 */
        float w_L = fmaxf(0.0f, 0.5f - dx * 0.5f);
        float w_R = fmaxf(0.0f, 0.5f + dx * 0.5f);

        wx_L += gain * w_L * dx;
        wy_L += gain * w_L * dy;
        wz_L += gain * w_L * dz;
        wg_L += gain * w_L;

        wx_R += gain * w_R * dx;
        wy_R += gain * w_R * dy;
        wz_R += gain * w_R * dz;
        wg_R += gain * w_R;
    }

    /* --- Phase 2a: DD+ JOC direct object positioning --- */
    /* When JOC is active, each reconstructed object gets its own HRTF at
     * its exact OAMD position.  Object channels start at index
     * num_bed_channels (i.e., 6 for 5.1). */
    if (p->objcoding_active && p->objcoding_num_objects > 0 && p->sofa) {
        const float JOC_SMOOTH = compute_smooth_alpha(p->oamd_ramp_duration);
        int bed_ch = p->num_bed_channels > 0 ? p->num_bed_channels : 6;

        for (int i = num_bed; i < num_obj && i < SPATIAL_EXT_MAX_OBJECTS; i++) {
            SpatialExtObjectPos *obj = &g_spatial_objmeta_ptr->objects[i];
            int obj_idx = i - num_bed;  /* 0-based object index */
            int ch = bed_ch + obj_idx;  /* channel in HRTF arrays */

            if (ch >= HRTF_MAX_CHANNELS || obj_idx >= OBJCODING_MAX_OBJECTS)
                break;
            if (!obj->active)
                continue;

            /* DAMF → room-relative cartesian */
            float rx = obj->x * room_w * 0.5f;
            float ry = obj->y * room_d * 0.5f;
            float rz = obj->z * room_h * 0.5f;

            float dist = sqrtf(rx * rx + ry * ry + rz * rz);
            if (dist < 0.3f) dist = 0.3f;

            float new_az = atan2f(-rx, -ry) * (180.0f / (float)M_PI);
            float new_el = asinf(fminf(1.0f, fmaxf(-1.0f, rz / dist)))
                            * (180.0f / (float)M_PI);
            new_el = soft_elevation_floor(new_el);

            /* Exponential smoothing */
            if (p->joc_smooth_valid) {
                p->joc_smooth_az[obj_idx]   = JOC_SMOOTH * p->joc_smooth_az[obj_idx]
                                             + (1.0f - JOC_SMOOTH) * new_az;
                p->joc_smooth_el[obj_idx]   = JOC_SMOOTH * p->joc_smooth_el[obj_idx]
                                             + (1.0f - JOC_SMOOTH) * new_el;
                p->joc_smooth_dist[obj_idx] = JOC_SMOOTH * p->joc_smooth_dist[obj_idx]
                                             + (1.0f - JOC_SMOOTH) * dist;
            } else {
                p->joc_smooth_az[obj_idx]   = new_az;
                p->joc_smooth_el[obj_idx]   = new_el;
                p->joc_smooth_dist[obj_idx] = dist;
            }

            float faz  = p->joc_smooth_az[obj_idx];
            float fel  = p->joc_smooth_el[obj_idx];
            float fdst = p->joc_smooth_dist[obj_idx];

            /* Only reload HRIR if position changed enough */
            float daz = fabsf(faz - p->speaker_pos[ch].azimuth);
            float del = fabsf(fel - p->speaker_pos[ch].elevation);
            if (daz > 2.0f || del > 2.0f) {
                p->speaker_pos[ch] = (HrtfSpeakerPos){faz, fel, fdst};
                update_channel_hrir(p, ch);
            }
        }
        p->joc_smooth_valid = 1;
        goto shared_state_done;
    }

    /* --- Phase 2c: truehd FFI per-object HRTF positioning --- */
    /* When the truehd Rust decoder is active, object channels (bed_count..N)
     * contain individually separated object audio.  Each object gets its own
     * HRTF at its OAMD position — the same architecture as Dolby's renderer. */
    {
        int bed_ch = p->num_bed_channels > 0 ? p->num_bed_channels : 8;
        int obj_count = total_dyn;
        /* Detect truehd objects: more channels than bed, objects have valid positions */
        HRTF_DBG("Phase2c check: obj_count=%d num_ch=%d bed_ch=%d objcoding=%d\n",
                 obj_count, p->num_channels, bed_ch, p->objcoding_active);
        if (obj_count > 0 && p->num_channels > bed_ch && p->sofa &&
            !p->objcoding_active) {
            const float OBJ_SMOOTH = compute_smooth_alpha(p->oamd_ramp_duration);
            float room_w = 6.5f, room_d = 5.0f, room_h = 2.7f;
            if (p->shared) {
                float rw = atomic_load_explicit(&p->shared->room_width, memory_order_relaxed);
                float rd = atomic_load_explicit(&p->shared->room_depth, memory_order_relaxed);
                float rh = atomic_load_explicit(&p->shared->room_height, memory_order_relaxed);
                if (rw > 0) room_w = rw;
                if (rd > 0) room_d = rd;
                if (rh > 0) room_h = rh;
            }

            for (int i = 0; i < obj_count && i < SPATIAL_EXT_MAX_OBJECTS; i++) {
                SpatialExtObjectPos *obj = &g_spatial_objmeta_ptr->objects[i];
                int ch = bed_ch + i;
                if (ch >= p->num_channels || ch >= HRTF_MAX_CHANNELS)
                    break;
                if (!obj->active || obj->gain_db <= -128)
                    continue;

                /* DAMF → room-relative cartesian → az/el/dist */
                float rx = obj->x * room_w * 0.5f;
                float ry = obj->y * room_d * 0.5f;
                float rz = obj->z * room_h * 0.5f;

                float dist = sqrtf(rx * rx + ry * ry + rz * rz);
                if (dist < 0.3f) dist = 0.3f;

                float new_az = atan2f(-rx, -ry) * (180.0f / (float)M_PI);
                float new_el = asinf(fminf(1.0f, fmaxf(-1.0f, rz / dist)))
                                * (180.0f / (float)M_PI);
                new_el = soft_elevation_floor(new_el);

                /* Exponential smoothing */
                if (p->joc_smooth_valid) {
                    p->joc_smooth_az[i]   = OBJ_SMOOTH * p->joc_smooth_az[i]
                                           + (1.0f - OBJ_SMOOTH) * new_az;
                    p->joc_smooth_el[i]   = OBJ_SMOOTH * p->joc_smooth_el[i]
                                           + (1.0f - OBJ_SMOOTH) * new_el;
                    p->joc_smooth_dist[i] = OBJ_SMOOTH * p->joc_smooth_dist[i]
                                           + (1.0f - OBJ_SMOOTH) * dist;
                } else {
                    p->joc_smooth_az[i]   = new_az;
                    p->joc_smooth_el[i]   = new_el;
                    p->joc_smooth_dist[i] = dist;
                }

                float faz  = p->joc_smooth_az[i];
                float fel  = p->joc_smooth_el[i];
                float fdst = p->joc_smooth_dist[i];

                /* Only reload HRIR if position changed significantly */
                float daz = fabsf(faz - p->speaker_pos[ch].azimuth);
                float del = fabsf(fel - p->speaker_pos[ch].elevation);
                if (daz > 2.0f || del > 2.0f) {
                    p->speaker_pos[ch] = (HrtfSpeakerPos){faz, fel, fdst};
                    update_channel_hrir(p, ch);
                    { static int p2c_log = 0;
                      if (++p2c_log <= 10)
                        HRTF_DBG("Phase2c: obj[%d] ch=%d az=%.1f el=%.1f dist=%.2f\n",
                                 i, ch, faz, fel, fdst);
                    }
                }
            }
            p->joc_smooth_valid = 1;
            goto shared_state_done;
        }
    }

    /* --- Phase 2b: TrueHD centroid positioning for height channels --- */

    if (p->num_height_channels <= 0 || !p->sofa)
        goto shared_state_done;

    /* Adaptive smoothing factor from OAMD ramp_duration.
     * Short ramp → low alpha (fast tracking for rapid objects).
     * Long/unknown ramp → high alpha (smooth slow movements). */
    const float SMOOTH = compute_smooth_alpha(p->oamd_ramp_duration);

    for (int h = 0; h < p->num_height_channels && h < HRTF_MAX_CHANNELS; h++) {
        int ch = p->height_ch_idx[h];
        if (ch < 0 || ch >= HRTF_MAX_CHANNELS)
            continue;

        /* Skip HRIR updates while height is fading out — let the
         * convolver ring out its overlap tail naturally. */
        if (p->height_fade_remaining > 0)
            continue;

        /* Determine target position for this height channel.
         * Use azimuth sign to decide: positive az = left speaker → use L centroid,
         * negative az = right speaker → use R centroid. */
        float orig_az = p->speaker_pos[ch].azimuth;
        int use_left = (orig_az >= 0.0f);

        float cx, cy, cz;
        float wg = use_left ? wg_L : wg_R;

        /* Per-side dominant: use the loudest object on this height speaker's
         * side for direct positioning.  Falls back to centroid when 5+ objects
         * are evenly spread or no per-side dominant exists. */
        int per_side_dom = use_left ? dominant_idx_L : dominant_idx_R;
        if (dominant_count <= 4 && dominant_count > 0 && per_side_dom >= 0) {
            SpatialExtObjectPos *dom = &g_spatial_objmeta_ptr->objects[per_side_dom];
            cx = dom->x;
            cy = dom->y;
            cz = dom->z;
        } else if (wg > 1e-6f) {
            /* Gain-weighted centroid */
            if (use_left) {
                cx = wx_L / wg_L;
                cy = wy_L / wg_L;
                cz = wz_L / wg_L;
            } else {
                cx = wx_R / wg_R;
                cy = wy_R / wg_R;
                cz = wz_R / wg_R;
            }
        } else {
            /* No active objects — keep current position (no update) */
            continue;
        }

        /* Convert DAMF centroid to room-relative cartesian → az/el/dist */
        float rx = cx * room_w * 0.5f;
        float ry = cy * room_d * 0.5f;
        float rz = cz * room_h * 0.5f;

        float dist = sqrtf(rx * rx + ry * ry + rz * rz);
        if (dist < 0.3f) dist = 0.3f;  /* minimum distance clamp */

        float new_az = atan2f(-rx, -ry) * (180.0f / (float)M_PI);
        float new_el = asinf(fminf(1.0f, fmaxf(-1.0f, rz / dist)))
                        * (180.0f / (float)M_PI);

        /* Ensure height objects maintain some minimum elevation so they
         * don't collapse into the ear-level bed plane. */
        new_el = soft_elevation_floor(new_el);

        /* Apply exponential smoothing */
        if (p->height_smooth_valid) {
            p->height_smooth_az[h]   = SMOOTH * p->height_smooth_az[h]
                                      + (1.0f - SMOOTH) * new_az;
            p->height_smooth_el[h]   = SMOOTH * p->height_smooth_el[h]
                                      + (1.0f - SMOOTH) * new_el;
            p->height_smooth_dist[h] = SMOOTH * p->height_smooth_dist[h]
                                      + (1.0f - SMOOTH) * dist;
        } else {
            p->height_smooth_az[h]   = new_az;
            p->height_smooth_el[h]   = new_el;
            p->height_smooth_dist[h] = dist;
        }

        float final_az   = p->height_smooth_az[h];
        float final_el   = p->height_smooth_el[h];
        float final_dist = p->height_smooth_dist[h];

        /* Only update HRIR if position changed significantly */
        float daz = fabsf(final_az - p->speaker_pos[ch].azimuth);
        float del = fabsf(final_el - p->speaker_pos[ch].elevation);
        float ddi = fabsf(final_dist - p->speaker_pos[ch].distance);
        if (daz > 2.0f || del > 2.0f || ddi > 0.1f) {
            p->speaker_pos[ch] = (HrtfSpeakerPos){final_az, final_el, final_dist};
            update_channel_hrir(p, ch);
        }
    }

    p->height_smooth_valid = 1;

shared_state_done:
    if (p->shared) {
        atomic_store_explicit(&p->shared->num_objects, total_dyn,
                              memory_order_relaxed);
    }
    update_min_dist(p);
}

static void update_min_dist(struct priv *p) {
    /* Only consider bed channels for the reference distance so that dynamic
     * object movements don't cause bed volume to fluctuate. */
    float md = 1e9f;
    int bed_end = p->num_bed_channels;
    if (bed_end > HRTF_MAX_CHANNELS) bed_end = HRTF_MAX_CHANNELS;
    for (int ch = 0; ch < bed_end; ch++) {
        float d = p->speaker_pos[ch].distance;
        if (d > 0 && d < md) md = d;
    }
    p->min_dist = md > 0 && md < 1e9f ? md : 1.0f;
}

// ---------------------------------------------------------------------------
// Filter initialization
// ---------------------------------------------------------------------------

static int init_hrtf(struct priv *p, int sample_rate, int num_channels) {
    HRTF_DBG("init_hrtf: %s rate=%d ch=%d (prev=%d)\n",
             p->initialized ? "REINIT" : "INIT", sample_rate,
             num_channels, p->num_channels);

    // Clean up previous state on reinit to avoid memory leaks
    if (p->initialized) {
        for (int ch = 0; ch < HRTF_MAX_CHANNELS; ch++) {
            for (int i = 0; i < 2; i++) {
                convolver_destroy(&p->channels[ch].left[i]);
                convolver_destroy(&p->channels[ch].right[i]);
            }
            free(p->input_accum[ch]);
            p->input_accum[ch] = NULL;
        }
        free(p->output_l); p->output_l = NULL;
        free(p->output_r); p->output_r = NULL;
        for (int i = 0; i < OBJCODING_MAX_OBJECTS; i++) {
            free(p->objcoding_obj_buf[i]);
            p->objcoding_obj_buf[i] = NULL;
        }
        reverb_destroy(&p->reverb);
        pconv_destroy(&p->ir_l);
        pconv_destroy(&p->ir_r);
        p->ir_loaded_path[0] = '\0';
        er_destroy(&p->er);
    }

    p->sample_rate = sample_rate;
    p->num_channels = num_channels;
    p->min_dist = 1.0f;
    p->out_limiter_gain = 1.0f;

    // Initialize convolver pairs
    for (int ch = 0; ch < num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
        HrtfChannelPair *pair = &p->channels[ch];
        pair->active_idx = 0;
        pair->crossfade_remaining = 0;
        pair->crossfade_total = HRTF_CROSSFADE_LEN;
        for (int i = 0; i < 2; i++) {
            convolver_init(&pair->left[i]);
            convolver_init(&pair->right[i]);
        }
    }

    // Allocate accumulation buffers
    for (int ch = 0; ch < num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
        p->input_accum[ch] = calloc(HRTF_BLOCK_SIZE, sizeof(float));
    }
    p->output_l = calloc(HRTF_BLOCK_SIZE, sizeof(float));
    p->output_r = calloc(HRTF_BLOCK_SIZE, sizeof(float));
    p->input_accum_pos = 0;

    // Set default speaker positions
    init_speaker_positions(p);

    // Initialize height channel tracking state
    p->num_height_channels = 0;
    p->height_smooth_valid = 0;
    p->height_mat_valid = 0;
    for (int i = 0; i < HRTF_MAX_CHANNELS; i++)
        p->height_mat_idx[i] = -1;

    // Resolve spatial coefficient global from avcodec DLL at runtime
#ifdef _WIN32
    if (!g_spatial_coeff_ptr) {
        HMODULE hAvcodec = GetModuleHandleA("avcodec-62.dll");
        if (hAvcodec) {
            g_spatial_coeff_ptr = (SpatialExtCoeff *)GetProcAddress(hAvcodec, "g_spatial_ext_coeff");
            if (g_spatial_coeff_ptr)
                HRTF_DBG("init_hrtf: resolved g_spatial_ext_coeff from avcodec-62.dll\n");
            else
                HRTF_DBG("init_hrtf: GetProcAddress failed for g_spatial_ext_coeff\n");

            g_spatial_objmeta_ptr = (SpatialExtObjMeta *)GetProcAddress(hAvcodec, "g_spatial_ext_objmeta");
            if (g_spatial_objmeta_ptr)
                HRTF_DBG("init_hrtf: resolved g_spatial_ext_objmeta from avcodec-62.dll\n");
            else
                HRTF_DBG("init_hrtf: GetProcAddress failed for g_spatial_ext_objmeta\n");

            g_objcoding_data_ptr = (ObjCodingMixData *)GetProcAddress(hAvcodec, "g_objcoding_data");
            if (g_objcoding_data_ptr)
                HRTF_DBG("init_hrtf: resolved g_objcoding_data from avcodec-62.dll\n");
            else
                HRTF_DBG("init_hrtf: GetProcAddress failed for g_objcoding_data\n");

        } else {
            HRTF_DBG("init_hrtf: avcodec-62.dll not loaded\n");
        }
    }
#endif

    // Determine bed vs object channels.
    // Standard lossless HD spatial: 8-channel 7.1 bed + up to 8 object channels.
    // If >8 channels, the extras are individual spatial objects.
    if (num_channels > 8) {
        p->num_bed_channels = 8;
        int num_obj = num_channels - 8;

        // Assign heuristic positions for object channels:
        // spread evenly around azimuth at 45deg elevation, 2m distance
        for (int i = 0; i < num_obj && (8 + i) < HRTF_MAX_CHANNELS; i++) {
            float az = -180.0f + 360.0f * (float)i / (float)num_obj;
            float el = 45.0f;
            float dist = 2.0f;
            p->speaker_pos[8 + i] = (HrtfSpeakerPos){az, el, dist};
            p->object_az[i] = az;
            p->object_el[i] = el;
            p->object_dist[i] = dist;
        }
    } else {
        p->num_bed_channels = num_channels;
    }

    // Load SOFA file
    const char *path = p->opts->sofa_path;
    if (!path || !path[0]) {
        // Try default location
        path = "assets/hrtf/default.sofa";
    }

    if (load_sofa(p, path) < 0) {
        // Filter will pass through without HRTF if no SOFA loaded
        return 0;
    }

    // Load HRIRs only for bed channels at init.
    // Object channels (ch >= bed) start with invalid convolvers (silent)
    // and get their HRIRs loaded by Phase 2c when OAMD positions arrive.
    // This prevents all objects from starting at position (0,0) which
    // causes comb filtering on the first frames.
    {
        int init_ch = p->num_bed_channels > 0 ? p->num_bed_channels : num_channels;
        if (init_ch > num_channels) init_ch = num_channels;
        for (int ch = 0; ch < init_ch && ch < HRTF_MAX_CHANNELS; ch++)
            update_channel_hrir_ex(p, ch, 0);
    }

    // Debug: count valid convolvers
    {
        int valid_count = 0;
        for (int ch = 0; ch < num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
            int a = p->channels[ch].active_idx;
            if (p->channels[ch].left[a].valid) valid_count++;
        }
        HRTF_DBG("init_hrtf: rate=%d ch=%d valid_convolvers=%d/%d sofa=%p hrir_len=%d\n",
                  sample_rate, num_channels, valid_count, num_channels, (void*)p->sofa, p->hrir_length);
    }

    // Initialize object coding reconstruction state (for DD+ spatial)
    p->objcoding_active = 0;
    p->objcoding_num_objects = 0;
    p->objcoding_num_channels = 0;
    for (int i = 0; i < 6; i++)
        objcoding_qmf_analysis_init(&p->objcoding_ana[i]);
    for (int i = 0; i < OBJCODING_MAX_OBJECTS; i++) {
        objcoding_qmf_synthesis_init(&p->objcoding_syn[i]);
        p->objcoding_obj_buf[i] = calloc(HRTF_BLOCK_SIZE, sizeof(float));
    }
    // Pre-initialize convolvers for object coding object channels (6..6+MAX_OBJECTS-1)
    // so that object metadata updates can safely call update_channel_hrir() before
    // the first object coding reconstruction block is processed.
    for (int i = 0; i < OBJCODING_MAX_OBJECTS && (num_channels + i) < HRTF_MAX_CHANNELS; i++) {
        int ch = num_channels + i;
        HrtfChannelPair *pair = &p->channels[ch];
        pair->active_idx = 0;
        pair->crossfade_remaining = 0;
        pair->crossfade_total = HRTF_CROSSFADE_LEN;
        for (int k = 0; k < 2; k++) {
            convolver_init(&pair->left[k]);
            convolver_init(&pair->right[k]);
        }
        p->speaker_pos[ch] = (HrtfSpeakerPos){0, 0, 2.0f};
    }
    p->joc_smooth_valid = 0;
    p->oamd_ramp_duration = 0;
    p->height_was_active = 0;
    p->height_fade_remaining = 0;
    memset(p->joc_smooth_az, 0, sizeof(p->joc_smooth_az));
    memset(p->joc_smooth_el, 0, sizeof(p->joc_smooth_el));
    memset(p->joc_smooth_dist, 0, sizeof(p->joc_smooth_dist));
    HRTF_DBG("init_hrtf: object coding reconstruction state initialized, g_objcoding_data_ptr=%p\n",
              (void*)g_objcoding_data_ptr);

    // Initialize reverb and sync initial params from shared state
    reverb_init(&p->reverb, sample_rate);
    pconv_init(&p->ir_l, HRTF_BLOCK_SIZE);
    pconv_init(&p->ir_r, HRTF_BLOCK_SIZE);
    p->ir_loaded_path[0] = '\0';
    memset(&p->hp_eq, 0, sizeof(p->hp_eq));
    p->hp_eq_loaded_path[0] = '\0';
    er_init(&p->er);
    // Precompute ambisonic-binaural decoder filters from current SOFA.
    // Steam Audio HRTFDatabase::precomputeAmbisonicsHRTFs() runs once per HRTF
    // load; we mirror that here.  Must come after load_sofa + er_init.
    compute_ambi_decoder_hrirs(p);
    if (p->shared) {
        float rv_decay = atomic_load_explicit(&p->shared->reverb_decay,
                                               memory_order_relaxed);
        float rv_damp  = atomic_load_explicit(&p->shared->reverb_damping,
                                               memory_order_relaxed);
        float rv_wet   = atomic_load_explicit(&p->shared->reverb_wet,
                                               memory_order_relaxed);
        float rv_pd    = atomic_load_explicit(&p->shared->reverb_predelay,
                                               memory_order_relaxed);
        reverb_update(&p->reverb, rv_decay, rv_damp, rv_wet, rv_pd, sample_rate);
        p->reverb.enabled = atomic_load_explicit(&p->shared->reverb_enabled,
                                                   memory_order_relaxed);

        // Sync initial room geometry for early reflections
        float rm_w = atomic_load_explicit(&p->shared->room_width,
                                           memory_order_relaxed);
        if (rm_w > 0) {
            float rm_d = atomic_load_explicit(&p->shared->room_depth,
                                               memory_order_relaxed);
            float rm_h = atomic_load_explicit(&p->shared->room_height,
                                               memory_order_relaxed);
            float rm_a = atomic_load_explicit(&p->shared->room_absorption,
                                               memory_order_relaxed);
            er_update(&p->er, rm_w, rm_d, rm_h, rm_a, sample_rate);
        }
    }

    // Compute minimum speaker distance for distance attenuation
    update_min_dist(p);

    p->initialized = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Process one block of audio
// ---------------------------------------------------------------------------

static void process_block(struct priv *p, float *channel_data[], int num_ch,
                          int num_samples, float *out_l, float *out_r) {
    memset(out_l, 0, num_samples * sizeof(float));
    memset(out_r, 0, num_samples * sizeof(float));

    /* Clamp to actual channels with audio.
     * With truehd FFI: bed (8) + objects (12) = 20 channels, all active.
     * With FFmpeg fallback: bed (8) + height (0-2) = 8-10, rest silent.
     * Check g_spatial_ext_objmeta for the true object count. */
    {
        int obj_from_meta = 0;
        if (g_spatial_objmeta_ptr)
            obj_from_meta = g_spatial_objmeta_ptr->num_dynamic_objects;
        int active_limit = p->num_bed_channels + p->num_height_channels;
        if (obj_from_meta > 0 && obj_from_meta > p->num_height_channels)
            active_limit = p->num_bed_channels + obj_from_meta;
        if (active_limit > 0 && active_limit < num_ch)
            num_ch = active_limit;
    }

    // Debug mute flags
    int mute_bed = p->shared ? atomic_load_explicit(&p->shared->mute_bed, memory_order_relaxed) : 0;
    int mute_obj = p->shared ? atomic_load_explicit(&p->shared->mute_objects, memory_order_relaxed) : 0;

    // Room gain: compensates for larger speaker distances in big rooms.
    // In real theaters, amplifiers are calibrated so SPL at the seat is
    // constant regardless of room size. This simulates that calibration.
    float room_gain = 1.0f;
    if (p->shared) {
        float rg = atomic_load_explicit(&p->shared->room_gain, memory_order_relaxed);
        if (rg > 0.0f) room_gain = rg;
    }

    float block_l[HRTF_BLOCK_SIZE];
    float block_r[HRTF_BLOCK_SIZE];

    int bed_count = p->num_bed_channels > 0 ? p->num_bed_channels : 8;

    // Height channel scaling is now handled in the decoder via output_shift.
    // Each AU applies its own bed_shift, avoiding timing mismatches.

    /* When JOC objects are active, the reconstructed objects already contain
     * all the audio from the 5.1 bed (extracted via mixing matrix inversion).
     * Processing both bed AND objects would double the audio with different
     * HRTF phases, causing comb filtering ("robotic" sound).
     * Solution: skip bed channels (except LFE=ch3) when JOC is active. */
    int joc_replaces_bed = (p->objcoding_active && p->objcoding_num_objects > 0
                            && num_ch > bed_count);

    /* TrueHD Atmos height channels (TFL/TFR) are ~75-80% remixed bed audio
     * (R²=0.73-0.81 linear regression against bed channels).  Processing
     * them through separate HRIRs alongside the bed creates comb filtering
     * that sounds robotic/metallic.
     *
     * Solution: SKIP height channels in the HRTF convolution.  The OAMD
     * object positions are used to dynamically adjust the bed channels'
     * HRTF elevation (in update_object_positions_from_objmeta), giving
     * overhead spatial cues WITHOUT comb filtering.
     *
     * The height channel energy is instead mixed into the bed channels
     * as a subtle overhead reinforcement: scale height content by a small
     * factor and add to the closest bed speaker pair (FL/FR for TFL/TFR). */
    /* When the truehd FFI decoder is active, object channels are clean
     * (individually separated, not remixed bed).  Process them at full
     * level through HRTF at their OAMD positions.
     * When using FFmpeg fallback, height channels are remixed bed and
     * must be skipped to avoid comb filtering. */
    int truehd_objects = (num_ch > bed_count && !joc_replaces_bed &&
                          g_spatial_objmeta_ptr &&
                          atomic_load(&g_spatial_objmeta_ptr->updated) >= 0 &&
                          g_spatial_objmeta_ptr->num_dynamic_objects > 0);
    int skip_height = (p->num_height_channels > 0 && !joc_replaces_bed && !truehd_objects);

    /* Objects: mix into stereo with simple panning instead of full HRTF
     * convolution.  Full HRTF on 12+ object channels causes clipping
     * artifacts from the overlap-add sum.  Simple panning is lightweight
     * and avoids the accumulation problem while preserving spatialization. */
    if (truehd_objects && bed_count > 0 && !mute_obj) {
        for (int ch = bed_count; ch < num_ch && ch < HRTF_MAX_CHANNELS; ch++) {
            if (mute_obj) break;
            int obj_idx = ch - bed_count;
            float obj_gain_lin = 1.0f;
            if (g_spatial_objmeta_ptr && obj_idx < SPATIAL_EXT_MAX_OBJECTS) {
                SpatialExtObjectPos *obj = &g_spatial_objmeta_ptr->objects[obj_idx];
                if (!obj->active || obj->gain_db <= -128) continue;
                if (obj->gain_db < 0)
                    obj_gain_lin = powf(10.0f, (float)obj->gain_db / 20.0f);
            }
            float az = p->speaker_pos[ch].azimuth;
            /* Sine/cosine pan law from azimuth */
            float pan = (az + 90.0f) / 180.0f;
            if (pan < 0.0f) pan = 0.0f;
            if (pan > 1.0f) pan = 1.0f;
            float g_l = sinf(pan * (float)M_PI * 0.5f) * obj_gain_lin * 0.3f;
            float g_r = cosf(pan * (float)M_PI * 0.5f) * obj_gain_lin * 0.3f;
            for (int i = 0; i < num_samples; i++) {
                out_l[i] += channel_data[ch][i] * g_l;
                out_r[i] += channel_data[ch][i] * g_r;
            }
        }
        /* Clamp num_ch to bed only for HRTF convolution below */
        num_ch = bed_count;
    }

    /* Mix height channel energy into corresponding bed channels.
     * TFL → FL (ch0), TFR → FR (ch1).  Use a subtle mix level (0.15)
     * to add overhead presence without introducing comb filtering.
     * This runs BEFORE HRTF convolution so the mixed content gets
     * the bed speaker's HRTF (whose elevation is dynamically adjusted
     * by OAMD), not a separate overhead HRTF. */
    if (skip_height) {
        for (int h = 0; h < p->num_height_channels && h < HRTF_MAX_CHANNELS; h++) {
            int hch = p->height_ch_idx[h];
            if (hch < 0 || hch >= num_ch) continue;
            /* Map height channel to closest bed channel:
             * positive azimuth (left) → FL (ch0)
             * negative azimuth (right) → FR (ch1) */
            float az = p->speaker_pos[hch].azimuth;
            int target_bed = (az >= 0.0f) ? 0 : 1;
            if (target_bed < num_ch) {
                float mix = 0.15f;
                for (int i = 0; i < num_samples; i++)
                    channel_data[target_bed][i] += channel_data[hch][i] * mix;
            }
        }
    }

    // Mono room-send bus for the wet processing stages (ambisonic ER + IR
    // convolution reverb).  Accumulated below (after dist attenuation + air
    // absorption) from every non-LFE source we actually render.  Contains
    // ONLY the distance weighting — each consumer applies its own level
    // (er_level / ir_wet) at its call site, so the bus can feed both paths.
    float mono_send[HRTF_BLOCK_SIZE];
    memset(mono_send, 0, sizeof(mono_send));
    float er_level = 0.0f;
    float ir_wet_level = 0.0f;
    if (p->shared) {
        er_level = atomic_load_explicit(&p->shared->er_level,
                                         memory_order_relaxed);
        ir_wet_level = atomic_load_explicit(&p->shared->ir_wet,
                                             memory_order_relaxed);
    }
    if (er_level < 0.0f) er_level = 0.0f;
    if (er_level > 1.0f) er_level = 1.0f;
    if (ir_wet_level < 0.0f) ir_wet_level = 0.0f;
    if (ir_wet_level > 1.0f) ir_wet_level = 1.0f;
    int need_mono_send = (er_level > 0.0f) ||
                         (ir_wet_level > 0.0f && p->ir_l.valid);

    for (int ch = 0; ch < num_ch && ch < HRTF_MAX_CHANNELS; ch++) {
        // Skip muted channel groups
        if (mute_bed && ch < bed_count) continue;
        if (mute_obj && ch >= bed_count) continue;

        // When JOC objects replace bed, skip bed channels (keep LFE=3)
        if (joc_replaces_bed && ch < bed_count && ch != 3) continue;

        // Skip TrueHD height channels — they cause comb filtering
        // because they contain remixed bed audio (see comment above).
        if (skip_height && ch >= bed_count && ch != 3) continue;

        HrtfChannelPair *pair = &p->channels[ch];
        int active = pair->active_idx;

        // Distance attenuation (relative 1/r law), compensated by room gain
        float dist = p->speaker_pos[ch].distance;
        if (dist < p->min_dist) dist = p->min_dist;
        float dist_gain = (p->min_dist / dist) * room_gain;

        // Apply OAMD gain for object channels.
        // Objects have individual gains from the Atmos metadata.
        // Without this, all objects play at full level causing clipping
        // when 12+ convolved signals are summed.
        if (ch >= bed_count && g_spatial_objmeta_ptr) {
            int obj_idx = ch - bed_count;
            if (obj_idx >= 0 && obj_idx < SPATIAL_EXT_MAX_OBJECTS) {
                SpatialExtObjectPos *obj = &g_spatial_objmeta_ptr->objects[obj_idx];
                if (!obj->active || obj->gain_db <= -128) {
                    // Muted object — skip entirely
                    continue;
                }
                if (obj->gain_db < 0) {
                    float obj_gain = powf(10.0f, (float)obj->gain_db / 20.0f);
                    dist_gain *= obj_gain;
                }
            }
        }

        // Smooth elevation attenuation — gradual rolloff replaces
        // the old hard ±20° cliff that caused audible "muting" when
        // objects crossed the threshold.  Unity below 10°, gentle
        // quadratic rolloff to -3 dB at 90°.
        {
            float el_abs = fabsf(p->speaker_pos[ch].elevation);
            if (el_abs > 10.0f) {
                float t = (el_abs - 10.0f) / 80.0f; // 0→1 over 10°→90°
                dist_gain *= 1.0f - 0.29f * t * t;   // 1.0→0.71 (-3 dB)
            }
        }

        /* Pre-convolution gain: combines distance attenuation with headroom.
         * Applying headroom BEFORE convolution (instead of after the sum)
         * prevents the HRIR's frequency-domain amplification (~2-3x at
         * pinna resonances) from pushing the convolution output above 1.0.
         * This eliminates the "vinyl crackle" micro-clipping artifact. */
        float pre_gain = dist_gain / sqrtf((float)(bed_count > 4 ? bed_count : 4));
        for (int i = 0; i < num_samples; i++)
            channel_data[ch][i] *= pre_gain;

        // Air absorption — one-pole lowpass (skip LFE)
        if (ch != 3) {
            float fc = 20000.0f / (1.0f + (dist - p->min_dist) * AIR_ABS_FACTOR);
            float coeff = 1.0f - expf(-2.0f * (float)M_PI * fc / (float)p->sample_rate);
            for (int i = 0; i < num_samples; i++) {
                p->air_abs_state[ch] = coeff * channel_data[ch][i]
                                      + (1.0f - coeff) * p->air_abs_state[ch];
                channel_data[ch][i] = p->air_abs_state[ch];
            }
        }

        // Screen baffling: perforated cinema screens absorb ~3 dB >2 kHz on
        // front speakers (FL=0, FR=1, FC=2).  Applied as one-pole LP mixed
        // 35% with dry for a gentle shelf.  Bypassed for LFE and surrounds.
        if (ch < 3 && p->shared &&
            atomic_load_explicit(&p->shared->screen_baffling,
                                  memory_order_relaxed)) {
            const float fc_shelf = 3500.0f;
            const float mix = 0.35f;
            float coeff = 1.0f - expf(-2.0f * (float)M_PI * fc_shelf /
                                       (float)p->sample_rate);
            for (int i = 0; i < num_samples; i++) {
                p->screen_baffle_state[ch] =
                    coeff * channel_data[ch][i] +
                    (1.0f - coeff) * p->screen_baffle_state[ch];
                channel_data[ch][i] =
                    channel_data[ch][i] * (1.0f - mix) +
                    p->screen_baffle_state[ch] * mix;
            }
        }

        // Frontal pinna boost: +4 dB peak at 4 kHz, -3 dB notch at 8 kHz on
        // FL/FR/FC.  Applied as two cascaded RBJ peaking-EQ biquads.  The
        // peak/notch pattern mimics the characteristic response of an ear
        // facing a frontal source, giving the brain a strong "this is front"
        // cue that generic HRTFs otherwise fail to provide.
        if (ch < 3 && p->shared &&
            atomic_load_explicit(&p->shared->front_pinna_boost,
                                  memory_order_relaxed)) {
            const float Fs = (float)p->sample_rate;
            // Biquad 1: peak, f=4kHz, Q=2.0, gain=+4 dB
            // Biquad 2: notch/cut, f=8kHz, Q=2.0, gain=-3 dB
            struct { float f; float Q; float gain_db; } eq[2] = {
                {4000.0f, 2.0f, +4.0f},
                {8000.0f, 2.0f, -3.0f},
            };
            float (*st[2])[4] = { p->pinna_peak_state, p->pinna_notch_state };

            for (int b = 0; b < 2; b++) {
                float A  = powf(10.0f, eq[b].gain_db / 40.0f);
                float w0 = 2.0f * (float)M_PI * eq[b].f / Fs;
                float cw = cosf(w0), sw = sinf(w0);
                float alpha = sw / (2.0f * eq[b].Q);
                // RBJ peaking EQ
                float b0 = 1.0f + alpha * A;
                float b1 = -2.0f * cw;
                float b2 = 1.0f - alpha * A;
                float a0 = 1.0f + alpha / A;
                float a1 = -2.0f * cw;
                float a2 = 1.0f - alpha / A;
                float ib0 = b0 / a0, ib1 = b1 / a0, ib2 = b2 / a0;
                float ia1 = a1 / a0, ia2 = a2 / a0;
                float *s = st[b][ch];
                for (int i = 0; i < num_samples; i++) {
                    float x = channel_data[ch][i];
                    float y = ib0 * x + ib1 * s[0] + ib2 * s[1]
                            - ia1 * s[2] - ia2 * s[3];
                    s[1] = s[0]; s[0] = x;
                    s[3] = s[2]; s[2] = y;
                    channel_data[ch][i] = y;
                }
            }
        }

        // LFE: process through HRTF like any other speaker.
        // In a real cinema the subwoofer has a physical position
        // (typically front-center, below screen, at -30° elevation).
        // Processing it through HRTF contributes to room immersion.

        // Room-send accumulation for ambisonic ER (skip LFE — subwoofers
        // don't excite high-frequency reflections usefully).
        //
        // Distance-weighted send.  The physical direct/reverb ratio follows
        // `direct ∝ 1/dist` while the reverberant field is approximately
        // constant throughout the room, so the WET send rises with distance:
        //     wet(dist) = dist / (dist + τ)
        // where τ is the critical distance (direct == reverb) — around 3 m
        // for a typical cinema/home-theatre geometry.  That gives:
        //     dist = 0   → 0     (pure dry, source at the head)
        //     dist = 1 m → 0.25
        //     dist = 3 m → 0.5   (half-wet)
        //     dist = 10 m → 0.77
        //     dist → ∞   → 1     (fully wet, ambient bed)
        // Combined with the direct path's own 1/dist attenuation, this is
        // the cue a listener uses to judge depth — a close whisper stays
        // dry and forward, a distant rumble washes out into the room.
        if (ch != 3 && need_mono_send) {
            const float tau = 3.0f;
            float dist_send = dist / (dist + tau);
            if (dist_send > 0.0f) {
                for (int i = 0; i < num_samples; i++)
                    mono_send[i] += channel_data[ch][i] * dist_send;
            }
        }

        if (!pair->left[active].valid)
            continue;

        // Consistent binaural path: every non-LFE source uses the same
        // HRIR renderer, whether it comes from bed speakers, height feeds,
        // or reconstructed object channels.
        convolver_process(&pair->left[active], channel_data[ch], block_l);
        convolver_process(&pair->right[active], channel_data[ch], block_r);

        // Handle crossfade if HRIR was recently changed
        if (pair->crossfade_remaining > 0) {
            int prev = 1 - active;
            if (pair->left[prev].valid) {
                float prev_l[HRTF_BLOCK_SIZE], prev_r[HRTF_BLOCK_SIZE];
                convolver_process(&pair->left[prev], channel_data[ch], prev_l);
                convolver_process(&pair->right[prev], channel_data[ch], prev_r);

                for (int i = 0; i < num_samples; i++) {
                    if (pair->crossfade_remaining > 0) {
                        // Equal-power crossfade: maintains constant energy
                        // even when old and new HRIRs are uncorrelated,
                        // preventing the audible dip of a linear fade.
                        int xf_total = pair->crossfade_total > 0 ? pair->crossfade_total : HRTF_CROSSFADE_LEN;
                        float phase = (1.0f - (float)pair->crossfade_remaining / (float)xf_total)
                                      * ((float)M_PI * 0.5f);
                        float g_new = sinf(phase);
                        float g_old = cosf(phase);
                        block_l[i] = block_l[i] * g_new + prev_l[i] * g_old;
                        block_r[i] = block_r[i] * g_new + prev_r[i] * g_old;
                        pair->crossfade_remaining--;
                    }
                }
            } else {
                pair->crossfade_remaining = 0;
            }
        }

        /* Height channel degraded fade: smoothly fade out height channels
         * when the decoder enters degraded mode (no spatial rematrix),
         * and fade in when it recovers.  Prevents overlap-add tail
         * discontinuities from abrupt height silence/restoration. */
        if (ch >= bed_count && p->height_fade_remaining != 0) {
            for (int i = 0; i < num_samples; i++) {
                if (p->height_fade_remaining > 0) {
                    /* Fade-out: 1→0 over HEIGHT_FADE_LEN samples */
                    float fade = (float)p->height_fade_remaining / (float)HEIGHT_FADE_LEN;
                    block_l[i] *= fade;
                    block_r[i] *= fade;
                    p->height_fade_remaining--;
                } else if (p->height_fade_remaining < 0) {
                    /* Fade-in: 0→1 over HEIGHT_FADE_LEN samples */
                    float fade = 1.0f - (float)(-p->height_fade_remaining) / (float)HEIGHT_FADE_LEN;
                    block_l[i] *= fade;
                    block_r[i] *= fade;
                    p->height_fade_remaining++;
                }
            }
        }

        // Near-field compensation.  For sources within ~1.5 m we apply a
        // per-ear low-shelf boost on the post-HRTF channel signal.  The
        // boost amount is weighted by azimuth so the proximal ear gets
        // most of the lift (frontal sources get a smaller bilateral boost).
        // Generic HRTFs measured at 1.5–2 m miss the LF coupling that
        // very close sources produce; this brings it back so close objects
        // feel "right at the head" instead of "moderately distant".
        if (ch != 3 && p->shared &&
            atomic_load_explicit(&p->shared->near_field_comp,
                                  memory_order_relaxed)) {
            const float ref_dist     = 1.5f;
            const float max_boost_db = 4.0f;
            const float fc           = 700.0f;
            if (dist < ref_dist) {
                float prox = (ref_dist - dist) / ref_dist;          // 0..1
                if (prox > 1.0f) prox = 1.0f;
                if (prox < 0.0f) prox = 0.0f;
                float az_rad = p->speaker_pos[ch].azimuth *
                               (float)(M_PI / 180.0);
                float sa = sinf(az_rad);
                float w_l = prox * 0.5f * (1.0f + sa);
                float w_r = prox * 0.5f * (1.0f - sa);
                float boost_lin = powf(10.0f, max_boost_db / 20.0f);
                float gm1_l = (boost_lin - 1.0f) * w_l;
                float gm1_r = (boost_lin - 1.0f) * w_r;
                float coeff = 1.0f - expf(-2.0f * (float)M_PI * fc /
                                           (float)p->sample_rate);
                for (int i = 0; i < num_samples; i++) {
                    p->nf_lp_state_l[ch] =
                        (1.0f - coeff) * p->nf_lp_state_l[ch] +
                        coeff * block_l[i];
                    block_l[i] += gm1_l * p->nf_lp_state_l[ch];
                    p->nf_lp_state_r[ch] =
                        (1.0f - coeff) * p->nf_lp_state_r[ch] +
                        coeff * block_r[i];
                    block_r[i] += gm1_r * p->nf_lp_state_r[ch];
                }
            }
        }

        // Accumulate into output
        for (int i = 0; i < num_samples; i++) {
            out_l[i] += block_l[i];
            out_r[i] += block_r[i];
        }
    }

    /* Headroom is now applied pre-convolution (in the per-channel loop above)
     * so no post-sum attenuation is needed.  This prevents HRIR frequency
     * amplification from causing internal clipping during the convolution. */

    // Ambisonic spatialised early reflections.  Adds its binaural output
    // directly into out_l/out_r so the downstream crossfeed + limiter see
    // the combined direct+reflected signal — matching the way Steam Audio
    // sums AmbisonicsBinauralEffect into the binaural mix.
    if (er_level > 0.0f && p->er.initialized && p->er.decoder_valid
        && p->er.num_taps > 0) {
        float er_in[HRTF_BLOCK_SIZE];
        for (int i = 0; i < num_samples; i++)
            er_in[i] = mono_send[i] * er_level;
        er_process_ambi(&p->er, er_in, out_l, out_r, num_samples);
    }

    // IR convolution reverb (late tail from a real-room impulse response).
    // Reuses the same distance-weighted mono send as the ER, scaled by
    // ir_wet.  Stereo output (one convolver per ear) mixes directly into
    // out_l/out_r.
    if (ir_wet_level > 0.0f && p->ir_l.valid && p->ir_r.valid) {
        float ir_in[HRTF_BLOCK_SIZE];
        float ir_out_l[HRTF_BLOCK_SIZE];
        float ir_out_r[HRTF_BLOCK_SIZE];
        for (int i = 0; i < num_samples; i++)
            ir_in[i] = mono_send[i] * ir_wet_level;
        pconv_process(&p->ir_l, ir_in, ir_out_l);
        pconv_process(&p->ir_r, ir_in, ir_out_r);
        for (int i = 0; i < num_samples; i++) {
            out_l[i] += ir_out_l[i];
            out_r[i] += ir_out_r[i];
        }
    }
}

// ---------------------------------------------------------------------------
// object coding reconstruction: 5.1 downmix → individual object audio via QMF
// ---------------------------------------------------------------------------

/*
 * When DD+ spatial content is playing, the decoder outputs 5.1 PCM and the
 * object coding mixing matrix tells us how each object was mixed into those channels.
 * We reconstruct objects in QMF domain:
 *
 *   For each timeslot (64 samples):
 *     1. QMF analyze each bed channel → qmf_in[ch][sb] (complex)
 *     2. For each object:
 *          qmf_obj[sb] = sum_ch( qmf_in[ch][sb] * mix_mtx[obj][ts][ch][sb] )
 *     3. QMF synthesize → object PCM
 *
 * Input: 256 samples of 5.1 (6ch) in accum_ptrs[0..5]
 * Output: objcoding_obj_buf[0..N-1] with 256 samples each
 * Returns the number of object coding objects reconstructed (0 if object coding not active)
 *
 * Channel mapping for 5.x: L=0, R=1, C=2, LFE=3, Ls=4, Rs=5
 * object coding channels (excl LFE): L=0, R=1, C=2, Ls=3, Rs=4
 * So object coding ch 0..2 map to PCM ch 0..2, object coding ch 3..4 map to PCM ch 4..5
 */
static int objcoding_reconstruct_objects(struct priv *p, float *bed_data[],
                                   int bed_channels)
{
    if (!g_objcoding_data_ptr)
        return 0;

    /* Detect new object coding frame: atomically read and clear the updated flag.
     * New frame → reset timeslot offset to start of frame. */
    if (atomic_exchange(&g_objcoding_data_ptr->updated, 0)) {
        p->objcoding_ts_offset = 0;
        p->objcoding_active = 1;
    }

    if (!p->objcoding_active)
        return 0;

    int num_objects = g_objcoding_data_ptr->num_objects;
    int objcoding_channels = g_objcoding_data_ptr->num_channels;
    int num_timeslots = g_objcoding_data_ptr->num_timeslots;

    if (num_objects <= 0 || num_objects > OBJCODING_MAX_OBJECTS)
        return 0;
    if (objcoding_channels < 5 || objcoding_channels > 7)
        return 0;
    if (bed_channels < 6)
        return 0;

    /* Measure bed energy (excl LFE) BEFORE reconstruction.
     * We'll normalize object energy to match this, ensuring the binaural
     * output level is comparable to standard 5.1 HRTF rendering. */
    float bed_energy = 0;
    for (int ch = 0; ch < bed_channels && ch < 6; ch++) {
        if (ch == 3) continue; /* skip LFE */
        for (int i = 0; i < HRTF_BLOCK_SIZE; i++)
            bed_energy += bed_data[ch][i] * bed_data[ch][i];
    }

    /* Map object coding channel index to PCM channel index (skip LFE=3) */
    int objcoding_to_pcm[7] = {0, 1, 2, 4, 5, -1, -1}; /* 5.x default */
    if (objcoding_channels == 7 && bed_channels >= 8) {
        objcoding_to_pcm[5] = 6; /* Lb */
        objcoding_to_pcm[6] = 7; /* Rb */
    }

    /* Timeslot tracking: a DD+ frame has 24 timeslots (1536 samples / 64).
     * Each HRTF block (256 samples) consumes 4 timeslots. We advance
     * through the frame across successive blocks, using the correct
     * temporal portion of the mix matrix for each block. */
    int ts_per_block = HRTF_BLOCK_SIZE / OBJCODING_QMF_N; /* 256/64 = 4 */
    if (ts_per_block > num_timeslots)
        ts_per_block = num_timeslots;

    int ts_base = p->objcoding_ts_offset;
    if (ts_base + ts_per_block > num_timeslots)
        ts_base = num_timeslots - ts_per_block; /* clamp to last valid range */
    if (ts_base < 0) ts_base = 0;

    /* Clear object output buffers */
    for (int obj = 0; obj < num_objects; obj++)
        memset(p->objcoding_obj_buf[obj], 0, HRTF_BLOCK_SIZE * sizeof(float));

    /* Process ts_per_block timeslots of 64 samples each */
    for (int ts = 0; ts < ts_per_block; ts++) {
        int sample_off = ts * OBJCODING_QMF_N;
        int mtx_ts = ts_base + ts; /* index into the 24-timeslot mix matrix */

        /* QMF analysis of each bed channel (excl LFE) */
        float qmf_re[7][OBJCODING_QMF_N];
        float qmf_im[7][OBJCODING_QMF_N];

        for (int jch = 0; jch < objcoding_channels; jch++) {
            int pch = objcoding_to_pcm[jch];
            if (pch < 0 || pch >= bed_channels) {
                memset(qmf_re[jch], 0, sizeof(qmf_re[jch]));
                memset(qmf_im[jch], 0, sizeof(qmf_im[jch]));
                continue;
            }
            objcoding_qmf_analyze(&p->objcoding_ana[jch], bed_data[pch] + sample_off,
                             qmf_re[jch], qmf_im[jch]);
        }

        /* For each object: apply mix matrix in QMF domain */
        for (int obj = 0; obj < num_objects; obj++) {
            float obj_re[OBJCODING_QMF_N] = {0};
            float obj_im[OBJCODING_QMF_N] = {0};

            for (int sb = 0; sb < OBJCODING_QMF_N; sb++) {
                float sum_re = 0, sum_im = 0;
                for (int jch = 0; jch < objcoding_channels; jch++) {
                    float coeff = g_objcoding_data_ptr->mix_mtx[obj][mtx_ts][jch][sb];
                    sum_re += qmf_re[jch][sb] * coeff;
                    sum_im += qmf_im[jch][sb] * coeff;
                }
                obj_re[sb] = sum_re;
                obj_im[sb] = sum_im;
            }

            /* QMF synthesis → PCM for this object */
            float pcm_out[OBJCODING_QMF_N];
            objcoding_qmf_synthesize(&p->objcoding_syn[obj], obj_re, obj_im, pcm_out);
            memcpy(p->objcoding_obj_buf[obj] + sample_off, pcm_out,
                   OBJCODING_QMF_N * sizeof(float));
        }
    }

    /* Advance timeslot offset for next block within this DD+ frame */
    p->objcoding_ts_offset += ts_per_block;

    /* Energy normalization with smoothing: scale all objects so their
     * combined energy matches the original 5.1 bed energy (excl LFE).
     *
     * Uses exponential smoothing (~50ms time constant) to avoid choppy
     * gain changes from block-to-block energy fluctuations. During silent
     * passages (low bed or object energy), the previous scale factor is
     * held to maintain consistent output level. */
    float obj_energy = 0;
    for (int obj = 0; obj < num_objects; obj++) {
        for (int i = 0; i < HRTF_BLOCK_SIZE; i++)
            obj_energy += p->objcoding_obj_buf[obj][i] * p->objcoding_obj_buf[obj][i];
    }

    /* Update smoothed scale factor when both energies are meaningful */
    if (obj_energy > 1e-10f && bed_energy > 1e-10f) {
        float target_scale = sqrtf(bed_energy / obj_energy);
        /* Clamp to reasonable range to avoid extreme values from transients */
        if (target_scale > 10.0f) target_scale = 10.0f;
        if (target_scale < 0.01f) target_scale = 0.01f;

        if (p->objcoding_norm_scale <= 0.0f)
            p->objcoding_norm_scale = target_scale;  /* first block: instant */
        else {
            /* alpha=0.1 → ~10 blocks (~53ms at 48kHz) time constant */
            p->objcoding_norm_scale += 0.1f * (target_scale - p->objcoding_norm_scale);
        }
    }

    /* Always apply the smoothed scale (persists through silent passages) */
    if (p->objcoding_norm_scale > 0.0f) {
        for (int obj = 0; obj < num_objects; obj++)
            for (int i = 0; i < HRTF_BLOCK_SIZE; i++)
                p->objcoding_obj_buf[obj][i] *= p->objcoding_norm_scale;
    }

    p->objcoding_num_objects = num_objects;
    p->objcoding_num_channels = objcoding_channels;

    /* Debug: log first few object coding reconstructions */
    {
        static int objcoding_recon_logged = 0;
        if (objcoding_recon_logged < 10 && (bed_energy > 1e-10f || obj_energy > 1e-10f)) {
            HRTF_DBG("objcoding[%d]: %d obj, ts=%d, bed_e=%.4f, obj_e=%.4f, scale=%.4f\n",
                      objcoding_recon_logged, num_objects, ts_base,
                      bed_energy, obj_energy, p->objcoding_norm_scale);
            objcoding_recon_logged++;
        }
    }

    return num_objects;
}

// ---------------------------------------------------------------------------
// mpv filter interface
// ---------------------------------------------------------------------------

static void af_hrtf_process(struct mp_filter *f) {
    struct priv *p = f->priv;

    if (!mp_pin_can_transfer_data(f->ppins[1], p->in_pin))
        return;

    struct mp_frame frame = mp_pin_out_read(p->in_pin);

    if (frame.type == MP_FRAME_EOF) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    if (frame.type != MP_FRAME_AUDIO)
        goto error;

    struct mp_aframe *in = frame.data;

    int in_rate = mp_aframe_get_rate(in);
    struct mp_chmap in_chmap = {0};
    mp_aframe_get_chmap(in, &in_chmap);
    int in_channels = in_chmap.num;
    int in_samples = mp_aframe_get_size(in);

    // Initialize on first frame or format change
    if (!p->initialized || p->sample_rate != in_rate ||
        p->num_channels != in_channels) {
        init_hrtf(p, in_rate, in_channels);
        // Override default speaker positions with actual channel layout.
        // Apply chmap first for ALL channels (including height/objects),
        // then override bed channels with bed_mask (authoritative).
        // Positional mapping.  FFmpeg's mp_chmap is the authoritative source
        // for the layout of the frame we're processing — it tells us which
        // speaker ID is at each channel index of the actual PCM data.  The
        // Atmos decoder also publishes a bed_mask, but that describes the
        // BED SUBSTREAM (a TrueHD internal layout), which may be narrower
        // than the 16-channel frame we actually receive when the stream has
        // FLC/FRC or extended channels.  Applying bed_mask unconditionally
        // used to override chmap with WRONG positions for streams where
        // num_channels > popcount(bed_mask): e.g. ch6 gets audio the frame
        // labels FLC but bed_mask would place SL there.
        //
        // Strategy: trust chmap when it's available and fully-known.  Only
        // fall back to bed_mask if chmap is empty / mostly unknown.
        bool chmap_ok = false;
        if (in_chmap.num > 0) {
            init_speaker_positions_from_chmap(p, &in_chmap);
            int known = 0;
            for (int i = 0; i < in_chmap.num; i++)
                if (is_known_speaker(in_chmap.speaker[i])) known++;
            // Consider chmap trustworthy if at least ~75% of channels map to
            // a known speaker ID.  Leaves room for a couple of exotic IDs.
            chmap_ok = (known * 4 >= in_chmap.num * 3);
        }
        if (!chmap_ok && g_spatial_coeff_ptr && g_spatial_coeff_ptr->bed_mask) {
            int bed_count = init_speaker_positions_from_bed_mask(p, g_spatial_coeff_ptr->bed_mask);
            p->num_bed_channels = bed_count;
        }
        // Dolby/DCI content uses SMPTE channel order but FFmpeg exposes it
        // in WAVE/bit order without remapping the audio samples.  Swap the
        // SL↔BL and SR↔BR positions so the audio lands at the intended
        // direction.  Gated by a runtime flag (default ON) so users with
        // raw WAVE-ordered 7.1 files can turn it off.
        if (p->shared && atomic_load_explicit(&p->shared->channel_order_smpte,
                                               memory_order_relaxed)) {
            apply_smpte_channel_order(p);
        }
        // Sync speaker positions to shared state so the UI visualizer
        // shows the correct layout (including height channels for 7.1.4)
        if (p->shared) {
            for (int ch = 0; ch < in_channels && ch < HRTF_SS_MAX_CHANNELS; ch++)
                p->shared->speaker_pos[ch] = p->speaker_pos[ch];
        }

        // Reload HRIRs with correct positions (direct set, no crossfade)
        if (p->sofa) {
            for (int ch = 0; ch < in_channels && ch < HRTF_MAX_CHANNELS; ch++)
                update_channel_hrir_ex(p, ch, 0);
            update_min_dist(p);
        }
        p->output_avail = 0;
        p->output_read_pos = 0;
    }

    if (!p->sofa) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    // -----------------------------------------------------------------
    // Shared state synchronization (before processing)
    // -----------------------------------------------------------------
    float master_vol = 1.0f;
    if (p->shared) {
        atomic_store_explicit(&p->shared->active, 1, memory_order_relaxed);
        atomic_store_explicit(&p->shared->sample_rate, (int32_t)in_rate,
                              memory_order_relaxed);
        atomic_store_explicit(&p->shared->num_channels, (int32_t)in_channels,
                              memory_order_relaxed);
        atomic_store_explicit(&p->shared->num_bed_channels,
                              (int32_t)((p->num_bed_channels > 0 && p->num_bed_channels <= in_channels)
                                      ? p->num_bed_channels
                                      : ((in_channels > 8) ? 8 : in_channels)),
                              memory_order_relaxed);

        // Write current PTS so host app can sync sidecar metadata
        double pts = mp_aframe_get_pts(in);
        if (pts != MP_NOPTS_VALUE)
            atomic_store_explicit(&p->shared->current_pts, pts,
                                  memory_order_relaxed);

        if (atomic_load_explicit(&p->shared->speaker_pos_changed,
                                  memory_order_relaxed)) {
            for (int ch = 0; ch < p->num_channels && ch < HRTF_SS_MAX_CHANNELS; ch++) {
                p->speaker_pos[ch] = p->shared->speaker_pos[ch];
                update_channel_hrir(p, ch);
            }
            update_min_dist(p);
            atomic_store_explicit(&p->shared->speaker_pos_changed, 0,
                                  memory_order_relaxed);
        }

        // Sync spatial object positions from host app (sidecar loader)
        if (atomic_load_explicit(&p->shared->objects_changed,
                                  memory_order_relaxed)) {
            int num_obj = atomic_load_explicit(&p->shared->num_objects,
                                                memory_order_relaxed);
            int bed_count = p->num_bed_channels > 0 ? p->num_bed_channels : 8;
            if (bed_count < 0) bed_count = 0;
            if (bed_count > HRTF_MAX_CHANNELS) bed_count = HRTF_MAX_CHANNELS;
            if (num_obj > HRTF_MAX_CHANNELS - bed_count)
                num_obj = HRTF_MAX_CHANNELS - bed_count;

            float room_w = atomic_load_explicit(&p->shared->room_width,
                                                 memory_order_relaxed);
            float room_d = atomic_load_explicit(&p->shared->room_depth,
                                                 memory_order_relaxed);
            float room_h = atomic_load_explicit(&p->shared->room_height,
                                                 memory_order_relaxed);
            if (room_w <= 0) room_w = 6.5f;
            if (room_d <= 0) room_d = 5.0f;
            if (room_h <= 0) room_h = 2.7f;

            for (int i = 0; i < num_obj; i++) {
                int ch = bed_count + i;
                if (ch >= HRTF_MAX_CHANNELS) break;

                float ox = atomic_load_explicit(&p->shared->object_x[i],
                                                 memory_order_relaxed);
                float oy = atomic_load_explicit(&p->shared->object_y[i],
                                                 memory_order_relaxed);
                float oz = atomic_load_explicit(&p->shared->object_z[i],
                                                 memory_order_relaxed);

                // Convert spatial coords to room-relative cartesian:
                // X: 0=L, 1=R -> centered: (ox-0.5)*width, >0=right
                // Y: 0=front, 1=back -> (1-oy)*depth, >0=in front of listener
                // Z: -1=floor, 1=ceiling -> oz*height*0.5
                float rx = (ox - 0.5f) * room_w;
                float ry = (1.0f - oy) * room_d;
                float rz = oz * room_h * 0.5f;

                float dist = sqrtf(rx * rx + ry * ry + rz * rz);
                if (dist < 0.01f) dist = 0.01f;
                // Negate rx: right (rx>0) → negative azimuth (our convention)
                float az = atan2f(-rx, ry) * (180.0f / (float)M_PI);
                float el = asinf(rz / dist) * (180.0f / (float)M_PI);

                // Threshold: only reload HRIR if position changed significantly
                float daz = fabsf(az - p->object_az[i]);
                float del = fabsf(el - p->object_el[i]);
                float ddi = fabsf(dist - p->object_dist[i]);
                if (daz > 5.0f || del > 5.0f || ddi > 0.2f) {
                    p->object_az[i] = az;
                    p->object_el[i] = el;
                    p->object_dist[i] = dist;
                    p->speaker_pos[ch] = (HrtfSpeakerPos){az, el, dist};
                    update_channel_hrir(p, ch);
                }
            }
            update_min_dist(p);
            atomic_store_explicit(&p->shared->objects_changed, 0,
                                  memory_order_relaxed);
        }

        // Real-time object position estimation from decoder coefficients.
        // This runs every frame and overrides/supplements sidecar positions.
        if (p->num_channels > 8 && p->sofa)
            update_object_positions_from_coefficients(p);

        // Real-time object metadata object positions from spatial decoder (lossless HD or DD+).
        // Overrides sidecar/coefficient positions with true metadata.
        // For DD+ spatial (5.1), only SharedState visualization is updated.
        if (p->sofa)
            update_object_positions_from_objmeta(p);

        if (atomic_load_explicit(&p->shared->sofa_path_changed,
                                  memory_order_relaxed)) {
            if (p->shared->sofa_path[0]) {
                load_sofa(p, p->shared->sofa_path);
                for (int ch = 0; ch < p->num_channels && ch < HRTF_MAX_CHANNELS; ch++)
                    update_channel_hrir_ex(p, ch, 0);
                update_min_dist(p);
                // Rebuild SH-HRIR decoder for the new SOFA.
                compute_ambi_decoder_hrirs(p);
            }
            atomic_store_explicit(&p->shared->sofa_path_changed, 0,
                                  memory_order_relaxed);
        }

        // Hot-reload convolution-reverb IR when the UI changes the path.
        if (atomic_load_explicit(&p->shared->ir_changed,
                                  memory_order_relaxed)) {
            const char *newp = p->shared->ir_file_path;
            if (newp[0] == '\0') {
                // User cleared the selection — silence both convolvers.
                p->ir_l.valid = 0;
                p->ir_r.valid = 0;
                p->ir_loaded_path[0] = '\0';
            } else if (strncmp(newp, p->ir_loaded_path,
                                sizeof(p->ir_loaded_path)) != 0) {
                float *ch[4] = {NULL, NULL, NULL, NULL};
                int n_ch = 0, len = 0;
                int rc = wav_load_n(newp, p->sample_rate, ch, &n_ch, &len);
                if (rc == 0 && len > 0) {
                    int basis_len = p->hrir_length;
                    int total_len = (n_ch == 4 && p->er.decoder_valid)
                                      ? len + basis_len - 1
                                      : len;

                    float *bin_l = (float *)calloc((size_t)total_len, sizeof(float));
                    float *bin_r = (float *)calloc((size_t)total_len, sizeof(float));

                    if (n_ch == 4 && p->er.decoder_valid) {
                        // Ambisonic decode via SH-HRIR basis.  IR is FuMa
                        // (W, X, Y, Z); our basis is ACN (W, Y, Z, X) with
                        // N3D normalisation.  FuMa→N3D scales: W*=sqrt(2),
                        // X/Y/Z*=sqrt(3).
                        const int  ir_to_basis[4] = { 0, 3, 1, 2 };
                        const float scale[4] = {
                            1.41421356f, 1.73205081f,
                            1.73205081f, 1.73205081f
                        };
                        for (int i = 0; i < 4; i++) {
                            int bi = ir_to_basis[i];
                            offline_convolve(ch[i], len,
                                              p->er.dec_l[bi].hrir_td, basis_len,
                                              scale[i], bin_l);
                            offline_convolve(ch[i], len,
                                              p->er.dec_r[bi].hrir_td, basis_len,
                                              scale[i], bin_r);
                        }
                        // Normalise post-decode peak to ~0.9 so the
                        // convolution stage doesn't clip.
                        float peak = 0.0f;
                        for (int k = 0; k < total_len; k++) {
                            float a = fabsf(bin_l[k]); if (a > peak) peak = a;
                            float b = fabsf(bin_r[k]); if (b > peak) peak = b;
                        }
                        if (peak > 1e-9f) {
                            float g = 0.9f / peak;
                            for (int k = 0; k < total_len; k++) {
                                bin_l[k] *= g; bin_r[k] *= g;
                            }
                        }
                        fprintf(stderr,
                                "[af_hrtf] IR loaded (B-format ambisonic decode): "
                                "'%s' (%d samples → %d after SH-HRIR conv, %.2f s)\n",
                                newp, len, total_len,
                                (float)total_len / (float)p->sample_rate);
                    } else {
                        // Stereo / mono fall-back: ch[0] = L, ch[1] = R (or
                        // mono into both).  Same behaviour as the previous
                        // wav_load_stereo path.
                        for (int k = 0; k < len; k++) {
                            bin_l[k] = ch[0][k];
                            bin_r[k] = (n_ch >= 2) ? ch[1][k] : ch[0][k];
                        }
                        fprintf(stderr,
                                "[af_hrtf] IR loaded: '%s' (%d ch, %d samples, "
                                "%.2f s)\n",
                                newp, n_ch, len,
                                (float)len / (float)p->sample_rate);
                    }

                    pconv_load(&p->ir_l, bin_l, total_len);
                    pconv_load(&p->ir_r, bin_r, total_len);
                    strncpy(p->ir_loaded_path, newp,
                            sizeof(p->ir_loaded_path) - 1);
                    p->ir_loaded_path[sizeof(p->ir_loaded_path) - 1] = '\0';
                    free(bin_l); free(bin_r);
                } else {
                    fprintf(stderr,
                            "[af_hrtf] IR load FAILED: '%s' rc=%d "
                            "(need 1/2/4-channel 16/24/32-bit PCM or "
                            "float32 WAV at %d Hz)\n",
                            newp, rc, p->sample_rate);
                    p->ir_l.valid = 0;
                    p->ir_r.valid = 0;
                    p->ir_loaded_path[0] = '\0';
                }
                for (int i = 0; i < 4; i++) free(ch[i]);
            }
            atomic_store_explicit(&p->shared->ir_changed, 0,
                                  memory_order_relaxed);
        }

        // Hot-reload headphone-EQ profile when the UI changes the path.
        if (atomic_load_explicit(&p->shared->hp_eq_changed,
                                  memory_order_relaxed)) {
            const char *newp = p->shared->hp_eq_path;
            if (newp[0] == '\0') {
                p->hp_eq.valid = 0;
                p->hp_eq.num_bands = 0;
                p->hp_eq_loaded_path[0] = '\0';
            } else if (strncmp(newp, p->hp_eq_loaded_path,
                                sizeof(p->hp_eq_loaded_path)) != 0) {
                int rc = hp_eq_parse_file(newp, &p->hp_eq,
                                           (float)p->sample_rate);
                if (rc == 0 && p->hp_eq.num_bands > 0) {
                    strncpy(p->hp_eq_loaded_path, newp,
                            sizeof(p->hp_eq_loaded_path) - 1);
                    p->hp_eq_loaded_path[sizeof(p->hp_eq_loaded_path) - 1] = '\0';
                    fprintf(stderr,
                            "[af_hrtf] Headphone EQ loaded: '%s' "
                            "(%d bands, preamp=%.1f dB)\n",
                            newp, p->hp_eq.num_bands, p->hp_eq.preamp_db);
                } else {
                    fprintf(stderr,
                            "[af_hrtf] Headphone EQ load FAILED: '%s' "
                            "(expected AutoEQ ParametricEq.txt format)\n",
                            newp);
                    p->hp_eq.valid = 0;
                    p->hp_eq.num_bands = 0;
                    p->hp_eq_loaded_path[0] = '\0';
                }
            }
            atomic_store_explicit(&p->shared->hp_eq_changed, 0,
                                  memory_order_relaxed);
        }

        // Sync reverb parameters
        if (atomic_load_explicit(&p->shared->reverb_changed,
                                  memory_order_relaxed)) {
            float rv_decay = atomic_load_explicit(&p->shared->reverb_decay,
                                                   memory_order_relaxed);
            float rv_damp  = atomic_load_explicit(&p->shared->reverb_damping,
                                                   memory_order_relaxed);
            float rv_wet   = atomic_load_explicit(&p->shared->reverb_wet,
                                                   memory_order_relaxed);
            float rv_pd    = atomic_load_explicit(&p->shared->reverb_predelay,
                                                   memory_order_relaxed);
            reverb_update(&p->reverb, rv_decay, rv_damp, rv_wet, rv_pd,
                          p->sample_rate);
            atomic_store_explicit(&p->shared->reverb_changed, 0,
                                  memory_order_relaxed);
        }
        p->reverb.enabled = atomic_load_explicit(&p->shared->reverb_enabled,
                                                   memory_order_relaxed);

        // Sync room geometry for early reflections
        if (atomic_load_explicit(&p->shared->room_changed,
                                  memory_order_relaxed)) {
            float rm_w = atomic_load_explicit(&p->shared->room_width,
                                               memory_order_relaxed);
            float rm_d = atomic_load_explicit(&p->shared->room_depth,
                                               memory_order_relaxed);
            float rm_h = atomic_load_explicit(&p->shared->room_height,
                                               memory_order_relaxed);
            float rm_a = atomic_load_explicit(&p->shared->room_absorption,
                                               memory_order_relaxed);
            er_update(&p->er, rm_w, rm_d, rm_h, rm_a, p->sample_rate);
            er_clear(&p->er);
            atomic_store_explicit(&p->shared->room_changed, 0,
                                  memory_order_relaxed);
        }

        master_vol = atomic_load_explicit(&p->shared->master_volume,
                                           memory_order_relaxed);

        // Write per-channel RMS/peak levels for UI meters
        uint8_t **rms_data = mp_aframe_get_data_ro(in);
        if (rms_data) {
            for (int ch = 0; ch < in_channels && ch < HRTF_SS_MAX_CHANNELS; ch++) {
                float sum_sq = 0, peak = 0;
                float *ch_data = (float*)rms_data[ch];
                for (int i = 0; i < in_samples; i++) {
                    float s = fabsf(ch_data[i]);
                    sum_sq += s * s;
                    if (s > peak) peak = s;
                }
                float rms = sqrtf(sum_sq / (float)(in_samples > 0 ? in_samples : 1));
                atomic_store_explicit(&p->shared->channel_rms[ch], rms,
                                      memory_order_relaxed);
                atomic_store_explicit(&p->shared->channel_peak[ch], peak,
                                      memory_order_relaxed);
            }
        }
    }

    // Get input data (planar float)
    uint8_t **in_data = mp_aframe_get_data_ro(in);
    if (!in_data)
        goto error;

    // prev_frame_tail[] holds the last sample of the PREVIOUS frame,
    // used for boundary smoothing in the accumulation loop below.
    // It gets updated after accumulation with THIS frame's last sample.

    // Create output frame (stereo, same number of samples as input)
    struct mp_aframe *out = mp_aframe_create();
    mp_aframe_set_format(out, AF_FORMAT_FLOATP);
    mp_aframe_set_rate(out, in_rate);

    struct mp_chmap stereo = MP_CHMAP_INIT_STEREO;
    mp_aframe_set_chmap(out, &stereo);

    if (mp_aframe_pool_allocate(p->out_pool, out, in_samples) < 0) {
        talloc_free(out);
        goto error;
    }

    mp_aframe_copy_attributes(out, in);

    uint8_t **out_data = mp_aframe_get_data_rw(out);
    if (!out_data) {
        talloc_free(out);
        goto error;
    }

    float *out_l = (float*)out_data[0];
    float *out_r = (float*)out_data[1];

#if HRTF_BYPASS_MODE
    // BYPASS: simple stereo downmix — no HRTF, no FFT, no overlap-add.
    // If the audio corruption persists, the problem is NOT in this filter.
    {
        memset(out_l, 0, in_samples * sizeof(float));
        memset(out_r, 0, in_samples * sizeof(float));
        for (int i = 0; i < in_samples; i++) {
            float l = 0, r = 0;
            for (int ch = 0; ch < in_channels && ch < HRTF_MAX_CHANNELS; ch++) {
                float s = ((float*)in_data[ch])[i];
                float az = p->speaker_pos[ch].azimuth;
                // Simple panning: left channels (az>0) to L, right (az<0) to R,
                // center (az~0) to both. Use sine/cosine pan law.
                float pan = (az + 90.0f) / 180.0f; // 0=hard right, 1=hard left
                if (pan < 0) pan = 0;
                if (pan > 1) pan = 1;
                l += s * pan;
                r += s * (1.0f - pan);
            }
            // Scale down to avoid clipping from summing many channels
            out_l[i] = l * 0.25f * master_vol;
            out_r[i] = r * 0.25f * master_vol;
        }

        mp_aframe_set_size(out, in_samples);
        mp_frame_unref(&frame);
        mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, out));
        return;
    }
#endif

    // -----------------------------------------------------------------
    // Block-based processing with input accumulation
    // -----------------------------------------------------------------
    // The overlap-add convolver requires fixed HRTF_BLOCK_SIZE blocks.
    // Input frames (especially lossless HD) can be much smaller (e.g. 40 samples).
    //
    // Strategy:
    //   1. Drain any residual output from the previous frame's last block.
    //   2. Consume ALL input, processing blocks as they fill up.
    //      - If there's room in the output frame (>= BLOCK_SIZE), write
    //        the block directly to the output frame.
    //      - Otherwise, write to the residual buffer and serve what fits.
    //   3. Zero-fill any remaining output (startup latency).
    // -----------------------------------------------------------------

    int out_written = 0;
    int in_read = 0;

    // Step 1: Drain residual from previous frame
    if (p->output_avail > 0) {
        int n = p->output_avail < in_samples ? p->output_avail : in_samples;
        memcpy(out_l, p->output_l + p->output_read_pos, n * sizeof(float));
        memcpy(out_r, p->output_r + p->output_read_pos, n * sizeof(float));
        p->output_avail -= n;
        p->output_read_pos += n;
        out_written = n;
    }

    // Step 2: Consume ALL input samples, processing blocks as they complete
    while (in_read < in_samples) {
        int space = HRTF_BLOCK_SIZE - p->input_accum_pos;
        int avail = in_samples - in_read;
        int n = space < avail ? space : avail;

        if (n > 0) {
            for (int ch = 0; ch < in_channels && ch < HRTF_MAX_CHANNELS; ch++) {
                memcpy(p->input_accum[ch] + p->input_accum_pos,
                       (float*)in_data[ch] + in_read, n * sizeof(float));

                /* Frame boundary smoothing DISABLED — was causing crackling
                 * by modifying normal audio at 1200 Hz rate (threshold too
                 * low: 0.001 catches legitimate signal transitions). */
                if (0 && in_read == 0 && p->prev_frame_tail_valid) {
                    float prev = p->prev_frame_tail[ch];
                    float curr = p->input_accum[ch][p->input_accum_pos];
                    float jump = fabsf(curr - prev);
                    int is_height_ch = (ch >= p->num_bed_channels && ch != 3);

                    float threshold = is_height_ch ? 0.0005f : 0.001f;
                    int max_blend = is_height_ch ? 16 : 4;

                    if (jump > threshold) {
                        int blend_n = n < max_blend ? n : max_blend;
                        for (int b = 0; b < blend_n; b++) {
                            float t = (float)(b + 1) / (float)(blend_n + 1);
                            /* Cosine window for height (equal-power),
                             * linear for bed (simpler, sufficient). */
                            float g_new, g_old;
                            if (is_height_ch) {
                                float phase = t * ((float)M_PI * 0.5f);
                                g_new = sinf(phase);
                                g_old = cosf(phase);
                            } else {
                                g_new = t;
                                g_old = 1.0f - t;
                            }
                            int idx = p->input_accum_pos + b;
                            p->input_accum[ch][idx] = prev * g_old
                                                    + p->input_accum[ch][idx] * g_new;
                        }
                    }
                }
            }
            p->input_accum_pos += n;
            in_read += n;
        }

        if (p->input_accum_pos >= HRTF_BLOCK_SIZE) {
            float *accum_ptrs[HRTF_MAX_CHANNELS];
            for (int ch = 0; ch < in_channels && ch < HRTF_MAX_CHANNELS; ch++)
                accum_ptrs[ch] = p->input_accum[ch];

            /* ObjCoding reconstruction: extract individual object audio from
             * the 5.1 downmix using the mixing matrix parsed from the E-AC3
             * EMDF payload.  Reconstructed objects are appended as extra
             * channels after the bed for HRTF spatialization. */
            int proc_channels = in_channels;
            int joc_objects = objcoding_reconstruct_objects(p, accum_ptrs, in_channels);
            if (joc_objects > 0) {
                for (int i = 0; i < joc_objects && (in_channels + i) < HRTF_MAX_CHANNELS; i++)
                    accum_ptrs[in_channels + i] = p->objcoding_obj_buf[i];
                proc_channels = in_channels + joc_objects;
                if (proc_channels > HRTF_MAX_CHANNELS)
                    proc_channels = HRTF_MAX_CHANNELS;
            }

            int needed = in_samples - out_written;
            if (needed >= HRTF_BLOCK_SIZE) {
                // Enough room — write directly into the output frame
                process_block(p, accum_ptrs, proc_channels, HRTF_BLOCK_SIZE,
                              out_l + out_written, out_r + out_written);
                out_written += HRTF_BLOCK_SIZE;
            } else {
                // Not enough room — process into residual buffer and
                // serve only what fits (rest stays for next frame)
                process_block(p, accum_ptrs, proc_channels, HRTF_BLOCK_SIZE,
                              p->output_l, p->output_r);
                p->output_read_pos = 0;
                p->output_avail = HRTF_BLOCK_SIZE;

                if (needed > 0) {
                    memcpy(out_l + out_written, p->output_l, needed * sizeof(float));
                    memcpy(out_r + out_written, p->output_r, needed * sizeof(float));
                    p->output_read_pos = needed;
                    p->output_avail -= needed;
                    out_written += needed;
                }
            }
            p->input_accum_pos = 0;
        }
    }

    // Save last sample of this frame for next frame's boundary smoothing
    if (in_samples > 0) {
        for (int ch = 0; ch < in_channels && ch < HRTF_MAX_CHANNELS; ch++)
            p->prev_frame_tail[ch] = ((float*)in_data[ch])[in_samples - 1];
        p->prev_frame_tail_valid = 1;
    }

    // Step 3: Zero-fill remaining output (startup latency)
    if (out_written < in_samples) {
        memset(out_l + out_written, 0, (in_samples - out_written) * sizeof(float));
        memset(out_r + out_written, 0, (in_samples - out_written) * sizeof(float));
    }

    // Apply master volume
    for (int i = 0; i < in_samples; i++) {
        out_l[i] *= master_vol;
        out_r[i] *= master_vol;
    }

    // Crossfeed: mix a small fraction of opposite channel to reduce
    // the hyper-lateralization inherent in HRTF-based binaural rendering.
    // Signed value: positive = classical crossfeed (narrows image, can pull
    // side sources toward the back); negative = stereo widener (subtracts
    // the contralateral signal, amplifying ILD so sides feel more lateral).
    // At ±0.30 artifacts appear (phasing / thin center); keep modest.
    if (p->shared) {
        float xfeed = atomic_load_explicit(&p->shared->crossfeed,
                                            memory_order_relaxed);
        if (xfeed < -0.3f) xfeed = -0.3f;
        if (xfeed >  0.3f) xfeed =  0.3f;
        if (xfeed != 0.0f) {
            for (int i = 0; i < in_samples; i++) {
                float l = out_l[i];
                float r = out_r[i];
                out_l[i] = l * (1.0f - xfeed) + r * xfeed;
                out_r[i] = r * (1.0f - xfeed) + l * xfeed;
            }
        }
    }

    // Bauer (frequency-selective) crossfeed.  Adds a delayed low-passed copy
    // of each channel into the opposite ear — models natural LF diffraction
    // around the head.  Unlike the broadband crossfeed above, this preserves
    // HF localisation cues (ITD/ILD >700 Hz) so side/rear sources stay put,
    // while LF body bleeds over to the contralateral ear — grounding frontal
    // sources that would otherwise feel collapsed to one side.
    if (p->shared) {
        float amt = atomic_load_explicit(&p->shared->bauer_crossfeed,
                                          memory_order_relaxed);
        if (amt < 0.0f) amt = 0.0f;
        if (amt > 0.5f) amt = 0.5f;
        if (amt > 0.0f) {
            const float fc = 700.0f;
            float alpha = 1.0f - expf(-2.0f * (float)M_PI * fc /
                                       (float)p->sample_rate);
            // ITD-like delay: ~290 µs = 14 samples at 48 kHz
            int delay = (int)(0.00029f * (float)p->sample_rate + 0.5f);
            if (delay < 1) delay = 1;
            if (delay >= BAUER_DELAY_LEN) delay = BAUER_DELAY_LEN - 1;

            for (int i = 0; i < in_samples; i++) {
                // Write current output into delay line
                p->bauer_buf_l[p->bauer_pos] = out_l[i];
                p->bauer_buf_r[p->bauer_pos] = out_r[i];
                int rp = p->bauer_pos - delay;
                if (rp < 0) rp += BAUER_DELAY_LEN;

                // Low-pass the delayed contralateral signal
                float dl = p->bauer_buf_l[rp];
                float dr = p->bauer_buf_r[rp];
                p->bauer_lp_l += alpha * (dl - p->bauer_lp_l);
                p->bauer_lp_r += alpha * (dr - p->bauer_lp_r);

                // Cross-feed LP'd L into R and vice versa.
                out_l[i] += amt * p->bauer_lp_r;
                out_r[i] += amt * p->bauer_lp_l;

                if (++p->bauer_pos >= BAUER_DELAY_LEN) p->bauer_pos = 0;
            }
        }
    }

    // NOTE: debug logging is done AFTER the limiter (below)

    // Early reflections are now applied inside process_block() via the
    // ambisonic er_process_ambi() pipeline (Steam Audio-style).  Nothing
    // to do here — they are already mixed into out_l/out_r.

    // Apply reverb
    if (p->reverb.enabled && p->reverb.initialized)
        reverb_process(&p->reverb, out_l, out_r, in_samples);

    // Headphone EQ correction.  Hot-reload triggered by ir_changed / path
    // change is handled in the SharedState sync section below; here we just
    // apply the precomputed biquad cascade to the final stereo output before
    // the limiter.  Order matters: spatial DSP first, then EQ for the
    // listener's specific headphones, then peak protection.
    if (p->shared && p->hp_eq.valid &&
        atomic_load_explicit(&p->shared->hp_eq_enabled,
                              memory_order_relaxed)) {
        hp_eq_process(&p->hp_eq, out_l, out_r, in_samples);
    }

    // -----------------------------------------------------------------
    // Test tone generation (mixed into output after HRTF processing)
    // Uses block-based accumulation identical to main audio path.
    // -----------------------------------------------------------------
    if (p->shared && p->sofa) {
        int tone_active = atomic_load_explicit(&p->shared->test_tone_active,
                                                memory_order_relaxed);

        // Start new test tone
        if (tone_active && p->test_tone_remaining <= 0) {
            p->test_tone_ch = atomic_load_explicit(
                &p->shared->test_tone_channel, memory_order_relaxed);
            p->test_tone_remaining = in_rate / 2;  // 0.5 seconds
            p->test_tone_phase = 0.0f;
            p->test_tone_generated = 0;
            p->test_tone_out_avail = 0;
            p->test_tone_out_read = 0;

            if (!p->test_tone_inited) {
                HrtfChannelPair *tp = &p->test_tone_pair;
                tp->active_idx = 0;
                tp->crossfade_remaining = 0;
                for (int i = 0; i < 2; i++) {
                    convolver_init(&tp->left[i]);
                    convolver_init(&tp->right[i]);
                }
                p->test_tone_inited = 1;
            }

            HrtfSpeakerPos tpos = {0.0f, 0.0f, 2.0f};
            if (p->test_tone_ch >= 0 && p->test_tone_ch < HRTF_SS_MAX_CHANNELS)
                tpos = p->shared->speaker_pos[p->test_tone_ch];

            float *hl = calloc(p->hrir_length, sizeof(float));
            float *hr = calloc(p->hrir_length, sizeof(float));
            get_hrir_for_position(p, tpos.azimuth, tpos.elevation,
                                  tpos.distance, hl, hr);

            int next = 1 - p->test_tone_pair.active_idx;
            convolver_set_hrir(&p->test_tone_pair.left[next], hl, p->hrir_length);
            convolver_set_hrir(&p->test_tone_pair.right[next], hr, p->hrir_length);
            p->test_tone_pair.active_idx = next;

            free(hl);
            free(hr);
        }

        // Mix test tone into output using block-based processing
        if (p->test_tone_remaining > 0) {
            int act = p->test_tone_pair.active_idx;
            int total_tone = in_rate / 2;
            int fade_len = in_rate / 50;  // 20ms fade
            int tone_written = 0;

            // Step 1: Drain residual from previous block
            if (p->test_tone_out_avail > 0) {
                int n = p->test_tone_out_avail;
                if (n > in_samples) n = in_samples;
                if (n > p->test_tone_remaining) n = p->test_tone_remaining;
                for (int i = 0; i < n; i++) {
                    out_l[i] += p->test_tone_out_l[p->test_tone_out_read + i] * master_vol;
                    out_r[i] += p->test_tone_out_r[p->test_tone_out_read + i] * master_vol;
                }
                p->test_tone_out_avail -= n;
                p->test_tone_out_read += n;
                p->test_tone_remaining -= n;
                tone_written = n;
            }

            // Step 2: Generate new BLOCK_SIZE blocks as needed
            while (tone_written < in_samples && p->test_tone_remaining > 0) {
                // Generate BLOCK_SIZE samples of 1kHz sine tone
                float tone[HRTF_BLOCK_SIZE];
                for (int i = 0; i < HRTF_BLOCK_SIZE; i++) {
                    float env = 1.0f;
                    int sample_idx = p->test_tone_generated + i;

                    if (sample_idx < fade_len)
                        env = (float)sample_idx / (float)fade_len;
                    int from_end = total_tone - 1 - sample_idx;
                    if (from_end >= 0 && from_end < fade_len)
                        env *= (float)(from_end + 1) / (float)fade_len;
                    if (sample_idx >= total_tone)
                        env = 0.0f;

                    tone[i] = 0.3f * env * sinf(p->test_tone_phase);
                    p->test_tone_phase += 2.0f * (float)M_PI * 1000.0f / in_rate;
                    if (p->test_tone_phase >= 2.0f * (float)M_PI)
                        p->test_tone_phase -= 2.0f * (float)M_PI;
                }
                p->test_tone_generated += HRTF_BLOCK_SIZE;

                // Convolve with HRIR
                float tl[HRTF_BLOCK_SIZE], tr[HRTF_BLOCK_SIZE];
                convolver_process(&p->test_tone_pair.left[act], tone, tl);
                convolver_process(&p->test_tone_pair.right[act], tone, tr);

                // Serve what fits into this output frame
                int needed = in_samples - tone_written;
                int serve = needed < HRTF_BLOCK_SIZE ? needed : HRTF_BLOCK_SIZE;
                if (serve > p->test_tone_remaining) serve = p->test_tone_remaining;

                for (int i = 0; i < serve; i++) {
                    out_l[tone_written + i] += tl[i] * master_vol;
                    out_r[tone_written + i] += tr[i] * master_vol;
                }
                tone_written += serve;
                p->test_tone_remaining -= serve;

                // Save remainder as residual for next frame
                int leftover = HRTF_BLOCK_SIZE - serve;
                if (leftover > 0 && p->test_tone_remaining > 0) {
                    memcpy(p->test_tone_out_l, tl + serve, leftover * sizeof(float));
                    memcpy(p->test_tone_out_r, tr + serve, leftover * sizeof(float));
                    p->test_tone_out_avail = leftover;
                    p->test_tone_out_read = 0;
                }
            }

            // Check if tone is finished
            if (p->test_tone_remaining <= 0) {
                p->test_tone_remaining = 0;
                p->test_tone_out_avail = 0;
                int a = p->test_tone_pair.active_idx;
                memset(p->test_tone_pair.left[a].overlap_buf, 0,
                       HRTF_FFT_N * sizeof(float));
                memset(p->test_tone_pair.right[a].overlap_buf, 0,
                       HRTF_FFT_N * sizeof(float));
                atomic_store_explicit(&p->shared->test_tone_active, 0,
                                      memory_order_relaxed);
            }
        }
    }

    /* Soft clipper: uses tanh-based saturation instead of hard clipping.
     * Hard clipping (the previous approach) cuts peaks flat, creating
     * harmonic distortion that sounds like electrical crackling.
     * Soft clipping rounds the peaks smoothly, preserving the waveform
     * shape and eliminating the "vinyl crackle" artifact.
     *
     * No envelope follower (limiter) — it caused gain pumping artifacts
     * from its 2ms attack / 200ms release time constants. The soft
     * clipper handles peak control instantaneously without pumping. */
    {
        const float drive = 0.85f;  /* how much signal hits the saturator */
        for (int i = 0; i < in_samples; i++) {
            float l = out_l[i] * drive;
            float r = out_r[i] * drive;

            /* Soft clip via tanh: smoothly saturates peaks above ~0.85
             * without the harsh harmonics of hard clipping. */
            out_l[i] = tanhf(l);
            out_r[i] = tanhf(r);
        }
    }

    // Debug: track peaks across ALL frames.
    {
        static int total_frames = 0;
        static float true_max_post = 0;
        total_frames++;

        for (int i = 0; i < in_samples; i++) {
            float v = fabsf(out_l[i]) > fabsf(out_r[i]) ? fabsf(out_l[i]) : fabsf(out_r[i]);
            if (v > true_max_post) true_max_post = v;
        }

        if ((hrtf_dbg_count++ % 500) == 0) {
            float max_in = 0;
            float ch_peak[HRTF_MAX_CHANNELS] = {0};
            for (int ch = 0; ch < in_channels && ch < HRTF_MAX_CHANNELS; ch++) {
                float *cd = (float*)in_data[ch];
                for (int i = 0; i < in_samples; i++) {
                    float v = fabsf(cd[i]);
                    if (v > ch_peak[ch]) ch_peak[ch] = v;
                    if (v > max_in) max_in = v;
                }
            }
            HRTF_DBG("frame %d: max_in=%.6f POST_PEAK=%.6f vol=%.3f lim=%.3f wr=%d\n",
                      total_frames, max_in, true_max_post, master_vol,
                      p->out_limiter_gain, out_written);
            if (max_in > 0.001f) {
                HRTF_DBG("  ch_peak:");
                for (int ch = 0; ch < in_channels && ch < 16; ch++)
                    HRTF_DBG(" [%d]=%.4f", ch, ch_peak[ch]);
                HRTF_DBG("\n");
            }
            true_max_post = 0;
        }
    }

    mp_frame_unref(&frame);
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, out));
    return;

error:
    mp_frame_unref(&frame);
    mp_filter_internal_mark_failed(f);
}

static void af_hrtf_reset(struct mp_filter *f) {
    struct priv *p = f->priv;

    // Reset overlap buffers
    for (int ch = 0; ch < p->num_channels; ch++) {
        for (int i = 0; i < 2; i++) {
            if (p->channels[ch].left[i].overlap_buf)
                memset(p->channels[ch].left[i].overlap_buf, 0,
                       HRTF_FFT_N * sizeof(float));
            if (p->channels[ch].right[i].overlap_buf)
                memset(p->channels[ch].right[i].overlap_buf, 0,
                       HRTF_FFT_N * sizeof(float));
        }
    }
    p->input_accum_pos = 0;
    p->output_avail = 0;
    p->output_read_pos = 0;
    p->test_tone_out_avail = 0;
    p->test_tone_out_read = 0;

    memset(p->air_abs_state, 0, sizeof(p->air_abs_state));
    p->out_limiter_gain = 1.0f;

    reverb_clear(&p->reverb);
    er_clear(&p->er);
}

static void af_hrtf_destroy(struct mp_filter *f) {
    struct priv *p = f->priv;

    for (int ch = 0; ch < HRTF_MAX_CHANNELS; ch++) {
        for (int i = 0; i < 2; i++) {
            convolver_destroy(&p->channels[ch].left[i]);
            convolver_destroy(&p->channels[ch].right[i]);
        }
        free(p->input_accum[ch]);
    }
    free(p->output_l);
    free(p->output_r);

    // Clean up test tone convolvers
    if (p->test_tone_inited) {
        for (int i = 0; i < 2; i++) {
            convolver_destroy(&p->test_tone_pair.left[i]);
            convolver_destroy(&p->test_tone_pair.right[i]);
        }
    }

    // Free object coding object buffers
    for (int i = 0; i < OBJCODING_MAX_OBJECTS; i++)
        free(p->objcoding_obj_buf[i]);

    reverb_destroy(&p->reverb);
    pconv_destroy(&p->ir_l);
    pconv_destroy(&p->ir_r);
    er_destroy(&p->er);

    if (p->sofa)
        mysofa_close(p->sofa);

    talloc_free(p->out_pool);
}

static const struct mp_filter_info af_hrtf_filter = {
    .name = "hrtf",
    .priv_size = sizeof(struct priv),
    .process = af_hrtf_process,
    .reset = af_hrtf_reset,
    .destroy = af_hrtf_destroy,
};

static struct mp_filter *af_hrtf_create(struct mp_filter *parent, void *options) {
    struct mp_filter *f = mp_filter_create(parent, &af_hrtf_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    // Add input and output pins (MUST be done before accessing ppins)
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(f, options);
    p->out_pool = mp_aframe_pool_create(f);

    // Extract shared state pointer for host app communication
    if (p->opts->shared_state_ptr)
        p->shared = (HrtfSharedState*)(intptr_t)p->opts->shared_state_ptr;

    // Create autoconvert sub-filter for format conversion to planar float
    struct mp_autoconvert *conv = mp_autoconvert_create(f);
    if (!conv) {
        talloc_free(f);
        return NULL;
    }

    mp_autoconvert_add_afmt(conv, AF_FORMAT_FLOATP);

    // Connect: external_in -> autoconvert -> our_in_pin
    mp_pin_connect(conv->f->pins[0], f->ppins[0]);
    p->in_pin = conv->f->pins[1];

    return f;
}

#define OPT_BASE_STRUCT struct hrtf_opts

const struct mp_user_filter_entry af_hrtf = {
    .desc = {
        .description = "HRTF binaural spatialization",
        .name = "hrtf",
        .priv_size = sizeof(struct hrtf_opts),
        .priv_defaults = &(const struct hrtf_opts){0},
        .options = (const struct m_option[]){
            {"sofa", OPT_STRING(sofa_path)},
            {"shared-state", OPT_INT64(shared_state_ptr)},
            {0}
        },
    },
    .create = af_hrtf_create,
};
