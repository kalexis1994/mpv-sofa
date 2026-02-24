/*
 * HaloSound Early Reflections - Image-source method
 * Extracted from mpv af_hrtf filter
 */

#ifndef EARLY_REFLECTIONS_H
#define EARLY_REFLECTIONS_H

#define ER_NUM_TAPS     6
#define ER_MAX_DELAY    8192
#define SPEED_OF_SOUND  343.0f

typedef struct {
    float *buf_l, *buf_r;
    int buf_size;
    int write_pos;
    int delays[ER_NUM_TAPS];
    float gain_l[ER_NUM_TAPS];
    float gain_r[ER_NUM_TAPS];
    int num_taps;
    int initialized;
} EarlyReflections;

void er_init(EarlyReflections *er);
void er_update(EarlyReflections *er, float width, float depth,
               float height, float absorption, int sample_rate);
void er_process(EarlyReflections *er, float *l, float *r, int n);
void er_clear(EarlyReflections *er);
void er_destroy(EarlyReflections *er);

#endif
