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
} HrtfSharedState;

// ---------------------------------------------------------------------------
// Per-channel convolver (one per ear per channel)
// ---------------------------------------------------------------------------

typedef struct {
    PFFFT_Setup *fft_setup;

    // HRIR in time domain (zero-padded to FFT_N)
    float *hrir_td;     // [HRTF_FFT_N]
    // HRIR in frequency domain
    float *hrir_fd;     // [HRTF_FFT_N] (PFFFT complex format)

    // Input overlap buffer
    float *input_buf;   // [HRTF_FFT_N]
    // Output overlap-add buffer
    float *overlap_buf; // [HRTF_FFT_N]

    // Temp buffers for FFT
    float *fft_tmp;     // [HRTF_FFT_N]
    float *fft_work;    // PFFFT work area

    int hrir_len;
    int valid;
} HrtfConvolver;

// ---------------------------------------------------------------------------
// Double-buffered convolver pair (for click-free HRIR updates)
// ---------------------------------------------------------------------------

typedef struct {
    HrtfConvolver left[2];   // [0]=current, [1]=next
    HrtfConvolver right[2];
    int active_idx;           // 0 or 1
    int crossfade_remaining;  // samples left in crossfade
    float crossfade_prev_l[HRTF_BLOCK_SIZE]; // previous output for crossfade
    float crossfade_prev_r[HRTF_BLOCK_SIZE];
} HrtfChannelPair;

// ---------------------------------------------------------------------------
// Freeverb (Schroeder reverb) types
// ---------------------------------------------------------------------------

#define REVERB_NUM_COMBS    8
#define REVERB_NUM_ALLPASS  4
#define REVERB_STEREO_SPREAD 23
#define REVERB_MAX_PREDELAY  8192  // ~170ms at 48kHz

typedef struct {
    float *buffer;
    int size, pos;
    float feedback, damp1, damp2, filterstore;
} ReverbComb;

typedef struct {
    float *buffer;
    int size, pos;
    float feedback;  // typically 0.5
} ReverbAllpass;

typedef struct {
    ReverbComb combs_l[REVERB_NUM_COMBS], combs_r[REVERB_NUM_COMBS];
    ReverbAllpass allpass_l[REVERB_NUM_ALLPASS], allpass_r[REVERB_NUM_ALLPASS];
    float *predelay_l, *predelay_r;
    int predelay_size, predelay_pos;
    float wet, dry;
    int enabled, initialized;
} ReverbState;

// ---------------------------------------------------------------------------
// Early reflections (image-source method, 6 first-order taps)
// ---------------------------------------------------------------------------

typedef struct {
    float *buf_l, *buf_r;           // circular delay buffers
    int buf_size;                    // ER_MAX_DELAY
    int write_pos;
    int delays[ER_NUM_TAPS];        // delay in samples per tap
    float gain_l[ER_NUM_TAPS];      // per-tap gain for L
    float gain_r[ER_NUM_TAPS];      // per-tap gain for R
    int num_taps;
    int initialized;
} EarlyReflections;

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
    float min_dist;                           // minimum speaker distance
    float out_limiter_gain;                   // smoothed output limiter gain

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

};


// ---------------------------------------------------------------------------
// Convolver functions
// ---------------------------------------------------------------------------

static void convolver_init(HrtfConvolver *c) {
    memset(c, 0, sizeof(*c));
    c->fft_setup = pffft_new_setup(HRTF_FFT_N, PFFFT_REAL);
    c->hrir_td   = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->hrir_fd   = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->input_buf = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->overlap_buf = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->fft_tmp   = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->fft_work  = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));

    memset(c->hrir_td, 0, HRTF_FFT_N * sizeof(float));
    memset(c->hrir_fd, 0, HRTF_FFT_N * sizeof(float));
    memset(c->input_buf, 0, HRTF_FFT_N * sizeof(float));
    memset(c->overlap_buf, 0, HRTF_FFT_N * sizeof(float));
    memset(c->fft_tmp, 0, HRTF_FFT_N * sizeof(float));
    c->valid = 0;
}

