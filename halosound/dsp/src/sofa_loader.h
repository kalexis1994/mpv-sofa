/*
 * HaloSound SOFA Loader
 * Loads HRTF data from SOFA files (uses libmysofa when available)
 */

#ifndef SOFA_LOADER_H
#define SOFA_LOADER_H

#include <stdint.h>

typedef struct SofaData {
    void *handle;          /* libmysofa handle (MYSOFA_EASY*) */
    int ir_length;         /* HRIR length in samples */
    int sample_rate;       /* Target sample rate */
    int loaded;            /* Non-zero if data loaded successfully */
} SofaData;

SofaData* sofa_load_buffer(const uint8_t *data, int size, int target_sample_rate);
void sofa_get_hrir(SofaData *sofa, float x, float y, float z,
                   float *hrir_l, float *hrir_r,
                   float *delay_l, float *delay_r);
void sofa_destroy(SofaData *sofa);

#endif
