/*
 * HaloSound HRTF Engine - Main implementation
 * Binaural spatialization of multichannel audio using HRTF convolution,
 * Freeverb reverb, and image-source early reflections.
 *
 * Adapted from mpv af_hrtf.c
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "hrtf_engine.h"
#include "hrtf_convolver.h"
#include "freeverb.h"
#include "er_spatial.h"
#include "sofa_loader.h"
#include "channel_layouts.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ======================================================================
 * Engine structure
 * ====================================================================== */

struct HaloEngine {
    int sample_rate;
    int num_channels;

    /* Per-channel double-buffered convolvers */
    HrtfChannelPair channels[HRTF_MAX_CHANNELS];

    /* Current speaker positions */
    HrtfSpeakerPos speaker_pos[HRTF_MAX_CHANNELS];

    /* Per-channel de-interleave buffers */
    float *channel_buf[HRTF_MAX_CHANNELS];

    /* Effects */
    ReverbState reverb;
    Er3dState er3d;

    /* SOFA / HRTF data */
    SofaData *sofa;
    int hrir_length;

    /* Air absorption one-pole state per channel */
    float air_abs_state[HRTF_MAX_CHANNELS];

    /* Minimum speaker distance (for relative attenuation) */
    float min_dist;

    /* Room gain compensation */
    float room_gain;

    /* Output limiter smoothed gain */
    float out_limiter_gain;

    /* Bed/object channel split */
    int num_bed_channels;

    /* Mute flags (set from JS) */
    int mute_bed;
    int mute_obj;

    int initialized;
};

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

static void update_min_dist(HaloEngine *e) {
    float md = 1e9f;
    for (int ch = 0; ch < e->num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
        float d = e->speaker_pos[ch].distance;
        if (d > 0 && d < md) md = d;
    }
    e->min_dist = (md > 0 && md < 1e9f) ? md : 1.0f;
}

static void get_hrir_for_position(HaloEngine *e, float azimuth, float elevation,
                                  float distance, float *hrir_l, float *hrir_r) {
    int len = e->hrir_length;
    if (len <= 0) len = 128;

    /* Spherical to cartesian (libmysofa convention: x=front, y=left, z=up) */
    float az_rad = azimuth * (float)(M_PI / 180.0);
    float el_rad = elevation * (float)(M_PI / 180.0);

    float x = distance * cosf(el_rad) * cosf(az_rad);
    float y = distance * cosf(el_rad) * sinf(az_rad);
    float z = distance * sinf(el_rad);

    float delay_l = 0, delay_r = 0;

    /* Allocate temp buffers for raw HRIR */
    float *ir_l = calloc(len, sizeof(float));
    float *ir_r = calloc(len, sizeof(float));

    sofa_get_hrir(e->sofa, x, y, z, ir_l, ir_r, &delay_l, &delay_r);

    /* Apply ITD: convert delays to samples, normalize to relative */
    float min_delay = delay_l < delay_r ? delay_l : delay_r;
    int dl = (int)((delay_l - min_delay) * e->sample_rate + 0.5f);
    int dr = (int)((delay_r - min_delay) * e->sample_rate + 0.5f);

    int max_shift = len / 4;
    if (dl > max_shift) dl = max_shift;
    if (dr > max_shift) dr = max_shift;

    /* Window the HRIR tail: cosine fade-out over the last 40% */
    {
        int fade_start = (int)(len * 0.6f);
        int fade_len = len - fade_start;
        for (int i = fade_start; i < len; i++) {
            float t = (float)(i - fade_start) / (float)fade_len;
            float w = 0.5f * (1.0f + cosf(t * (float)M_PI));
            ir_l[i] *= w;
            ir_r[i] *= w;
        }
    }

    /* Left ear: shift HRIR right by dl samples (prepend zeros) */
    memset(hrir_l, 0, len * sizeof(float));
    if (len - dl > 0)
        memcpy(hrir_l + dl, ir_l, (len - dl) * sizeof(float));

    /* Right ear: shift HRIR right by dr samples */
    memset(hrir_r, 0, len * sizeof(float));
    if (len - dr > 0)
        memcpy(hrir_r + dr, ir_r, (len - dr) * sizeof(float));

    free(ir_l);
    free(ir_r);
}

