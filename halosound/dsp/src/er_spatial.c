#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "er_spatial.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ER3D_SPEED_OF_SOUND 343.0f
#define ER3D_EAR_HEIGHT     1.2f
/* Listener sits 1/3 of the room depth from the back wall (matches the
 * historical layout of the old stereo-tap stage). */
#define ER3D_BACK_FRAC      (1.0f / 3.0f)

/* Bus render directions: 4 horizontal + upper pair + lower pair.
 * az: 0 front, 90 left, 180 back, -90/270 right. */
static const struct { float az, el; } BUS_DIR[ER3D_NUM_BUS] = {
    {   0.0f,  0.0f },   /* 0 front  */
    {  90.0f,  0.0f },   /* 1 left   */
    { 180.0f,  0.0f },   /* 2 back   */
    { -90.0f,  0.0f },   /* 3 right  */
    {  55.0f, 55.0f },   /* 4 up-left    (ceiling) */
    { -55.0f, 55.0f },   /* 5 up-right   (ceiling) */
    {  45.0f, -45.0f },  /* 6 down-left  (floor)   */
    { -45.0f, -45.0f },  /* 7 down-right (floor)   */
};

void er3d_bus_dir(int idx, float *az, float *el) {
    if (idx < 0 || idx >= ER3D_NUM_BUS) { *az = 0; *el = 0; return; }
    *az = BUS_DIR[idx].az;
    *el = BUS_DIR[idx].el;
}

void er3d_init(Er3dState *er, int sample_rate) {
    memset(er, 0, sizeof(*er));
    er->sample_rate = sample_rate;
    for (int b = 0; b < ER3D_NUM_BUS; b++) {
        convolver_init(&er->bus[b].conv_l);
        convolver_init(&er->bus[b].conv_r);
        er->bus[b].ring = calloc(ER3D_RING, sizeof(float));
    }
    er->initialized = 1;
}

void er3d_destroy(Er3dState *er) {
    if (!er->initialized) return;
    for (int b = 0; b < ER3D_NUM_BUS; b++) {
        convolver_destroy(&er->bus[b].conv_l);
        convolver_destroy(&er->bus[b].conv_r);
        free(er->bus[b].ring);
    }
    memset(er, 0, sizeof(*er));
}

void er3d_clear(Er3dState *er) {
    if (!er->initialized) return;
    for (int b = 0; b < ER3D_NUM_BUS; b++)
        if (er->bus[b].ring) memset(er->bus[b].ring, 0, ER3D_RING * sizeof(float));
    er->write_pos = 0;
}

void er3d_set_bus_hrir(Er3dState *er, int idx,
                       const float *hl, const float *hr, int len) {
    if (!er->initialized || idx < 0 || idx >= ER3D_NUM_BUS) return;
    convolver_set_hrir(&er->bus[idx].conv_l, hl, len);
    convolver_set_hrir(&er->bus[idx].conv_r, hr, len);
    er->bus[idx].valid = 1;
}

/* Split one arrival direction across the fixed buses. Elevated arrivals go
 * to the ceiling/floor pairs (lateral balance preserved); horizontal ones
 * interpolate between the two nearest of the 4 horizontal buses. */
static void map_direction(float az_deg, float el_deg,
                          int *b0, int *b1, float *w0, float *w1) {
    if (el_deg > 30.0f || el_deg < -25.0f) {
        int up = el_deg > 0.0f;
        float lw = 0.5f + 0.5f * sinf(az_deg * (float)M_PI / 180.0f);
        if (lw < 0.0f) lw = 0.0f;
        if (lw > 1.0f) lw = 1.0f;
        *b0 = up ? 4 : 6;          /* left of pair */
        *b1 = up ? 5 : 7;          /* right of pair */
        *w0 = lw;
        *w1 = 1.0f - lw;
        return;
    }
    /* Horizontal ring: front(0)=0°, left(1)=90°, back(2)=180°, right(3)=270° */
    float a = fmodf(az_deg, 360.0f);
    if (a < 0.0f) a += 360.0f;
    float pos = a / 90.0f;                 /* 0..4 around the ring */
    int i0 = ((int)pos) & 3;
    int i1 = (i0 + 1) & 3;
    float frac = pos - (float)((int)pos);
    *b0 = i0; *b1 = i1;
    *w0 = 1.0f - frac;
    *w1 = frac;
}