static void convolver_destroy(HrtfConvolver *c) {
    if (c->fft_setup) pffft_destroy_setup(c->fft_setup);
    pffft_aligned_free(c->hrir_td);
    pffft_aligned_free(c->hrir_fd);
    pffft_aligned_free(c->input_buf);
    pffft_aligned_free(c->overlap_buf);
    pffft_aligned_free(c->fft_tmp);
    pffft_aligned_free(c->fft_work);
    memset(c, 0, sizeof(*c));
}

static void convolver_set_hrir(HrtfConvolver *c, const float *hrir, int len) {
    if (len > HRTF_MAX_HRIR_LEN)
        len = HRTF_MAX_HRIR_LEN;

    memset(c->hrir_td, 0, HRTF_FFT_N * sizeof(float));
    memcpy(c->hrir_td, hrir, len * sizeof(float));
    c->hrir_len = len;

    // Clear overlap buffer — prevents stale overlap from previous HRIR
    // from leaking into the crossfaded output.
    memset(c->overlap_buf, 0, HRTF_FFT_N * sizeof(float));

    // Transform HRIR to frequency domain
    pffft_transform(c->fft_setup, c->hrir_td, c->hrir_fd, c->fft_work, PFFFT_FORWARD);
    c->valid = 1;
}

// Overlap-add convolution: process one block of HRTF_BLOCK_SIZE input samples
static void convolver_process(HrtfConvolver *c, const float *input, float *output) {
    if (!c->valid) {
        memset(output, 0, HRTF_BLOCK_SIZE * sizeof(float));
        return;
    }

    // Zero-pad input to FFT size
    memset(c->input_buf, 0, HRTF_FFT_N * sizeof(float));
    memcpy(c->input_buf, input, HRTF_BLOCK_SIZE * sizeof(float));

    // Forward FFT of input
    pffft_transform(c->fft_setup, c->input_buf, c->fft_tmp, c->fft_work, PFFFT_FORWARD);

    // Multiply in frequency domain (input * HRIR)
    // input_buf is reused as accumulator — must be zeroed first
    memset(c->input_buf, 0, HRTF_FFT_N * sizeof(float));
    pffft_zconvolve_accumulate(c->fft_setup, c->fft_tmp, c->hrir_fd, c->input_buf, 1.0f);

    // Inverse FFT
    pffft_transform(c->fft_setup, c->input_buf, c->fft_tmp, c->fft_work, PFFFT_BACKWARD);

    // Scale by 1/N (PFFFT doesn't normalize)
    float scale = 1.0f / HRTF_FFT_N;
    for (int i = 0; i < HRTF_FFT_N; i++)
        c->fft_tmp[i] *= scale;

    // Overlap-add: output = first L samples of convolution + accumulated overlap
    for (int i = 0; i < HRTF_BLOCK_SIZE; i++)
        output[i] = c->fft_tmp[i] + c->overlap_buf[i];

    // Shift overlap buffer left by L and ADD new convolution tail.
    // This correctly handles HRIRs longer than BLOCK_SIZE where overlap
    // from one block can span multiple future blocks.
    int tail = HRTF_FFT_N - HRTF_BLOCK_SIZE;
    for (int i = 0; i < tail; i++)
        c->overlap_buf[i] = c->overlap_buf[i + HRTF_BLOCK_SIZE] + c->fft_tmp[HRTF_BLOCK_SIZE + i];
    memset(c->overlap_buf + tail, 0, HRTF_BLOCK_SIZE * sizeof(float));
}

// ---------------------------------------------------------------------------
// Freeverb functions
// ---------------------------------------------------------------------------

static const int reverb_comb_lengths[REVERB_NUM_COMBS] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
static const int reverb_allpass_lengths[REVERB_NUM_ALLPASS] = {
    556, 441, 341, 225
};

