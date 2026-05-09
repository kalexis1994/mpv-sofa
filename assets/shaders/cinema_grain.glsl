//!HOOK OUTPUT
//!BIND HOOKED
//!DESC mpv-sofa 35mm projection grain (gaussian-blurred isotropic noise)
//
//  Real-time analog film grain emulation for mpv.  Replaces the previous
//  cell-noise approach with the technique Marty McFly's METEOR shader
//  and haasn's filmgrain-smooth.glsl converge on, and which is the
//  cheap real-time relative of Newson/Galerne 2017's Boolean model:
//
//   - Per-pixel uniform white noise → no spatial structure / no grid.
//   - 3×3 Gaussian blur, kernel spacing = GRAIN_SIZE px → gives the
//     grain a tunable spatial extent.  The blur is isotropic, so the
//     output has no preferred direction (no stripes, no rotating grid).
//   - By the Central Limit Theorem the blurred output is approximately
//     Gaussian-distributed, which matches the silver-halide statistics
//     Newson/Galerne derive analytically — without the cost of a real
//     Boolean integrator.
//   - Three independent channel hashes give the multi-coupler-layer
//     decorrelation that real print stocks have.
//   - Luminance-adaptive strength keeps grain calm in shadows / clipped
//     highlights, with a low default for the "even projection" look.
//   - Frame-coherent seed (`frame` builtin scrambled by a coprime) so
//     adjacent frames land far apart in seed space → no slow boil that
//     reads as a moving pattern.
//
//  Hooked at OUTPUT so the grain rides over both the video frame AND
//  the composited subtitles — matching how a real 35mm release print
//  carried the grain layer over both image and burned-in subs.
//
//  This file is a TEMPLATE.  Tokens like {{INTENSITY}} are substituted
//  at runtime by Settings::applyCinemaGrainToPlayer; mpv itself sees a
//  regular shader with constants baked in.

#define INTENSITY    {{INTENSITY}}
#define GRAIN_SIZE   {{GRAIN_SIZE}}
#define LUM_ADAPTIVE {{LUM_ADAPTIVE}}
#define CHROMA       {{CHROMA}}

// Integer hash (PCG/Wang family).  The classic `fract(sin(dot(...)))`
// trick produces visible diagonal stripes because sin's iso-value
// curves are parallel to a fixed direction; integer mixers don't have
// any preferred orientation so the noise is truly isotropic.
uint hashUint(uint x) {
    x ^= x >> 17u;
    x *= 0xed5ad4bbu;
    x ^= x >> 11u;
    x *= 0xac4c1b51u;
    x ^= x >> 15u;
    x *= 0x31848babu;
    x ^= x >> 14u;
    return x;
}

float hashUniform(vec2 p, float seed) {
    uint x = floatBitsToUint(p.x);
    uint y = floatBitsToUint(p.y);
    uint s = floatBitsToUint(seed);
    uint h = hashUint(x ^ hashUint(y ^ hashUint(s)));
    return float(h) * (1.0 / 4294967295.0);
}

// Per-pixel three-channel uniform noise centred at zero.
vec3 perPixelNoise(vec2 p, float seed) {
    return vec3(
        hashUniform(p, seed),
        hashUniform(p, seed + 5.123),
        hashUniform(p, seed + 7.456)
    ) - 0.5;
}

// 5×5 isotropic Gaussian-blurred noise at a given spacing.  Sums of
// independent uniform samples become Gaussian-distributed by the
// Central Limit Theorem, so the output already approximates the
// silver-halide statistics Newson/Galerne derive without paying for a
// per-pixel Box-Muller transform.  Kernel spacing scales with `r` so a
// larger value reads as coarser grain without ever introducing a grid.
vec3 gaussianBlurNoise(vec2 px, float seed, float r) {
    vec3 acc = vec3(0.0);
    float wSum = 0.0;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            float d2 = float(dx * dx + dy * dy);
            float w  = exp(-d2 * 0.5);          // unit sigma in cells
            vec2  offset = vec2(float(dx), float(dy)) * r;
            acc  += perPixelNoise(px + offset, seed) * w;
            wSum += w;
        }
    }
    return acc / wSum;
}

vec4 hook() {
    vec4 col = HOOKED_texOff(0);

    vec2 px = HOOKED_pos * HOOKED_size;

    // Decorrelated per-frame seed: coprime multiplier + modulo so adjacent
    // frames land far apart in seed space (no visible "boil" pattern).
    float seed = mod(float(frame) * 7177.0 + 13.0, 9973.0);

    // Two-octave FBM: a dominant coarse layer plus a finer second
    // octave at half scale and lower amplitude.  Real silver-halide
    // grain has structure at multiple spatial frequencies; blending two
    // independent Gaussian-blurred noise layers gives the output a
    // richer texture than a single octave can.  Both layers share the
    // same isotropic per-pixel noise base so neither introduces
    // directional bias.
    float r0 = max(GRAIN_SIZE, 0.5);
    float r1 = r0 * 0.5;
    vec3 n0 = gaussianBlurNoise(px, seed,         r0);
    vec3 n1 = gaussianBlurNoise(px, seed + 137.0, r1);
    vec3 n  = n0 * 0.7 + n1 * 0.3;

    // Per-channel decorrelation: chroma=0 collapses to a single luminance
    // pattern, chroma=1 keeps R/G/B fully independent.
    float nAvg = (n.r + n.g + n.b) / 3.0;
    n = mix(vec3(nAvg), n, CHROMA);

    // Luminance-adaptive strength: peak in mid-tones, fall off at the
    // extremes.  Mixed against a flat 1.0 so the user can dial back to a
    // perfectly uniform "projection" feel.
    float luma     = dot(col.rgb, vec3(0.2126, 0.7152, 0.0722));
    float lumCurve = 1.0 - 4.0 * (luma - 0.5) * (luma - 0.5);
    lumCurve = clamp(lumCurve, 0.0, 1.0);
    float strength = mix(1.0, lumCurve, LUM_ADAPTIVE) * INTENSITY;

    col.rgb += n * strength;
    return col;
}