void er3d_disable_source(Er3dState *er, int src) {
    if (!er->initialized || src < 0 || src >= ER3D_MAX_SRC) return;
    er->src_active[src] = 0;
}

void er3d_set_source(Er3dState *er, int src, float az_deg, float el_deg,
                     float dist) {
    if (!er->initialized || src < 0 || src >= ER3D_MAX_SRC) return;
    if (er->width <= 0.1f) { er->src_active[src] = 0; return; }

    /* Crossfade from the current taps: snapshot them as "old" and restart
     * the fade. If a previous fade was still running, freeze its blend as
     * the new starting point isn't tracked per-tap — close enough at
     * 512-sample fades. */
    if (er->src_active[src]) {
        memcpy(er->tap_old[src], er->tap[src], sizeof(er->tap[src]));
        er->fade_pos[src] = 0;
    } else {
        er->fade_pos[src] = ER3D_FADE;   /* first placement: no fade */
    }

    const float W = er->width, D = er->depth, H = er->height;
    const float az = az_deg * (float)M_PI / 180.0f;
    const float el = el_deg * (float)M_PI / 180.0f;

    /* Listener at origin, ear height above the floor. Room extents:
     * x ∈ [-W/2, W/2] (x<0 = left), y ∈ [-D*back, D*(1-back)] (y>0 front),
     * z ∈ [-earH, H-earH]. */
    float sx = -sinf(az) * cosf(el) * dist;
    float sy =  cosf(az) * cosf(el) * dist;
    float sz =  sinf(el) * dist;

    const float xmin = -W * 0.5f, xmax = W * 0.5f;
    const float ymin = -D * ER3D_BACK_FRAC, ymax = D * (1.0f - ER3D_BACK_FRAC);
    const float zmin = -ER3D_EAR_HEIGHT, zmax = H - ER3D_EAR_HEIGHT;

    /* Keep the source inside the room so every image lands outside it. */
    const float m = 0.92f;
    if (sx < xmin * m) sx = xmin * m;
    if (sx > xmax * m) sx = xmax * m;
    if (sy < ymin * m) sy = ymin * m;
    if (sy > ymax * m) sy = ymax * m;
    if (sz < zmin * m) sz = zmin * m;
    if (sz > zmax * m) sz = zmax * m;

    float d_src = sqrtf(sx * sx + sy * sy + sz * sz);
    if (d_src < 0.3f) d_src = 0.3f;

    /* Image sources: reflect across each of the 6 surfaces. */
    const float planes[ER3D_NUM_SURF][2] = {
        /* {axis, coordinate}: axis 0=x 1=y 2=z */
        { 0, xmin }, { 0, xmax },   /* left, right wall  */
        { 1, ymin }, { 1, ymax },   /* back, front wall  */
        { 2, zmin }, { 2, zmax },   /* floor, ceiling    */
    };

    for (int s = 0; s < ER3D_NUM_SURF; s++) {
        float ix = sx, iy = sy, iz = sz;
        int axis = (int)planes[s][0];
        float c  = planes[s][1];
        if (axis == 0) ix = 2.0f * c - sx;
        else if (axis == 1) iy = 2.0f * c - sy;
        else iz = 2.0f * c - sz;

        float d_img = sqrtf(ix * ix + iy * iy + iz * iz);
        if (d_img < d_src + 0.01f) d_img = d_src + 0.01f;

        float extra = d_img - d_src;
        int delay = (int)(extra / ER3D_SPEED_OF_SOUND *
                          (float)er->sample_rate + 0.5f);
        if (delay < 8) delay = 8;
        if (delay > ER3D_RING - HRTF_BLOCK_SIZE - 1)
            delay = ER3D_RING - HRTF_BLOCK_SIZE - 1;

        /* Absorption on the bounce + spherical spreading relative to the
         * direct path (which the engine attenuates separately). */
        float gain = er->level * (1.0f - er->alpha) * (d_src / d_img);
        if (gain > 0.85f) gain = 0.85f;

        float az_i = atan2f(-ix, iy) * 180.0f / (float)M_PI;
        float el_i = asinf(iz / d_img) * 180.0f / (float)M_PI;

        Er3dTap *t = &er->tap[src][s];
        map_direction(az_i, el_i, &t->bus0, &t->bus1, &t->w0, &t->w1);
        t->delay  = delay;
        t->gain   = gain;
        t->active = 1;
    }
    er->src_active[src] = 1;
}

