/*
 * Single-partition FFT overlap-add convolver used for HRIRs (one per ear,
 * per channel) and for the ambisonic ER decoder taps.  Extracted from
 * af_hrtf.c — included once into that translation unit.
 *
 * Requires (from the including TU):
 *   - PFFFT (pffft_*) symbols
 *   - HRTF_FFT_N, HRTF_BLOCK_SIZE, HRTF_MAX_HRIR_LEN
 *   - <string.h> for memset/memcpy
 */

#ifndef AF_HRTF_CONVOLVER_H
#define AF_HRTF_CONVOLVER_H

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

// Double-buffered convolver pair (for click-free HRIR updates).
typedef struct {
    HrtfConvolver left[2];   // [0]=current, [1]=next
    HrtfConvolver right[2];
    int active_idx;           // 0 or 1
    int crossfade_remaining;  // samples left in crossfade
    int crossfade_total;      // actual crossfade length for current transition
    float crossfade_prev_l[HRTF_BLOCK_SIZE]; // previous output for crossfade
    float crossfade_prev_r[HRTF_BLOCK_SIZE];
} HrtfChannelPair;

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

#endif /* AF_HRTF_CONVOLVER_H */