static void reverb_init(ReverbState *r, int sample_rate) {
    memset(r, 0, sizeof(*r));
    float scale = (float)sample_rate / 44100.0f;

    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        int len_l = (int)(reverb_comb_lengths[i] * scale);
        int len_r = (int)((reverb_comb_lengths[i] + REVERB_STEREO_SPREAD) * scale);
        if (len_l < 1) len_l = 1;
        if (len_r < 1) len_r = 1;

        r->combs_l[i].buffer = calloc(len_l, sizeof(float));
        r->combs_l[i].size = len_l;
        r->combs_l[i].pos = 0;
        r->combs_l[i].filterstore = 0;

        r->combs_r[i].buffer = calloc(len_r, sizeof(float));
        r->combs_r[i].size = len_r;
        r->combs_r[i].pos = 0;
        r->combs_r[i].filterstore = 0;
    }

    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        int len_l = (int)(reverb_allpass_lengths[i] * scale);
        int len_r = (int)((reverb_allpass_lengths[i] + REVERB_STEREO_SPREAD) * scale);
        if (len_l < 1) len_l = 1;
        if (len_r < 1) len_r = 1;

        r->allpass_l[i].buffer = calloc(len_l, sizeof(float));
        r->allpass_l[i].size = len_l;
        r->allpass_l[i].pos = 0;
        r->allpass_l[i].feedback = 0.5f;

        r->allpass_r[i].buffer = calloc(len_r, sizeof(float));
        r->allpass_r[i].size = len_r;
        r->allpass_r[i].pos = 0;
        r->allpass_r[i].feedback = 0.5f;
    }

    r->predelay_l = calloc(REVERB_MAX_PREDELAY, sizeof(float));
    r->predelay_r = calloc(REVERB_MAX_PREDELAY, sizeof(float));
    r->predelay_size = 1;
    r->predelay_pos = 0;

    r->wet = 0.0f;
    r->dry = 1.0f;
    r->enabled = 0;
    r->initialized = 1;
}

static void reverb_update(ReverbState *r, float decay, float damping,
                           float wet, float predelay_ms, int sample_rate) {
    if (!r->initialized) return;

    // Map decay (0-1) to comb feedback (0.7-0.98)
    float feedback = 0.7f + decay * 0.28f;
    float damp1 = damping;
    float damp2 = 1.0f - damping;

    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        r->combs_l[i].feedback = feedback;
        r->combs_l[i].damp1 = damp1;
        r->combs_l[i].damp2 = damp2;
        r->combs_r[i].feedback = feedback;
        r->combs_r[i].damp1 = damp1;
        r->combs_r[i].damp2 = damp2;
    }

    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        r->allpass_l[i].feedback = 0.5f;
        r->allpass_r[i].feedback = 0.5f;
    }

    r->wet = wet;
    r->dry = 1.0f;

    int pd = (int)(predelay_ms * (float)sample_rate / 1000.0f);
    if (pd < 1) pd = 1;
    if (pd > REVERB_MAX_PREDELAY) pd = REVERB_MAX_PREDELAY;
    r->predelay_size = pd;
}

static inline float comb_process(ReverbComb *c, float input) {
    float output = c->buffer[c->pos];
    // One-pole lowpass in feedback loop
    c->filterstore = output * c->damp2 + c->filterstore * c->damp1;
    c->buffer[c->pos] = input + c->filterstore * c->feedback;
    if (++c->pos >= c->size) c->pos = 0;
    return output;
}

static inline float allpass_process(ReverbAllpass *a, float input) {
    float bufout = a->buffer[a->pos];
    float output = bufout - input;
    a->buffer[a->pos] = input + bufout * a->feedback;
    if (++a->pos >= a->size) a->pos = 0;
    return output;
}