void er3d_set_room(Er3dState *er, float width, float depth, float height,
                   float alpha, float level) {
    if (!er->initialized) return;
    er->width  = width;
    er->depth  = depth;
    er->height = height;
    er->alpha  = alpha;
    er->level  = level;
    if (width <= 0.1f) {
        for (int s = 0; s < ER3D_MAX_SRC; s++) er->src_active[s] = 0;
        er3d_clear(er);
    }
    /* Sources are re-projected by the engine after a room change. */
}

static void er3d_feed_taps(Er3dState *er, const Er3dTap *taps,
                           const float *x, const float *w, int n) {
    const int wp = er->write_pos;
    for (int s = 0; s < ER3D_NUM_SURF; s++) {
        const Er3dTap *t = &taps[s];
        if (!t->active || t->gain < 1e-4f) continue;
        float *r0 = er->bus[t->bus0].ring;
        float *r1 = er->bus[t->bus1].ring;
        const float g0 = t->gain * t->w0;
        const float g1 = t->gain * t->w1;
        const int base = wp + t->delay;
        for (int i = 0; i < n; i++) {
            const int idx = (base + i) & ER3D_RING_MASK;
            const float xv = x[i] * w[i];
            r0[idx] += xv * g0;
            r1[idx] += xv * g1;
        }
    }
}

void er3d_feed(Er3dState *er, int src, const float *x, int n) {
    if (!er->initialized || er->width <= 0.1f) return;
    if (src < 0 || src >= ER3D_MAX_SRC || !er->src_active[src]) return;
    if (n > HRTF_BLOCK_SIZE) n = HRTF_BLOCK_SIZE;

    float w_new[HRTF_BLOCK_SIZE];
    const int fp = er->fade_pos[src];
    if (fp >= ER3D_FADE) {
        for (int i = 0; i < n; i++) w_new[i] = 1.0f;
        er3d_feed_taps(er, er->tap[src], x, w_new, n);
        return;
    }

    /* Crossfade: old taps fade out while the recomputed ones fade in. */
    float w_old[HRTF_BLOCK_SIZE];
    for (int i = 0; i < n; i++) {
        float w = (float)(fp + i) / (float)ER3D_FADE;
        if (w > 1.0f) w = 1.0f;
        w_new[i] = w;
        w_old[i] = 1.0f - w;
    }
    er3d_feed_taps(er, er->tap[src], x, w_new, n);
    er3d_feed_taps(er, er->tap_old[src], x, w_old, n);
    er->fade_pos[src] = fp + n;
}

void er3d_render(Er3dState *er, float *out_l, float *out_r, int n,
                 float master) {
    if (!er->initialized || er->width <= 0.1f) {
        return;
    }
    float in[HRTF_BLOCK_SIZE];
    float bl[HRTF_BLOCK_SIZE], br[HRTF_BLOCK_SIZE];
    const int wp = er->write_pos;

    for (int b = 0; b < ER3D_NUM_BUS; b++) {
        Er3dBus *bus = &er->bus[b];

        /* Consume the ring even when the bus has no HRIR yet, so no stale
         * energy bursts out when the SOFA profile lands. */
        for (int i = 0; i < n; i++) {
            const int idx = (wp + i) & ER3D_RING_MASK;
            in[i] = bus->ring[idx];
            bus->ring[idx] = 0.0f;
        }
        if (!bus->valid) continue;
        for (int i = n; i < HRTF_BLOCK_SIZE; i++) in[i] = 0.0f;

        convolver_process(&bus->conv_l, in, bl);
        convolver_process(&bus->conv_r, in, br);
        for (int i = 0; i < n; i++) {
            out_l[i] += bl[i] * master;
            out_r[i] += br[i] * master;
        }
    }
    er->write_pos = (wp + n) & ER3D_RING_MASK;
}
