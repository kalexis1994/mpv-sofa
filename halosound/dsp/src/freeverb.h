/*
 * HaloSound Freeverb - Schroeder reverb
 * Extracted from mpv af_hrtf filter
 */

#ifndef FREEVERB_H
#define FREEVERB_H

#define REVERB_NUM_COMBS     8
#define REVERB_NUM_ALLPASS   4
#define REVERB_STEREO_SPREAD 23
#define REVERB_MAX_PREDELAY  8192

typedef struct {
    float *buffer;
    int size, pos;
    float feedback, damp1, damp2, filterstore;
} ReverbComb;

typedef struct {
    float *buffer;
    int size, pos;
    float feedback;
} ReverbAllpass;

typedef struct {
    ReverbComb combs_l[REVERB_NUM_COMBS], combs_r[REVERB_NUM_COMBS];
    ReverbAllpass allpass_l[REVERB_NUM_ALLPASS], allpass_r[REVERB_NUM_ALLPASS];
    float *predelay_l, *predelay_r;
    int predelay_size, predelay_pos;
    float wet, dry;
    int enabled, initialized;
} ReverbState;

void reverb_init(ReverbState *r, int sample_rate);
void reverb_update(ReverbState *r, float decay, float damping,
                   float wet, float predelay_ms, int sample_rate);
void reverb_process(ReverbState *r, float *l, float *r_ch, int n);
void reverb_clear(ReverbState *r);
void reverb_destroy(ReverbState *r);

#endif
