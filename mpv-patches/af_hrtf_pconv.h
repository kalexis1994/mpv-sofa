/*
 * Uniform-partitioned overlap-add convolution engine for long IRs (e.g. real
 * reverb impulse responses).  IR is split into K partitions of block_size
 * samples (zero-padded to fft_n = 2*block_size and FFT'd once at load).
 * Per input block we FFT the new input into a circular history of
 * frequency-domain buffers, multiply-add every history slot with its
 * matching IR partition (PFFFT z-domain — vectorised complex multiply),
 * IFFT the sum, then standard overlap-add.
 *
 * Algorithmic latency: one block of processing latency only.
 *
 * Requires (from the including TU):
 *   - PFFFT (pffft_*) symbols
 *   - <stdlib.h>, <string.h>
 */

#ifndef AF_HRTF_PCONV_H
#define AF_HRTF_PCONV_H

typedef struct {
    PFFFT_Setup *fft;
    int   block_size;        // P
    int   fft_n;             // 2 * P
    int   num_partitions;    // K
    float *ir_fft;           // [K * fft_n] — IR partitions, z-domain
    float *hist_fft;         // [K * fft_n] — circular input FFT history
    int   head;              // next write slot in hist_fft (0..K-1)
    float *work;             // PFFFT work area
    float *acc_fd;           // [fft_n] accumulator (frequency domain)
    float *tmp_td;           // [fft_n] time-domain scratch
    float *overlap;          // [P] overlap-add tail from previous block
    int   valid;
} PartitionedConvolver;

static void pconv_init(PartitionedConvolver *c, int block_size) {
    memset(c, 0, sizeof(*c));
    c->block_size = block_size;
    c->fft_n = 2 * block_size;
    c->fft = pffft_new_setup(c->fft_n, PFFFT_REAL);
    c->work    = pffft_aligned_malloc(c->fft_n * sizeof(float));
    c->acc_fd  = pffft_aligned_malloc(c->fft_n * sizeof(float));
    c->tmp_td  = pffft_aligned_malloc(c->fft_n * sizeof(float));
    c->overlap = pffft_aligned_malloc(block_size * sizeof(float));
    memset(c->overlap, 0, block_size * sizeof(float));
}

static void pconv_clear(PartitionedConvolver *c) {
    if (!c->valid) return;
    memset(c->hist_fft, 0,
           (size_t)c->num_partitions * (size_t)c->fft_n * sizeof(float));
    memset(c->overlap, 0, (size_t)c->block_size * sizeof(float));
    c->head = 0;
}

static void pconv_destroy(PartitionedConvolver *c) {
    if (c->fft) pffft_destroy_setup(c->fft);
    pffft_aligned_free(c->work);
    pffft_aligned_free(c->acc_fd);
    pffft_aligned_free(c->tmp_td);
    pffft_aligned_free(c->overlap);
    pffft_aligned_free(c->ir_fft);
    pffft_aligned_free(c->hist_fft);
    memset(c, 0, sizeof(*c));
}

static void pconv_load(PartitionedConvolver *c, const float *ir, int ir_len) {
    int P = c->block_size;
    int N = c->fft_n;
    int K = (ir_len + P - 1) / P;
    if (K < 1) K = 1;

    if (c->ir_fft)   pffft_aligned_free(c->ir_fft);
    if (c->hist_fft) pffft_aligned_free(c->hist_fft);
    c->num_partitions = K;
    c->ir_fft   = pffft_aligned_malloc((size_t)K * (size_t)N * sizeof(float));
    c->hist_fft = pffft_aligned_malloc((size_t)K * (size_t)N * sizeof(float));
    memset(c->hist_fft, 0, (size_t)K * (size_t)N * sizeof(float));
    memset(c->overlap, 0, (size_t)P * sizeof(float));
    c->head = 0;

    float *tmp = pffft_aligned_malloc(N * sizeof(float));
    for (int k = 0; k < K; k++) {
        memset(tmp, 0, N * sizeof(float));
        int start = k * P;
        int len = P;
        if (start + len > ir_len) len = ir_len - start;
        if (len > 0) memcpy(tmp, ir + start, (size_t)len * sizeof(float));
        pffft_transform(c->fft, tmp, c->ir_fft + (size_t)k * N,
                        c->work, PFFFT_FORWARD);
    }
    pffft_aligned_free(tmp);
    c->valid = 1;
}

static void pconv_process(PartitionedConvolver *c, const float *in, float *out) {
    if (!c->valid) {
        memset(out, 0, (size_t)c->block_size * sizeof(float));
        return;
    }
    int P = c->block_size;
    int N = c->fft_n;
    int K = c->num_partitions;

    float *cur_fft = c->hist_fft + (size_t)c->head * N;
    memset(c->tmp_td, 0, (size_t)N * sizeof(float));
    memcpy(c->tmp_td, in, (size_t)P * sizeof(float));
    pffft_transform(c->fft, c->tmp_td, cur_fft, c->work, PFFFT_FORWARD);

    memset(c->acc_fd, 0, (size_t)N * sizeof(float));
    for (int k = 0; k < K; k++) {
        int hist_idx = c->head - k;
        while (hist_idx < 0) hist_idx += K;
        pffft_zconvolve_accumulate(c->fft,
                                    c->hist_fft + (size_t)hist_idx * N,
                                    c->ir_fft   + (size_t)k        * N,
                                    c->acc_fd, 1.0f);
    }

    pffft_transform(c->fft, c->acc_fd, c->tmp_td, c->work, PFFFT_BACKWARD);

    float inv_n = 1.0f / (float)N;
    for (int i = 0; i < P; i++)
        out[i] = c->tmp_td[i] * inv_n + c->overlap[i];
    for (int i = 0; i < P; i++)
        c->overlap[i] = c->tmp_td[P + i] * inv_n;

    c->head = (c->head + 1) % K;
}

#endif /* AF_HRTF_PCONV_H */
