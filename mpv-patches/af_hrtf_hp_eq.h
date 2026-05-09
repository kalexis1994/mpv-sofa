/*
 * Headphone EQ: parametric biquad cascade matching the AutoEQ project's
 * ParametricEq.txt format (https://github.com/jaakkopasanen/AutoEq).
 * Applied to the final stereo output AFTER all spatial DSP and the limiter
 * so the HRTF rendering reaches the listener's ear free of the headphone's
 * own frequency colouration.
 *
 * Requires (from the including TU):
 *   - <math.h>, <stdio.h>, <string.h>
 *   - M_PI defined
 */

#ifndef AF_HRTF_HP_EQ_H
#define AF_HRTF_HP_EQ_H

#define HP_EQ_MAX_BANDS 16

typedef enum {
    HP_EQ_PEAK = 0,
    HP_EQ_LOW_SHELF,
    HP_EQ_HIGH_SHELF,
} HpEqType;

typedef struct {
    HpEqType type;
    float    fc;
    float    gain_db;
    float    q;
} HpEqBand;

typedef struct {
    HpEqBand bands[HP_EQ_MAX_BANDS];
    int      num_bands;
    float    preamp_db;
    float    b0[HP_EQ_MAX_BANDS], b1[HP_EQ_MAX_BANDS], b2[HP_EQ_MAX_BANDS];
    float    a1[HP_EQ_MAX_BANDS], a2[HP_EQ_MAX_BANDS];
    float    state_l[HP_EQ_MAX_BANDS][4];
    float    state_r[HP_EQ_MAX_BANDS][4];
    float    preamp_lin;
    int      valid;
} HpEqProfile;

static void hp_eq_compute_band_coeffs(const HpEqBand *band, float fs,
                                      float *b0, float *b1, float *b2,
                                      float *a1, float *a2) {
    float A = powf(10.0f, band->gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * band->fc / fs;
    float cw = cosf(w0);
    float sw = sinf(w0);
    float Q = band->q > 0.01f ? band->q : 0.7f;
    float alpha = sw / (2.0f * Q);

    float B0, B1, B2, A0, A1, A2;
    if (band->type == HP_EQ_LOW_SHELF) {
        float twoSqrtA_alpha = 2.0f * sqrtf(A) * alpha;
        B0 =     A * ((A + 1.0f) - (A - 1.0f) * cw + twoSqrtA_alpha);
        B1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
        B2 =     A * ((A + 1.0f) - (A - 1.0f) * cw - twoSqrtA_alpha);
        A0 =          (A + 1.0f) + (A - 1.0f) * cw + twoSqrtA_alpha;
        A1 = -2.0f *  ((A - 1.0f) + (A + 1.0f) * cw);
        A2 =          (A + 1.0f) + (A - 1.0f) * cw - twoSqrtA_alpha;
    } else if (band->type == HP_EQ_HIGH_SHELF) {
        float twoSqrtA_alpha = 2.0f * sqrtf(A) * alpha;
        B0 =     A * ((A + 1.0f) + (A - 1.0f) * cw + twoSqrtA_alpha);
        B1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
        B2 =     A * ((A + 1.0f) + (A - 1.0f) * cw - twoSqrtA_alpha);
        A0 =          (A + 1.0f) - (A - 1.0f) * cw + twoSqrtA_alpha;
        A1 =  2.0f *  ((A - 1.0f) - (A + 1.0f) * cw);
        A2 =          (A + 1.0f) - (A - 1.0f) * cw - twoSqrtA_alpha;
    } else { // peaking EQ
        B0 = 1.0f + alpha * A;
        B1 = -2.0f * cw;
        B2 = 1.0f - alpha * A;
        A0 = 1.0f + alpha / A;
        A1 = -2.0f * cw;
        A2 = 1.0f - alpha / A;
    }

    float inv_A0 = 1.0f / A0;
    *b0 = B0 * inv_A0;
    *b1 = B1 * inv_A0;
    *b2 = B2 * inv_A0;
    *a1 = A1 * inv_A0;
    *a2 = A2 * inv_A0;
}

// Parse an AutoEQ ParametricEq.txt file.  Recognises lines of the form:
//   Preamp: -6.0 dB
//   Filter 1: ON PK Fc 50 Hz Gain -3.5 dB Q 1.4
//   Filter 2: ON LSC Fc 105 Hz Gain 6.5 dB Q 0.7
//   Filter 3: ON HS Fc 10000 Hz Gain -2 dB Q 0.7
// Disabled rows (ON OFF / typos) are skipped silently.  Returns 0 on success.
static int hp_eq_parse_file(const char *path, HpEqProfile *out, float fs) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    out->num_bands = 0;
    out->preamp_db = 0.0f;
    out->valid = 0;
    memset(out->state_l, 0, sizeof(out->state_l));
    memset(out->state_r, 0, sizeof(out->state_r));

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;

        float pre;
        if (sscanf(line, " Preamp: %f", &pre) == 1) {
            out->preamp_db = pre;
            continue;
        }

        int idx;
        char on_str[8] = {0}, type_str[8] = {0};
        float fc = 0, gain = 0, q = 0;
        if (sscanf(line, " Filter %d: %3s %3s Fc %f Hz Gain %f dB Q %f",
                   &idx, on_str, type_str, &fc, &gain, &q) == 6) {
            if (strncmp(on_str, "ON", 2) != 0) continue;
            if (out->num_bands >= HP_EQ_MAX_BANDS) continue;

            HpEqType t;
            if (strncmp(type_str, "PK", 2) == 0)       t = HP_EQ_PEAK;
            else if (strncmp(type_str, "LS", 2) == 0)  t = HP_EQ_LOW_SHELF;
            else if (strncmp(type_str, "HS", 2) == 0)  t = HP_EQ_HIGH_SHELF;
            else continue;

            HpEqBand *b = &out->bands[out->num_bands++];
            b->type = t;
            b->fc = fc;
            b->gain_db = gain;
            b->q = q;
        }
    }
    fclose(f);

    if (out->num_bands == 0) return -1;

    // Pre-compute biquad coefficients now so the audio path doesn't have to.
    for (int i = 0; i < out->num_bands; i++) {
        hp_eq_compute_band_coeffs(&out->bands[i], fs,
                                   &out->b0[i], &out->b1[i], &out->b2[i],
                                   &out->a1[i], &out->a2[i]);
    }
    out->preamp_lin = powf(10.0f, out->preamp_db / 20.0f);
    out->valid = 1;
    return 0;
}