static void load_channel_hrir(HaloEngine *e, int ch, int crossfade) {
    if (!e->sofa || e->hrir_length <= 0) return;
    if (ch < 0 || ch >= HRTF_MAX_CHANNELS) return;

    HrtfChannelPair *pair = &e->channels[ch];

    if (crossfade && pair->crossfade_remaining > 0) {
        /* Rate-limit: a crossfade is in flight. Remember that the position
         * moved so the freshest HRIR is loaded as soon as it finishes —
         * otherwise fast update bursts would silently drop their last
         * (= current) position. */
        pair->pending_reload = 1;
        return;
    }

    float *hrir_l = calloc(e->hrir_length, sizeof(float));
    float *hrir_r = calloc(e->hrir_length, sizeof(float));

    get_hrir_for_position(e, e->speaker_pos[ch].azimuth,
                          e->speaker_pos[ch].elevation,
                          e->speaker_pos[ch].distance,
                          hrir_l, hrir_r);

    if (crossfade && pair->left[pair->active_idx].valid) {
        int next = 1 - pair->active_idx;
        convolver_set_hrir(&pair->left[next], hrir_l, e->hrir_length);
        convolver_set_hrir(&pair->right[next], hrir_r, e->hrir_length);
        pair->crossfade_remaining = HRTF_CROSSFADE_LEN;
        pair->active_idx = next;
    } else {
        int slot = pair->active_idx;
        convolver_set_hrir(&pair->left[slot], hrir_l, e->hrir_length);
        convolver_set_hrir(&pair->right[slot], hrir_r, e->hrir_length);
    }

    free(hrir_l);
    free(hrir_r);
}

