#pragma once
// SharedState.h - Shared state between mpv's af_hrtf audio filter and the host app
// Communication is lock-free via atomics. Audio thread writes, UI thread reads.

#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#define HRTF_ATOMIC(T) std::atomic<T>
// Provide C-style atomic_load/atomic_store macros for C++ code
#define atomic_load(p)    ((p)->load(std::memory_order_relaxed))
#define atomic_store(p,v) ((p)->store((v), std::memory_order_relaxed))
extern "C" {
#else
#include <stdatomic.h>
#define HRTF_ATOMIC(T) _Atomic T
#endif

#define HRTF_MAX_CHANNELS 16
#define HRTF_MAX_OBJECTS  128

typedef struct {
    float azimuth;    // degrees, 0=front, 90=left, -90=right
    float elevation;  // degrees, 0=ear level, 90=above
    float distance;   // meters
} HrtfPosition;

typedef struct HrtfSharedState {
    // --- Written by audio filter, read by UI ---
    HRTF_ATOMIC(int32_t)  num_channels;
    HRTF_ATOMIC(int32_t)  num_bed_channels;                     // real bed channels (objects start after this)
    HRTF_ATOMIC(float)    channel_rms[HRTF_MAX_CHANNELS];      // per-channel RMS level
    HRTF_ATOMIC(float)    channel_peak[HRTF_MAX_CHANNELS];      // per-channel peak level
    HRTF_ATOMIC(int64_t)  frame_counter;                         // monotonic frame counter
    HRTF_ATOMIC(double)   current_pts;                           // presentation timestamp
    HRTF_ATOMIC(int32_t)  sample_rate;
    HRTF_ATOMIC(int32_t)  active;                                // 1 if filter is processing

    // --- Spatial object data (written by host app / sidecar loader, read by filter) ---
    HRTF_ATOMIC(int32_t)  num_objects;
    HRTF_ATOMIC(float)    object_x[HRTF_MAX_OBJECTS];   // X: 0=L, 1=R
    HRTF_ATOMIC(float)    object_y[HRTF_MAX_OBJECTS];   // Y: 0=front, 1=back
    HRTF_ATOMIC(float)    object_z[HRTF_MAX_OBJECTS];   // Z: -1=floor, 1=ceiling
    HRTF_ATOMIC(float)    object_gain[HRTF_MAX_OBJECTS]; // linear gain hint from metadata (0..1)
    HRTF_ATOMIC(int32_t)  object_active[HRTF_MAX_OBJECTS]; // 1 if object slot is active in current frame
    HRTF_ATOMIC(int32_t)  objects_changed;               // set by host app, cleared by filter

    // --- Written by UI, read by audio filter ---
    // Speaker positions (user can drag these)
    HrtfPosition     speaker_pos[HRTF_MAX_CHANNELS];
    HRTF_ATOMIC(int32_t)  speaker_pos_changed;                   // set by UI, cleared by filter

    // HRTF file path (UI sets this, filter reloads)
    char             sofa_path[512];
    HRTF_ATOMIC(int32_t)  sofa_path_changed;

    // Master volume
    HRTF_ATOMIC(float)    master_volume;                         // 0.0 - 1.0

    // Enable/disable HRTF processing
    HRTF_ATOMIC(int32_t)  hrtf_enabled;

    // Room reverb parameters (set by UI, read by filter)
    HRTF_ATOMIC(int32_t)  reverb_enabled;
    HRTF_ATOMIC(float)    reverb_decay;      // 0.0-1.0, maps to feedback
    HRTF_ATOMIC(float)    reverb_wet;        // 0.0-1.0, wet/dry mix
    HRTF_ATOMIC(float)    reverb_damping;    // 0.0-1.0, high-freq absorption
    HRTF_ATOMIC(float)    reverb_predelay;   // milliseconds
    HRTF_ATOMIC(int32_t)  reverb_changed;    // set by UI, cleared by filter

    // Room geometry for early reflections (set by UI room presets)
    HRTF_ATOMIC(float)    room_width;       // meters
    HRTF_ATOMIC(float)    room_depth;       // meters
    HRTF_ATOMIC(float)    room_height;      // meters
    HRTF_ATOMIC(float)    room_absorption;  // 0.0-1.0
    HRTF_ATOMIC(int32_t)  room_changed;     // set by UI, cleared by filter
    HRTF_ATOMIC(float)    room_gain;        // per-room volume compensation (1.0 = unity)

    // Test tone: UI writes channel + sets active=1, filter clears when done
    HRTF_ATOMIC(int32_t)  test_tone_channel;  // -1 = none, 0..N = channel index
    HRTF_ATOMIC(int32_t)  test_tone_active;   // 1 = playing

    // Debug: solo/mute bed vs objects
    HRTF_ATOMIC(int32_t)  mute_bed;           // 1 = mute channels 0-7 (bed)
    HRTF_ATOMIC(int32_t)  mute_objects;       // 1 = mute channels 8+ (objects)

    // Ambisonic early-reflections send level (0..1).  0 = dry, 1 = wet.
    HRTF_ATOMIC(float)    er_level;

    // Binaural crossfeed amount (-0.3..0.3).
    // Positive (e.g. 0.10) = classical crossfeed, narrows image.
    // Negative (e.g. -0.10) = stereo widener, amplifies ILD so sides feel
    // more lateral (useful when generic HRTF under-lateralises).
    HRTF_ATOMIC(float)    crossfeed;

    // Channel order convention.  Most Atmos/Dolby/DCI content is encoded in
    // SMPTE order (FL FR FC LFE SL SR BL BR) but FFmpeg exposes it in its
    // native WAVE/bit order (FL FR FC LFE BL BR SL SR).  When this flag is
    // 1, we swap speaker positions for the SL/BL and SR/BR pairs so that
    // channels land at the direction their audio was labelled for.
    HRTF_ATOMIC(int32_t)  channel_order_smpte;

    // Cinema screen baffling.  Perforated projection screens attenuate 2-4 dB
    // above 2 kHz for sources placed behind them (FL/FR/FC).  When enabled we
    // apply a gentle high-shelf to those three channels only.
    HRTF_ATOMIC(int32_t)  screen_baffling;

    // Frontal pinna enhancement: +4 dB peak ~4 kHz, -3 dB notch ~8 kHz applied
    // to FL/FR/FC.  Mimics the characteristic HF resonance the pinna produces
    // for sources directly in front, overriding the generic-HRTF front/back
    // ambiguity that causes frontals to feel lateral or behind.  Default on.
    HRTF_ATOMIC(int32_t)  front_pinna_boost;

    // Convolution reverb: stereo impulse response loaded from a WAV file at
    // 48 kHz.  ir_file_path is the absolute/relative path; setting
    // ir_changed=1 signals the audio filter to reload.  ir_wet scales the
    // resulting wet signal before mixing into the binaural output.
    char             ir_file_path[512];
    HRTF_ATOMIC(int32_t)  ir_changed;
    HRTF_ATOMIC(float)    ir_wet;

    // Near-field compensation: low-shelf boost on the proximal ear for
    // sources within ~1.5 m.  Generic HRTFs are measured at 1.5–2 m and
    // underestimate the LF coupling that very close sources produce; this
    // restores the "intimate body" close-mic'd dialogue or hyper-close
    // Atmos objects should have.  Default on.
    HRTF_ATOMIC(int32_t)  near_field_comp;

    // Bauer crossfeed amount (0..0.5).  Mixes a delayed low-passed (~700 Hz)
    // copy of each channel into the opposite ear, simulating the natural
    // low-frequency diffraction around the head.  Unlike broadband crossfeed
    // this preserves HRTF localization cues at HF, only adding body to the
    // contralateral ear — fixes "frontals collapse hard to the side" without
    // blurring side/rear sources.  Default 0.15.
    HRTF_ATOMIC(float)    bauer_crossfeed;

} HrtfSharedState;

// Global shared state instance (allocated by host app, passed to mpv via option)
// The af_hrtf filter reads the pointer from its options.
HrtfSharedState* hrtf_shared_state_create(void);
void hrtf_shared_state_destroy(HrtfSharedState* state);

// Default 7.1.4 speaker positions
void hrtf_shared_state_init_714(HrtfSharedState* state);

#ifdef __cplusplus
}
#endif