static void reverb_process(ReverbState *r, float *l, float *r_ch, int n) {
    if (!r->initialized) return;

    for (int i = 0; i < n; i++) {
        float in_l = l[i];
        float in_r = r_ch[i];

        // Pre-delay
        float pd_l = r->predelay_l[r->predelay_pos];
        float pd_r = r->predelay_r[r->predelay_pos];
        r->predelay_l[r->predelay_pos] = in_l;
        r->predelay_r[r->predelay_pos] = in_r;
        if (++r->predelay_pos >= r->predelay_size) r->predelay_pos = 0;

        // Parallel comb filters (fixedgain normalizes 8 parallel combs + feedback amp)
        static const float fixedgain = 0.05f;
        float scaled_l = pd_l * fixedgain;
        float scaled_r = pd_r * fixedgain;
        float comb_out_l = 0, comb_out_r = 0;
        for (int c = 0; c < REVERB_NUM_COMBS; c++) {
            comb_out_l += comb_process(&r->combs_l[c], scaled_l);
            comb_out_r += comb_process(&r->combs_r[c], scaled_r);
        }

        // Series allpass filters
        float ap_l = comb_out_l;
        float ap_r = comb_out_r;
        for (int a = 0; a < REVERB_NUM_ALLPASS; a++) {
            ap_l = allpass_process(&r->allpass_l[a], ap_l);
            ap_r = allpass_process(&r->allpass_r[a], ap_r);
        }

        // Wet/dry mix
        l[i] = in_l * r->dry + ap_l * r->wet;
        r_ch[i] = in_r * r->dry + ap_r * r->wet;
    }
}

static void reverb_clear(ReverbState *r) {
    if (!r->initialized) return;

    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        if (r->combs_l[i].buffer)
            memset(r->combs_l[i].buffer, 0, r->combs_l[i].size * sizeof(float));
        r->combs_l[i].pos = 0;
        r->combs_l[i].filterstore = 0;
        if (r->combs_r[i].buffer)
            memset(r->combs_r[i].buffer, 0, r->combs_r[i].size * sizeof(float));
        r->combs_r[i].pos = 0;
        r->combs_r[i].filterstore = 0;
    }
    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        if (r->allpass_l[i].buffer)
            memset(r->allpass_l[i].buffer, 0, r->allpass_l[i].size * sizeof(float));
        r->allpass_l[i].pos = 0;
        if (r->allpass_r[i].buffer)
            memset(r->allpass_r[i].buffer, 0, r->allpass_r[i].size * sizeof(float));
        r->allpass_r[i].pos = 0;
    }
    if (r->predelay_l)
        memset(r->predelay_l, 0, REVERB_MAX_PREDELAY * sizeof(float));
    if (r->predelay_r)
        memset(r->predelay_r, 0, REVERB_MAX_PREDELAY * sizeof(float));
    r->predelay_pos = 0;
}

static void reverb_destroy(ReverbState *r) {
    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        free(r->combs_l[i].buffer);
        free(r->combs_r[i].buffer);
    }
    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        free(r->allpass_l[i].buffer);
        free(r->allpass_r[i].buffer);
    }
    free(r->predelay_l);
    free(r->predelay_r);
    memset(r, 0, sizeof(*r));
}

// ---------------------------------------------------------------------------
// Early reflections functions
// ---------------------------------------------------------------------------

static void er_init(EarlyReflections *er) {
    memset(er, 0, sizeof(*er));
    er->buf_size = ER_MAX_DELAY;
    er->buf_l = calloc(ER_MAX_DELAY, sizeof(float));
    er->buf_r = calloc(ER_MAX_DELAY, sizeof(float));
    er->initialized = 1;
}