static void hp_eq_clear_state(HpEqProfile *p) {
    memset(p->state_l, 0, sizeof(p->state_l));
    memset(p->state_r, 0, sizeof(p->state_r));
}

// Apply biquad cascade in-place to a stereo block.  Each band is a
// Direct-Form-I biquad processed serially per ear.
static void hp_eq_process(HpEqProfile *p, float *l, float *r, int n) {
    if (!p->valid || p->num_bands == 0) return;

    for (int b = 0; b < p->num_bands; b++) {
        const float b0 = p->b0[b], b1 = p->b1[b], b2 = p->b2[b];
        const float a1 = p->a1[b], a2 = p->a2[b];
        float *sl = p->state_l[b];
        float *sr = p->state_r[b];

        for (int i = 0; i < n; i++) {
            float xl = l[i];
            float yl = b0 * xl + b1 * sl[0] + b2 * sl[1] - a1 * sl[2] - a2 * sl[3];
            sl[1] = sl[0]; sl[0] = xl;
            sl[3] = sl[2]; sl[2] = yl;
            l[i] = yl;

            float xr = r[i];
            float yr = b0 * xr + b1 * sr[0] + b2 * sr[1] - a1 * sr[2] - a2 * sr[3];
            sr[1] = sr[0]; sr[0] = xr;
            sr[3] = sr[2]; sr[2] = yr;
            r[i] = yr;
        }
    }

    // Apply preamp last; AutoEQ files use it to absorb the highest peaks
    // and avoid clipping after the boosts.
    if (p->preamp_lin != 1.0f) {
        for (int i = 0; i < n; i++) {
            l[i] *= p->preamp_lin;
            r[i] *= p->preamp_lin;
        }
    }
}

#endif /* AF_HRTF_HP_EQ_H */
