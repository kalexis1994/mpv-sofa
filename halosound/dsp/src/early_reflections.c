/*
 * HaloSound Early Reflections - Image-source method, 6 first-order taps
 * Extracted from mpv af_hrtf.c (lines 614-711)
 */

#include <string.h>
#include <stdlib.h>
#include "early_reflections.h"

void er_init(EarlyReflections *er) {
    memset(er, 0, sizeof(*er));
    er->buf_size = ER_MAX_DELAY;
    er->buf_l = calloc(ER_MAX_DELAY, sizeof(float));
    er->buf_r = calloc(ER_MAX_DELAY, sizeof(float));
    er->initialized = 1;
}

void er_update(EarlyReflections *er, float width, float depth,
               float height, float absorption, int sample_rate) {
    if (!er->initialized) return;

    float ear_height = 1.2f;
    float listener_depth_frac = 2.0f / 3.0f;

    /*
     * 6 image-source distances (extra path length beyond direct sound):
     * Tap 0: left wall    — image distance = room width
     * Tap 1: right wall   — same
     * Tap 2: floor        — 2 * ear_height
     * Tap 3: ceiling      — 2 * (height - ear_height)
     * Tap 4: front wall   — 2 * (listener_depth_frac * depth)
     * Tap 5: back wall    — 2 * ((1 - listener_depth_frac) * depth)
     */
    float extra[ER_NUM_TAPS];
    extra[0] = width;
    extra[1] = width;
    extra[2] = 2.0f * ear_height;
    extra[3] = 2.0f * (height - ear_height);
    extra[4] = 2.0f * (listener_depth_frac * depth);
    extra[5] = 2.0f * ((1.0f - listener_depth_frac) * depth);

    er->num_taps = ER_NUM_TAPS;

    for (int i = 0; i < ER_NUM_TAPS; i++) {
        int d = (int)(extra[i] / SPEED_OF_SOUND * (float)sample_rate + 0.5f);
        if (d < 1) d = 1;
        if (d >= ER_MAX_DELAY) d = ER_MAX_DELAY - 1;
        er->delays[i] = d;

        float gain = (1.0f - absorption) / (1.0f + extra[i]);

        if (i <= 1) {
            /* Lateral reflections: pan towards reflecting wall */
            float loud = 0.8f * gain;
            float quiet = 0.3f * gain;
            if (i == 0) {
                /* Left wall: boost left ear */
                er->gain_l[i] = loud;
                er->gain_r[i] = quiet;
            } else {
                /* Right wall: boost right ear */
                er->gain_l[i] = quiet;
                er->gain_r[i] = loud;
            }
        } else {
            /* Floor, ceiling, front, back: equal L/R */
            er->gain_l[i] = gain * 0.6f;
            er->gain_r[i] = gain * 0.6f;
        }
    }
}

void er_process(EarlyReflections *er, float *l, float *r, int n) {
    for (int i = 0; i < n; i++) {
        /* Write current sample into circular buffer */
        er->buf_l[er->write_pos] = l[i];
        er->buf_r[er->write_pos] = r[i];

        /* Accumulate all taps */
        float sum_l = 0, sum_r = 0;
        for (int t = 0; t < er->num_taps; t++) {
            int read_pos = er->write_pos - er->delays[t];
            if (read_pos < 0) read_pos += er->buf_size;
            sum_l += er->buf_l[read_pos] * er->gain_l[t];
            sum_r += er->buf_r[read_pos] * er->gain_r[t];
        }

        /* Add reflections to output */
        l[i] += sum_l;
        r[i] += sum_r;

        if (++er->write_pos >= er->buf_size)
            er->write_pos = 0;
    }
}

void er_clear(EarlyReflections *er) {
    if (!er->initialized) return;
    if (er->buf_l) memset(er->buf_l, 0, er->buf_size * sizeof(float));
    if (er->buf_r) memset(er->buf_r, 0, er->buf_size * sizeof(float));
    er->write_pos = 0;
}

void er_destroy(EarlyReflections *er) {
    free(er->buf_l);
    free(er->buf_r);
    memset(er, 0, sizeof(*er));
}
