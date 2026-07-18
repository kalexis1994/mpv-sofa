/*
 * HaloSound HRTF Convolver
 * Overlap-add convolution using PFFFT
 * Extracted from mpv af_hrtf filter
 */

#ifndef HRTF_CONVOLVER_H
#define HRTF_CONVOLVER_H

#include "pffft.h"

#define HRTF_BLOCK_SIZE      256
#define HRTF_MAX_HRIR_LEN    768
#define HRTF_FFT_N           1024
#define HRTF_CROSSFADE_LEN   1024
#define HRTF_MAX_CHANNELS    16

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AIR_ABS_FACTOR 0.1f

typedef struct {
    PFFFT_Setup *fft_setup;
    float *hrir_td;
    float *hrir_fd;
    float *input_buf;
    float *overlap_buf;
    float *fft_tmp;
    float *fft_work;
    int hrir_len;
    int valid;
} HrtfConvolver;

typedef struct {
    HrtfConvolver left[2];
    HrtfConvolver right[2];
    int active_idx;
    int crossfade_remaining;
    int pending_reload;   /* position changed while a crossfade was running */
    float crossfade_prev_l[HRTF_BLOCK_SIZE];
    float crossfade_prev_r[HRTF_BLOCK_SIZE];
} HrtfChannelPair;

void convolver_init(HrtfConvolver *c);
void convolver_destroy(HrtfConvolver *c);
void convolver_set_hrir(HrtfConvolver *c, const float *hrir, int len);
void convolver_process(HrtfConvolver *c, const float *input, float *output);

#endif