static void reload_all_hrirs(HaloEngine *e) {
    for (int ch = 0; ch < e->num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
        load_channel_hrir(e, ch, 0); /* Direct set, no crossfade */
    }
    /* Fixed-direction HRIRs for the early-reflection buses. */
    if (e->sofa && e->hrir_length > 0) {
        float *hl = calloc(e->hrir_length, sizeof(float));
        float *hr = calloc(e->hrir_length, sizeof(float));
        for (int b = 0; b < ER3D_NUM_BUS; b++) {
            float az, el;
            er3d_bus_dir(b, &az, &el);
            memset(hl, 0, e->hrir_length * sizeof(float));
            memset(hr, 0, e->hrir_length * sizeof(float));
            get_hrir_for_position(e, az, el, 2.0f, hl, hr);
            er3d_set_bus_hrir(&e->er3d, b, hl, hr, e->hrir_length);
        }
        free(hl);
        free(hr);
    }
    update_min_dist(e);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

HaloEngine* halo_create(int sample_rate, int num_channels) {
    HaloEngine *e = calloc(1, sizeof(HaloEngine));
    if (!e) return NULL;

    e->sample_rate = sample_rate;
    e->num_channels = num_channels;
    e->min_dist = 1.0f;
    e->room_gain = 1.0f;
    e->out_limiter_gain = 1.0f;
    e->hrir_length = 128; /* Default until SOFA loaded */
    e->num_bed_channels = (num_channels > 8) ? 8 : num_channels;
    e->mute_bed = 0;
    e->mute_obj = 0;

    /* Initialize convolvers */
    for (int ch = 0; ch < num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
        HrtfChannelPair *pair = &e->channels[ch];
        pair->active_idx = 0;
        pair->crossfade_remaining = 0;
        for (int i = 0; i < 2; i++) {
            convolver_init(&pair->left[i]);
            convolver_init(&pair->right[i]);
        }
    }

    /* Allocate per-channel buffers */
    for (int ch = 0; ch < num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
        e->channel_buf[ch] = calloc(HRTF_BLOCK_SIZE, sizeof(float));
    }

    /* Set default layout based on channel count */
    int layout = LAYOUT_STEREO;
    if (num_channels >= 16) layout = LAYOUT_714_OBJ;
    else if (num_channels >= 12) layout = LAYOUT_714;
    else if (num_channels >= 8) layout = LAYOUT_71;
    else if (num_channels >= 6) layout = LAYOUT_51;
    channel_layout_get_positions(layout, e->speaker_pos, HRTF_MAX_CHANNELS);

    /* Initialize reverb and spatialized early reflections */
    reverb_init(&e->reverb, sample_rate);
    er3d_init(&e->er3d, sample_rate);

    update_min_dist(e);

    /* Create stub SOFA and load default HRIRs so the engine always
     * produces binaural output even without a real .sofa file. */
    e->sofa = sofa_load_buffer(NULL, 0, sample_rate);
    if (e->sofa) {
        e->hrir_length = e->sofa->ir_length;
        if (e->hrir_length <= 0) e->hrir_length = 128;
        reload_all_hrirs(e);
    }

    e->initialized = 1;

    /* Default room: Home Theater */
    halo_set_room_preset(e, 1);

    return e;
}

void halo_destroy(HaloEngine *e) {
    if (!e) return;

    for (int ch = 0; ch < HRTF_MAX_CHANNELS; ch++) {
        for (int i = 0; i < 2; i++) {
            convolver_destroy(&e->channels[ch].left[i]);
            convolver_destroy(&e->channels[ch].right[i]);
        }
        free(e->channel_buf[ch]);
    }

    reverb_destroy(&e->reverb);
    er3d_destroy(&e->er3d);

    if (e->sofa) sofa_destroy(e->sofa);

    free(e);
}

int halo_load_sofa(HaloEngine *e, const uint8_t *data, int size) {
    if (!e) return -1;

    if (e->sofa) {
        sofa_destroy(e->sofa);
        e->sofa = NULL;
    }

    e->sofa = sofa_load_buffer(data, size, e->sample_rate);
    if (!e->sofa) return -1;

    e->hrir_length = e->sofa->ir_length;
    if (e->hrir_length <= 0) e->hrir_length = 128;
    if (e->hrir_length > HRTF_MAX_HRIR_LEN) e->hrir_length = HRTF_MAX_HRIR_LEN;

    reload_all_hrirs(e);
    return 0;
}

void halo_set_layout(HaloEngine *e, int layout) {
    if (!e) return;

    int n = channel_layout_get_positions(layout, e->speaker_pos, HRTF_MAX_CHANNELS);
    (void)n;

    if (e->sofa) reload_all_hrirs(e);
}

void halo_set_speaker_pos(HaloEngine *e, int ch, float az, float el, float dist) {
    if (!e || ch < 0 || ch >= HRTF_MAX_CHANNELS) return;

    e->speaker_pos[ch].azimuth = az;
    e->speaker_pos[ch].elevation = el;
    e->speaker_pos[ch].distance = dist;

    if (e->sofa) load_channel_hrir(e, ch, 1); /* With crossfade */
    /* Re-project this source's image-source reflections (LFE never
     * reflects — a sub is omnidirectional and only muddies the image). */
    if (ch != 3) er3d_set_source(&e->er3d, ch, az, el, dist);
    update_min_dist(e);
}

/*
 * Apply a room defined by its physical geometry and a mid-band RT60
 * target.  The average absorption follows Sabine's equation
 * α = 0.161·V / (S·RT60) and drives both the image-source reflection
 * gains and the late reverb's HF damping; the reverb feedback is
 * calibrated so the comb loop actually decays at RT60; pre-delay comes
 * from the room's mean free path (4V/S).
 */
static void apply_room(HaloEngine *e, float W, float D, float H,
                       float rt60, float wet, float room_gain) {
    if (W <= 0.1f) {                              /* dry */
        er3d_set_room(&e->er3d, 0, 0, 0, 1.0f, 0.0f);
        e->reverb.enabled = 0;
        e->room_gain = room_gain;
        return;
    }
    const float V = W * D * H;
    const float S = 2.0f * (W * D + W * H + D * H);
    float alpha = 0.161f * V / (S * rt60);
    if (alpha < 0.05f) alpha = 0.05f;
    if (alpha > 0.85f) alpha = 0.85f;

    er3d_set_room(&e->er3d, W, D, H, alpha, 1.0f);
    for (int ch = 0; ch < e->num_channels && ch < HRTF_MAX_CHANNELS; ch++) {
        if (ch == 3) continue;
        er3d_set_source(&e->er3d, ch, e->speaker_pos[ch].azimuth,
                        e->speaker_pos[ch].elevation,
                        e->speaker_pos[ch].distance);
    }

    reverb_update_rt60(&e->reverb, rt60, alpha, wet, V, S, e->sample_rate);
    e->reverb.enabled = (wet > 0.001f) ? 1 : 0;
    e->room_gain = room_gain;
}

void halo_set_room(HaloEngine *e, float width, float depth, float height,
                   float decay, float damping, float wet, float absorption) {
    if (!e) return;
    (void)damping; (void)absorption;   /* derived from geometry now */
    /* Legacy decay knob (0-1) maps onto an RT60 range. */
    float rt60 = 0.15f + decay * 2.35f;
    apply_room(e, width, depth, height, rt60, wet, e->room_gain);
}

void halo_set_room_preset(HaloEngine *e, int preset) {
    if (!e) return;

    /*
     * Rooms defined by published reference geometries and mid-band RT60
     * targets (Sabine derives the rest — see apply_room):
     *  0 Studio        ITU-R BS.1116-3 reference listening room
     *                  (~6.9×4.6×2.8 m, Tm ≈ 0.25 s mid-band)
     *  1 Home Theater  Dolby Atmos home guidelines (~0.35 s, sealed room)
     *  2 Cinema        SMPTE-style mid-size commercial hall (0.6 s @500 Hz)
     *  3 Concert Hall  classical shoebox, V ≈ 15 000 m³ (2.0 s)
     *  4 Living Room   typical untreated domestic room (0.55 s)
     *  5 Screening     post-production review room (0.30 s)
     *  6 IMAX          large but heavily treated auditorium (1.0 s)
     *  7 Dolby Cinema  premium large format, strongly damped (0.5 s)
     *  8 Dry           processing bypassed
     */
    struct {
        float width, depth, height;
        float rt60, wet, room_gain;
    } presets[] = {
        /* 0 */ {  6.9f,  4.6f,  2.8f, 0.25f, 0.016f, 1.0f },
        /* 1 */ {  6.7f,  4.9f,  2.8f, 0.35f, 0.050f, 1.0f },
        /* 2 */ { 20.0f, 14.0f,  9.0f, 0.60f, 0.090f, 1.6f },
        /* 3 */ { 40.0f, 22.0f, 17.0f, 2.00f, 0.160f, 2.2f },
        /* 4 */ {  5.0f,  4.2f,  2.5f, 0.55f, 0.035f, 1.0f },
        /* 5 */ {  9.0f,  6.5f,  3.5f, 0.30f, 0.045f, 1.2f },
        /* 6 */ { 32.0f, 24.0f, 18.0f, 1.00f, 0.120f, 2.0f },
        /* 7 */ { 24.0f, 16.0f, 10.0f, 0.50f, 0.070f, 1.4f },
        /* 8 */ {  0.0f,  0.0f,  0.0f, 0.00f, 0.000f, 1.0f },
    };

    int num_presets = sizeof(presets) / sizeof(presets[0]);
    if (preset < 0 || preset >= num_presets) preset = 1;

    apply_room(e, presets[preset].width, presets[preset].depth,
               presets[preset].height, presets[preset].rt60,
               presets[preset].wet, presets[preset].room_gain);
}

void halo_process(HaloEngine *e, const float *input, float *output_lr,
                  int num_samples) {
    if (!e || !input || !output_lr) return;
    if (num_samples > HRTF_BLOCK_SIZE) num_samples = HRTF_BLOCK_SIZE;
    if (num_samples <= 0) return;

    int num_ch = e->num_channels;
    if (num_ch > HRTF_MAX_CHANNELS) num_ch = HRTF_MAX_CHANNELS;

    /* De-interleave input */
    for (int ch = 0; ch < num_ch; ch++) {
        float *buf = e->channel_buf[ch];
        if (!buf) continue;
        for (int i = 0; i < num_samples; i++) {
            buf[i] = input[i * num_ch + ch];
        }
    }

    /* Output accumulators */
    float out_l[HRTF_BLOCK_SIZE];
    float out_r[HRTF_BLOCK_SIZE];
    memset(out_l, 0, num_samples * sizeof(float));
    memset(out_r, 0, num_samples * sizeof(float));

    float block_l[HRTF_BLOCK_SIZE];
    float block_r[HRTF_BLOCK_SIZE];

    int active_ch_count = 0;

    for (int ch = 0; ch < num_ch; ch++) {
        float *chdata = e->channel_buf[ch];
        if (!chdata) continue;

        HrtfChannelPair *pair = &e->channels[ch];
        int active = pair->active_idx;

        /* Distance attenuation (relative 1/r law) with room gain */
        float dist = e->speaker_pos[ch].distance;
        if (dist < e->min_dist) dist = e->min_dist;
        float dist_gain = (e->min_dist / dist) * e->room_gain;

        /* Smooth elevation attenuation */
        {
            float el_abs = fabsf(e->speaker_pos[ch].elevation);
            if (el_abs > 10.0f) {
                float t = (el_abs - 10.0f) / 80.0f;
                dist_gain *= 1.0f - 0.29f * t * t;
            }
        }

        for (int i = 0; i < num_samples; i++)
            chdata[i] *= dist_gain;

        /* Air absorption — one-pole lowpass (skip LFE, ch 3) */
        if (ch != 3) {
            float fc = 20000.0f / (1.0f + (dist - e->min_dist) * AIR_ABS_FACTOR);
            float coeff = 1.0f - expf(-2.0f * (float)M_PI * fc / (float)e->sample_rate);
            for (int i = 0; i < num_samples; i++) {
                e->air_abs_state[ch] = coeff * chdata[i]
                                     + (1.0f - coeff) * e->air_abs_state[ch];
                chdata[i] = e->air_abs_state[ch];
            }
        }

        /* LFE (channel 3): mix directly without HRTF */
        if (ch == 3) {
            for (int i = 0; i < num_samples; i++) {
                float s = chdata[i] * 0.5f;
                out_l[i] += s;
                out_r[i] += s;
            }
            continue;
        }

        if (!pair->left[active].valid)
            continue;

        active_ch_count++;

        /* Feed the dry signal into the image-source reflection stage; it
         * spatializes each surface bounce from its true arrival direction. */
        er3d_feed(&e->er3d, ch, chdata, num_samples);

        /* Convolve with left and right HRIRs */
        convolver_process(&pair->left[active], chdata, block_l);
        convolver_process(&pair->right[active], chdata, block_r);

        /* Handle crossfade if HRIR was recently changed */
        if (pair->crossfade_remaining > 0) {
            int prev = 1 - active;
            if (pair->left[prev].valid) {
                float prev_l[HRTF_BLOCK_SIZE], prev_r[HRTF_BLOCK_SIZE];
                convolver_process(&pair->left[prev], chdata, prev_l);
                convolver_process(&pair->right[prev], chdata, prev_r);

                for (int i = 0; i < num_samples; i++) {
                    if (pair->crossfade_remaining > 0) {
                        float phase = (1.0f - (float)pair->crossfade_remaining
                                       / HRTF_CROSSFADE_LEN) * ((float)M_PI * 0.5f);
                        float g_new = sinf(phase);
                        float g_old = cosf(phase);
                        block_l[i] = block_l[i] * g_new + prev_l[i] * g_old;
                        block_r[i] = block_r[i] * g_new + prev_r[i] * g_old;
                        pair->crossfade_remaining--;
                    }
                }
            } else {
                pair->crossfade_remaining = 0;
            }
        }

        /* Crossfade done and the position moved meanwhile → chase it */
        if (pair->crossfade_remaining == 0 && pair->pending_reload) {
            pair->pending_reload = 0;
            load_channel_hrir(e, ch, 1);
        }

        /* Accumulate into output */
        for (int i = 0; i < num_samples; i++) {
            out_l[i] += block_l[i];
            out_r[i] += block_r[i];
        }
    }

    /* Adaptive headroom: scale by 1/sqrt(active_channels) */
    if (active_ch_count < 6) active_ch_count = 6;
    const float headroom = 1.0f / sqrtf((float)active_ch_count);
    for (int i = 0; i < num_samples; i++) {
        out_l[i] *= headroom;
        out_r[i] *= headroom;
    }

    /* Spatialized early reflections (fed pre-headroom, so scale to match
     * the direct path). */
    er3d_render(&e->er3d, out_l, out_r, num_samples, headroom);

    /* Reverb */
    if (e->reverb.enabled && e->reverb.initialized) {
        reverb_process(&e->reverb, out_l, out_r, num_samples);
    }

    /* Transparent safety limiter (sample-wise envelope).
     * Avoids frame-wide gain dips that can sound like transient "cuts". */
    {
        const float ceiling = 0.90f;
        const float sr = (float)(e->sample_rate > 0 ? e->sample_rate : 48000);
        const float attack_tc = 0.002f;    /* ~2 ms attack */
        const float release_tc = 0.200f;   /* ~200 ms release */
        const float attack_a = expf(-1.0f / (sr * attack_tc));
        const float release_a = expf(-1.0f / (sr * release_tc));
        float g = e->out_limiter_gain;

        if (!(g > 0.0f) || g > 1.0f)
            g = 1.0f;

        for (int i = 0; i < num_samples; i++) {
            float l = out_l[i];
            float r = out_r[i];
            float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            float target = 1.0f;

            if (peak > ceiling && peak > 1e-12f)
                target = ceiling / peak;

            if (target < g)
                g = attack_a * g + (1.0f - attack_a) * target;
            else
                g = release_a * g + (1.0f - release_a) * target;

            l *= g;
            r *= g;

            if (l > ceiling) l = ceiling;
            else if (l < -ceiling) l = -ceiling;
            if (r > ceiling) r = ceiling;
            else if (r < -ceiling) r = -ceiling;

            out_l[i] = l;
            out_r[i] = r;
        }

        e->out_limiter_gain = g;
    }

    /* Interleave stereo output */
    for (int i = 0; i < num_samples; i++) {
        output_lr[i * 2]     = out_l[i];
        output_lr[i * 2 + 1] = out_r[i];
    }
}

int halo_get_num_channels(HaloEngine *e) {
    return e ? e->num_channels : 0;
}
