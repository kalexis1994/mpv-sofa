/*
 * HaloSound Freeverb - Schroeder reverb implementation
 * Extracted from mpv af_hrtf.c (lines 425-608)
 */

#include <string.h>
#include <stdlib.h>
#include "freeverb.h"

static const int reverb_comb_lengths[REVERB_NUM_COMBS] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
static const int reverb_allpass_lengths[REVERB_NUM_ALLPASS] = {
    556, 441, 341, 225
};

void reverb_init(ReverbState *r, int sample_rate) {
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

void reverb_update(ReverbState *r, float decay, float damping,
                   float wet, float predelay_ms, int sample_rate) {
    if (!r->initialized) return;

    /* Map decay (0-1) to comb feedback (0.7-0.98) */
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
    /* One-pole lowpass in feedback loop */
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

void reverb_process(ReverbState *r, float *l, float *r_ch, int n) {
    if (!r->initialized) return;

    for (int i = 0; i < n; i++) {
        float in_l = l[i];
        float in_r = r_ch[i];

        /* Pre-delay */
        float pd_l = r->predelay_l[r->predelay_pos];
        float pd_r = r->predelay_r[r->predelay_pos];
        r->predelay_l[r->predelay_pos] = in_l;
        r->predelay_r[r->predelay_pos] = in_r;
        if (++r->predelay_pos >= r->predelay_size) r->predelay_pos = 0;

        /* Parallel comb filters */
        static const float fixedgain = 0.05f;
        float scaled_l = pd_l * fixedgain;
        float scaled_r = pd_r * fixedgain;
        float comb_out_l = 0, comb_out_r = 0;
        for (int c = 0; c < REVERB_NUM_COMBS; c++) {
            comb_out_l += comb_process(&r->combs_l[c], scaled_l);
            comb_out_r += comb_process(&r->combs_r[c], scaled_r);
        }

        /* Series allpass filters */
        float ap_l = comb_out_l;
        float ap_r = comb_out_r;
        for (int a = 0; a < REVERB_NUM_ALLPASS; a++) {
            ap_l = allpass_process(&r->allpass_l[a], ap_l);
            ap_r = allpass_process(&r->allpass_r[a], ap_r);
        }

        /* Wet/dry mix */
        l[i] = in_l * r->dry + ap_l * r->wet;
        r_ch[i] = in_r * r->dry + ap_r * r->wet;
    }
}

void reverb_clear(ReverbState *r) {
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

void reverb_destroy(ReverbState *r) {
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
