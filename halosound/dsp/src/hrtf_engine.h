/*
 * HaloSound HRTF Engine - Main API
 * Binaural spatialization of multichannel audio
 */

#ifndef HRTF_ENGINE_H
#define HRTF_ENGINE_H

#include <stdint.h>
#include "channel_layouts.h"

typedef struct HaloEngine HaloEngine;

/* Create engine for given sample rate and channel count */
HaloEngine* halo_create(int sample_rate, int num_channels);

/* Destroy engine and free all resources */
void halo_destroy(HaloEngine* engine);

/* Load HRTF from SOFA file in memory buffer. Returns 0 on success. */
int halo_load_sofa(HaloEngine* engine, const uint8_t* data, int size);

/* Set speaker layout preset (LAYOUT_STEREO, LAYOUT_51, LAYOUT_71, LAYOUT_714) */
void halo_set_layout(HaloEngine* engine, int layout);

/* Set individual speaker position (azimuth degrees, elevation degrees, distance meters) */
void halo_set_speaker_pos(HaloEngine* engine, int ch, float az, float el, float dist);

/* Configure room reverb and early reflections */
void halo_set_room(HaloEngine* engine, float width, float depth, float height,
                   float decay, float damping, float wet, float absorption);

/* Apply room preset: 0=Studio, 1=Home Theater, 2=Cinema, 3=Concert Hall */
void halo_set_room_preset(HaloEngine* engine, int preset);

/* Process audio block.
 * input:     interleaved multichannel (num_channels * num_samples floats)
 * output_lr: interleaved stereo output (num_samples * 2 floats)
 * num_samples: must be <= 256 (HRTF_BLOCK_SIZE) */
void halo_process(HaloEngine* engine, const float* input,
                  float* output_lr, int num_samples);

/* Get current channel count */
int halo_get_num_channels(HaloEngine* engine);

#endif
