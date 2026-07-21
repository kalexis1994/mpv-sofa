/*
 * HaloSound Spatialized Early Reflections
 *
 * Image-source model, first order: for every source and every room surface
 * (4 walls, floor, ceiling) the reflection's true arrival direction, extra
 * path delay and absorption/distance gain are computed from the room
 * geometry. Each reflection is then rendered BINAURALLY by panning it into
 * a small set of fixed-direction HRIR buses (virtual speakers), so lateral
 * reflections genuinely arrive from the side and ceiling bounces from
 * above — which is what creates externalization. Cost stays flat: 8 bus
 * convolution pairs total, independent of source count.
 */

#ifndef ER_SPATIAL_H
#define ER_SPATIAL_H

#include "hrtf_convolver.h"

#define ER3D_NUM_BUS   8
#define ER3D_MAX_SRC   HRTF_MAX_CHANNELS
#define ER3D_NUM_SURF  6
/* 16384 samples ≈ 341 ms @48k — covers first-order extra paths of the
 * largest preset (concert hall). Power of two for cheap masking. */
#define ER3D_RING      16384
#define ER3D_RING_MASK (ER3D_RING - 1)

typedef struct {
    int valid;
    HrtfConvolver conv_l, conv_r;
    float *ring;                  /* mono scatter-add delay line */
} Er3dBus;

typedef struct {
    int   active;
    int   delay;                  /* samples of extra path vs direct */
    float gain;
    int   bus0, bus1;
    float w0, w1;
} Er3dTap;

typedef struct {
    int initialized;
    int sample_rate;
    int write_pos;
    Er3dBus bus[ER3D_NUM_BUS];
    Er3dTap tap[ER3D_MAX_SRC][ER3D_NUM_SURF];
    int src_active[ER3D_MAX_SRC];
    float width, depth, height;   /* room (m); 0 = ER disabled */
    float alpha;                  /* average absorption (Sabine) */
    float level;                  /* master ER level */
} Er3dState;

void er3d_init(Er3dState *er, int sample_rate);
void er3d_destroy(Er3dState *er);
void er3d_clear(Er3dState *er);

/* Room geometry + absorption. width==0 disables the stage entirely. */
void er3d_set_room(Er3dState *er, float width, float depth, float height,
                   float alpha, float level);

/* (Re)place one source. Engine speaker convention: az 0=front 90=left,
 * el 0=ear level 90=up, dist in meters. */
void er3d_set_source(Er3dState *er, int src, float az_deg, float el_deg,
                     float dist);
void er3d_disable_source(Er3dState *er, int src);

/* Fixed bus render directions (engine az/el convention). */
void er3d_bus_dir(int idx, float *az, float *el);
void er3d_set_bus_hrir(Er3dState *er, int idx,
                       const float *hl, const float *hr, int len);

/* Per block: feed each source's dry signal, then render all buses once. */
void er3d_feed(Er3dState *er, int src, const float *x, int n);
void er3d_render(Er3dState *er, float *out_l, float *out_r, int n,
                 float master);

#endif