static void er_update(EarlyReflections *er, float width, float depth,
                       float height, float absorption, int sample_rate) {
    if (!er->initialized) return;

    // Listener position: centered width, 2/3 depth, ear height 1.2m
    float ear_height = 1.2f;
    float listener_depth_frac = 2.0f / 3.0f;

    // 6 image-source distances (extra path length beyond direct sound)
    // Tap 0: left wall   — image distance = room width (centered listener)
    // Tap 1: right wall  — same
    // Tap 2: floor       — 2 * ear_height
    // Tap 3: ceiling     — 2 * (height - ear_height)
    // Tap 4: front wall  — 2 * (listener_depth_frac * depth)
    // Tap 5: back wall   — 2 * ((1 - listener_depth_frac) * depth)
    float extra[ER_NUM_TAPS];
    extra[0] = width;                                  // left wall
    extra[1] = width;                                  // right wall
    extra[2] = 2.0f * ear_height;                      // floor
    extra[3] = 2.0f * (height - ear_height);           // ceiling
    extra[4] = 2.0f * (listener_depth_frac * depth);   // front wall
    extra[5] = 2.0f * ((1.0f - listener_depth_frac) * depth); // back wall

    er->num_taps = ER_NUM_TAPS;

    for (int i = 0; i < ER_NUM_TAPS; i++) {
        int d = (int)(extra[i] / SPEED_OF_SOUND * (float)sample_rate + 0.5f);
        if (d < 1) d = 1;
        if (d >= ER_MAX_DELAY) d = ER_MAX_DELAY - 1;
        er->delays[i] = d;

        float gain = (1.0f - absorption) / (1.0f + extra[i]);

        if (i <= 1) {
            // Lateral reflections: pan towards the reflecting wall
            float loud = 0.8f * gain;
            float quiet = 0.3f * gain;
            if (i == 0) {
                // Left wall: boost left ear
                er->gain_l[i] = loud;
                er->gain_r[i] = quiet;
            } else {
                // Right wall: boost right ear
                er->gain_l[i] = quiet;
                er->gain_r[i] = loud;
            }
        } else {
            // Floor, ceiling, front, back: equal L/R
            er->gain_l[i] = gain * 0.6f;
            er->gain_r[i] = gain * 0.6f;
        }
    }
}

static void er_process(EarlyReflections *er, float *l, float *r, int n) {
    for (int i = 0; i < n; i++) {
        // Write current sample into circular buffer
        er->buf_l[er->write_pos] = l[i];
        er->buf_r[er->write_pos] = r[i];

        // Accumulate all taps
        float sum_l = 0, sum_r = 0;
        for (int t = 0; t < er->num_taps; t++) {
            int read_pos = er->write_pos - er->delays[t];
            if (read_pos < 0) read_pos += er->buf_size;
            sum_l += er->buf_l[read_pos] * er->gain_l[t];
            sum_r += er->buf_r[read_pos] * er->gain_r[t];
        }

        // Add reflections to output
        l[i] += sum_l;
        r[i] += sum_r;

        if (++er->write_pos >= er->buf_size)
            er->write_pos = 0;
    }
}

static void er_clear(EarlyReflections *er) {
    if (!er->initialized) return;
    if (er->buf_l) memset(er->buf_l, 0, er->buf_size * sizeof(float));
    if (er->buf_r) memset(er->buf_r, 0, er->buf_size * sizeof(float));
    er->write_pos = 0;
}

static void er_destroy(EarlyReflections *er) {
    free(er->buf_l);
    free(er->buf_r);
    memset(er, 0, sizeof(*er));
}

// ---------------------------------------------------------------------------
// SOFA HRIR loading
// ---------------------------------------------------------------------------

