/*
 * HaloSound HRTF Convolver - Overlap-add convolution using PFFFT
 * Extracted from mpv af_hrtf.c (lines 335-419)
 */

#include <string.h>
#include <stdlib.h>
#include "hrtf_convolver.h"

void convolver_init(HrtfConvolver *c) {
    memset(c, 0, sizeof(*c));
    c->fft_setup = pffft_new_setup(HRTF_FFT_N, PFFFT_REAL);
    c->hrir_td     = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->hrir_fd     = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->input_buf   = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->overlap_buf = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->fft_tmp     = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));
    c->fft_work    = pffft_aligned_malloc(HRTF_FFT_N * sizeof(float));

    memset(c->hrir_td, 0, HRTF_FFT_N * sizeof(float));
    memset(c->hrir_fd, 0, HRTF_FFT_N * sizeof(float));
    memset(c->input_buf, 0, HRTF_FFT_N * sizeof(float));
    memset(c->overlap_buf, 0, HRTF_FFT_N * sizeof(float));
    memset(c->fft_tmp, 0, HRTF_FFT_N * sizeof(float));
    c->valid = 0;
}

void convolver_destroy(HrtfConvolver *c) {
    if (c->fft_setup) pffft_destroy_setup(c->fft_setup);
    pffft_aligned_free(c->hrir_td);
    pffft_aligned_free(c->hrir_fd);
    pffft_aligned_free(c->input_buf);
    pffft_aligned_free(c->overlap_buf);
    pffft_aligned_free(c->fft_tmp);
    pffft_aligned_free(c->fft_work);
    memset(c, 0, sizeof(*c));
}

void convolver_set_hrir(HrtfConvolver *c, const float *hrir, int len) {
    if (len > HRTF_MAX_HRIR_LEN)
        len = HRTF_MAX_HRIR_LEN;

    memset(c->hrir_td, 0, HRTF_FFT_N * sizeof(float));
    memcpy(c->hrir_td, hrir, len * sizeof(float));
    c->hrir_len = len;

    /* Clear overlap buffer - prevents stale overlap from previous HRIR */
    memset(c->overlap_buf, 0, HRTF_FFT_N * sizeof(float));

    /* Transform HRIR to frequency domain */
    pffft_transform(c->fft_setup, c->hrir_td, c->hrir_fd, c->fft_work, PFFFT_FORWARD);
    c->valid = 1;
}

void convolver_process(HrtfConvolver *c, const float *input, float *output) {
    if (!c->valid) {
        memset(output, 0, HRTF_BLOCK_SIZE * sizeof(float));
        return;
    }

    /* Zero-pad input to FFT size */
    memset(c->input_buf, 0, HRTF_FFT_N * sizeof(float));
    memcpy(c->input_buf, input, HRTF_BLOCK_SIZE * sizeof(float));

    /* Forward FFT of input */
    pffft_transform(c->fft_setup, c->input_buf, c->fft_tmp, c->fft_work, PFFFT_FORWARD);

    /* Multiply in frequency domain (input * HRIR) */
    memset(c->input_buf, 0, HRTF_FFT_N * sizeof(float));
    pffft_zconvolve_accumulate(c->fft_setup, c->fft_tmp, c->hrir_fd, c->input_buf, 1.0f);

    /* Inverse FFT */
    pffft_transform(c->fft_setup, c->input_buf, c->fft_tmp, c->fft_work, PFFFT_BACKWARD);

    /* Scale by 1/N (PFFFT doesn't normalize) */
    float scale = 1.0f / HRTF_FFT_N;
    for (int i = 0; i < HRTF_FFT_N; i++)
        c->fft_tmp[i] *= scale;

    /* Overlap-add: output = first L samples + accumulated overlap */
    for (int i = 0; i < HRTF_BLOCK_SIZE; i++)
        output[i] = c->fft_tmp[i] + c->overlap_buf[i];

    /* Shift overlap left by L and ADD new convolution tail */
    int tail = HRTF_FFT_N - HRTF_BLOCK_SIZE;
    for (int i = 0; i < tail; i++)
        c->overlap_buf[i] = c->overlap_buf[i + HRTF_BLOCK_SIZE] + c->fft_tmp[HRTF_BLOCK_SIZE + i];
    memset(c->overlap_buf + tail, 0, HRTF_BLOCK_SIZE * sizeof(float));
}