static int load_sofa(struct priv *p, const char *path) {
    int filter_length = 0;
    int err;

    if (p->sofa) {
        mysofa_close(p->sofa);
        p->sofa = NULL;
    }

    p->sofa = mysofa_open(path, (float)p->sample_rate, &filter_length, &err);
    if (!p->sofa || err != MYSOFA_OK) {
        HRTF_DBG("SOFA LOAD FAILED: path=%s err=%d\n", path, err);
        p->sofa = NULL;
        return -1;
    }

    p->hrir_length = filter_length;
    HRTF_DBG("SOFA loaded OK: path=%s hrir_len=%d\n", path, filter_length);
    return 0;
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

    // Apply ITD: convert delays to samples, normalize to relative
    // (one ear at 0, the other at the ITD difference)
    float min_delay = delay_l < delay_r ? delay_l : delay_r;
    int dl = (int)((delay_l - min_delay) * p->sample_rate + 0.5f);
    int dr = (int)((delay_r - min_delay) * p->sample_rate + 0.5f);

    int max_shift = p->hrir_length / 4;  // safety clamp
    if (dl > max_shift) dl = max_shift;
    if (dr > max_shift) dr = max_shift;

    int len = p->hrir_length;

    // Window the HRIR tail: cosine fade-out over the last 40% to reduce
    // measurement-room ringing and coloration from long HRIR tails.
    {
        int fade_start = (int)(len * 0.6f);
        int fade_len = len - fade_start;
        for (int i = fade_start; i < len; i++) {
            float t = (float)(i - fade_start) / (float)fade_len;
            float w = 0.5f * (1.0f + cosf(t * (float)M_PI));  // 1 → 0
            ir_l[i] *= w;
            ir_r[i] *= w;
        }
    }

    // Left ear: shift HRIR right by dl samples (prepend zeros)
    memset(hrir_l, 0, len * sizeof(float));
    if (len - dl > 0)
        memcpy(hrir_l + dl, ir_l, (len - dl) * sizeof(float));

    // Right ear: shift HRIR right by dr samples
    memset(hrir_r, 0, len * sizeof(float));
    if (len - dr > 0)
        memcpy(hrir_r + dr, ir_r, (len - dr) * sizeof(float));

    free(ir_l);
    free(ir_r);
}

// Update convolvers for a given channel with new HRIR
// crossfade=1: real-time update with crossfade + rate-limiting
// crossfade=0: direct set on active slot (init/reload, no rate-limit)
static void update_channel_hrir_ex(struct priv *p, int ch, int crossfade) {
    HrtfChannelPair *pair = &p->channels[ch];

    if (crossfade) {
        // Rate-limit: skip if a crossfade is already in progress.
        // The position will be picked up on the next object metadata frame after
        // the current crossfade finishes (within ~21ms at 48kHz).
        if (pair->crossfade_remaining > 0)
            return;
    }

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

        pair->crossfade_remaining = HRTF_CROSSFADE_LEN;
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
    default:                 *az =   0.0f; *el =   0.0f; break;
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
    const float d = 2.0f;
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

// Fallback: hardcoded 7.1.4 positions (used if chmap not available)
static void init_speaker_positions(struct priv *p) {
    const float d = 2.0f;
    HrtfSpeakerPos defaults[] = {
        { 30.0f,  0.0f, d},  // 0: FL
        {-30.0f,  0.0f, d},  // 1: FR
        {  0.0f,  0.0f, d},  // 2: FC
        {  0.0f,  0.0f, d},  // 3: LFE
        {135.0f,  0.0f, d},  // 4: BL
        {-135.0f, 0.0f, d},  // 5: BR
        { 90.0f,  0.0f, d},  // 6: SL
        {-90.0f,  0.0f, d},  // 7: SR
        { 45.0f, 45.0f, d},  // 8: TFL
        {-45.0f, 45.0f, d},  // 9: TFR
        {135.0f, 45.0f, d},  // 10: TBL
        {-135.0f,45.0f, d},  // 11: TBR
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
                for (int ch = 0; ch < p->num_channels && ch < HRTF_MAX_CHANNELS; ch++)
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

    /* Dominant object gating: track top-gain objects */
    int dominant_count = 0;
    int dominant_idx = -1;
    float max_gain_db = -200.0f;

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
        }

        /* Linear gain for centroid weighting */
        float gain = powf(10.0f, (float)obj->gain_db / 20.0f);

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
        const float JOC_SMOOTH = 0.80f;
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

    /* --- Phase 2b: TrueHD centroid positioning for height channels --- */

    if (p->num_height_channels <= 0 || !p->sofa)
        goto shared_state_done;

    /* Smoothing factor: ~85% retention → ~15ms time constant at typical
     * OAMD update rate (~5ms). Prevents spatial "jumps". */
    const float SMOOTH = 0.85f;

    for (int h = 0; h < p->num_height_channels && h < HRTF_MAX_CHANNELS; h++) {
        int ch = p->height_ch_idx[h];
        if (ch < 0 || ch >= HRTF_MAX_CHANNELS)
            continue;

        /* Determine target position for this height channel.
         * Use azimuth sign to decide: positive az = left speaker → use L centroid,
         * negative az = right speaker → use R centroid. */
        float orig_az = p->speaker_pos[ch].azimuth;
        int use_left = (orig_az >= 0.0f);

        float cx, cy, cz;
        float wg = use_left ? wg_L : wg_R;

        if (dominant_count <= 2 && dominant_count > 0 && dominant_idx >= 0) {
            /* Dominant object gating: when 1-2 objects active, position
             * directly at the dominant object for maximum precision. */
            SpatialExtObjectPos *dom = &g_spatial_objmeta_ptr->objects[dominant_idx];
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
        if (new_el < 15.0f) new_el = 15.0f;

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

    // Load HRIRs for all channels (direct set, no crossfade during init)
    for (int ch = 0; ch < num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
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
        for (int k = 0; k < 2; k++) {
            convolver_init(&pair->left[k]);
            convolver_init(&pair->right[k]);
        }
        p->speaker_pos[ch] = (HrtfSpeakerPos){0, 0, 2.0f};
    }
    p->joc_smooth_valid = 0;
    memset(p->joc_smooth_az, 0, sizeof(p->joc_smooth_az));
    memset(p->joc_smooth_el, 0, sizeof(p->joc_smooth_el));
    memset(p->joc_smooth_dist, 0, sizeof(p->joc_smooth_dist));
    HRTF_DBG("init_hrtf: object coding reconstruction state initialized, g_objcoding_data_ptr=%p\n",
              (void*)g_objcoding_data_ptr);

    // Initialize reverb and sync initial params from shared state
    reverb_init(&p->reverb, sample_rate);
    er_init(&p->er);
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

    for (int ch = 0; ch < num_ch && ch < HRTF_MAX_CHANNELS; ch++) {
        // Skip muted channel groups
        if (mute_bed && ch < bed_count) continue;
        if (mute_obj && ch >= bed_count) continue;

        // When JOC objects replace bed, skip bed channels (keep LFE=3)
        if (joc_replaces_bed && ch < bed_count && ch != 3) continue;

        HrtfChannelPair *pair = &p->channels[ch];
        int active = pair->active_idx;

        // Distance attenuation (relative 1/r law), compensated by room gain
        float dist = p->speaker_pos[ch].distance;
        if (dist < p->min_dist) dist = p->min_dist;
        float dist_gain = (p->min_dist / dist) * room_gain;

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

        for (int i = 0; i < num_samples; i++)
            channel_data[ch][i] *= dist_gain;

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

        // Channel 3 is LFE - mix directly without HRTF
        if (ch == 3) {
            for (int i = 0; i < num_samples; i++) {
                float s = channel_data[ch][i] * 0.5f;
                out_l[i] += s;
                out_r[i] += s;
            }
            continue;
        }

        /* TrueHD height channels: mix as direct stereo instead of HRTF.
         * Height channels contain bed+object audio from the rematrix.
         * Running them through HRTF at a different position than the bed
         * creates comb filtering ("robotic" sound) because the bed
         * component gets convolved with two different HRIRs.
         *
         * Instead: pan TFL→left, TFR→right with a height-appropriate
         * gain, adding a subtle stereo width cue from the elevation. */
        int is_height = 0;
        for (int h = 0; h < p->num_height_channels; h++) {
            if (p->height_ch_idx[h] == ch) { is_height = 1; break; }
        }
        if (is_height && !p->objcoding_active) {
            float az = p->speaker_pos[ch].azimuth;
            /* Simple pan: az>0 = left-biased, az<0 = right-biased */
            float pan = 0.5f + az / 180.0f;  /* 0..1, 0.5=center */
            if (pan < 0.0f) pan = 0.0f;
            if (pan > 1.0f) pan = 1.0f;
            float gain_l = pan * 0.5f;       /* attenuated to avoid excess energy */
            float gain_r = (1.0f - pan) * 0.5f;
            for (int i = 0; i < num_samples; i++) {
                out_l[i] += channel_data[ch][i] * gain_l;
                out_r[i] += channel_data[ch][i] * gain_r;
            }
            continue;
        }

        if (!pair->left[active].valid)
            continue;

        // Convolve this channel with left and right HRIRs
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
                        float phase = (1.0f - (float)pair->crossfade_remaining / HRTF_CROSSFADE_LEN)
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

        // Accumulate into output
        for (int i = 0; i < num_samples; i++) {
            out_l[i] += block_l[i];
            out_r[i] += block_r[i];
        }
    }

    // Adaptive headroom: scales with active channel count to prevent
    // clipping from multi-channel summation through HRTF convolution.
    // For 7.1.2/7.1.4 Atmos beds, use a more conservative formula
    // since height channels carry significant energy.
    {
        int active_ch = 0;
        for (int ch = 0; ch < num_ch && ch < HRTF_MAX_CHANNELS; ch++) {
            if (mute_bed && ch < bed_count) continue;
            if (mute_obj && ch >= bed_count) continue;
            if (joc_replaces_bed && ch < bed_count && ch != 3) continue;
            active_ch++;
        }
        if (active_ch < 6) active_ch = 6;
        float headroom = 1.0f / sqrtf((float)active_ch);
        for (int i = 0; i < num_samples; i++) {
            out_l[i] *= headroom;
            out_r[i] *= headroom;
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
        if (in_chmap.num > 0) {
            init_speaker_positions_from_chmap(p, &in_chmap);
        }
        if (g_spatial_coeff_ptr && g_spatial_coeff_ptr->bed_mask) {
            int bed_count = init_speaker_positions_from_bed_mask(p, g_spatial_coeff_ptr->bed_mask);
            p->num_bed_channels = bed_count;
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
            }
            atomic_store_explicit(&p->shared->sofa_path_changed, 0,
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
            for (int ch = 0; ch < in_channels && ch < HRTF_MAX_CHANNELS; ch++)
                memcpy(p->input_accum[ch] + p->input_accum_pos,
                       (float*)in_data[ch] + in_read, n * sizeof(float));
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

    // NOTE: debug logging is done AFTER the limiter (below)

    // Apply early reflections
    if (p->er.initialized && p->er.num_taps > 0)
        er_process(&p->er, out_l, out_r, in_samples);

    // Apply reverb
    if (p->reverb.enabled && p->reverb.initialized)
        reverb_process(&p->reverb, out_l, out_r, in_samples);

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

    // Transparent safety limiter (sample-wise envelope).
    // Avoids frame-wide gain dips that can sound like transient "cuts".
    {
        const float ceiling = 0.90f; // leave headroom for DAC/output stages
        const float sr = (float)(p->sample_rate > 0 ? p->sample_rate : 48000);
        const float attack_tc = 0.002f;   // ~2 ms attack
        const float release_tc = 0.200f;  // ~200 ms release
        const float attack_a = expf(-1.0f / (sr * attack_tc));
        const float release_a = expf(-1.0f / (sr * release_tc));
        float g = p->out_limiter_gain;

        if (!(g > 0.0f) || g > 1.0f)
            g = 1.0f;

        for (int i = 0; i < in_samples; i++) {
            float l = out_l[i];
            float r = out_r[i];
            float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            float target = 1.0f;

            if (peak > ceiling && peak > 1e-12f)
                target = ceiling / peak;

            if (target < g)
                g = attack_a * g + (1.0f - attack_a) * target;
            else
                g = release_a * g + (1.0f - release_a) * target;

            l *= g;
            r *= g;

            // Hard safety clamp to guarantee no sample overs.
            if (l > ceiling) l = ceiling;
            else if (l < -ceiling) l = -ceiling;
            if (r > ceiling) r = ceiling;
            else if (r < -ceiling) r = -ceiling;

            out_l[i] = l;
            out_r[i] = r;
        }

        p->out_limiter_gain = g;
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
