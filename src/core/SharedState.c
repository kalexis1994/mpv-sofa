#include "SharedState.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

HrtfSharedState* hrtf_shared_state_create(void) {
    HrtfSharedState* s = (HrtfSharedState*)calloc(1, sizeof(HrtfSharedState));
    if (!s) return NULL;

    atomic_store(&s->master_volume, 1.0f);
    atomic_store(&s->hrtf_enabled, 1);
    atomic_store(&s->active, 0);
    atomic_store(&s->num_bed_channels, 12);

    // Test tone: inactive by default
    atomic_store(&s->test_tone_channel, -1);
    atomic_store(&s->test_tone_active, 0);

    // Default reverb: Home Theater
    atomic_store(&s->reverb_enabled, 1);
    atomic_store(&s->reverb_decay, 0.45f);
    atomic_store(&s->reverb_wet, 0.15f);
    atomic_store(&s->reverb_damping, 0.5f);
    atomic_store(&s->reverb_predelay, 10.0f);

    // Ambisonic early-reflections wet send (Steam Audio-style spatialised ER).
    atomic_store(&s->er_level, 0.3f);

    // Crossfeed: start at 0 now that the HRTF pipeline is correct.  Users
    // can dial in a tiny amount if stereo music sounds too lateralised.
    atomic_store(&s->crossfeed, 0.0f);

    // Default OFF so we respect FFmpeg's reported chmap.  Enable from the UI
    // when playing Atmos/DCI content where SL/BL and SR/BR arrive swapped.
    atomic_store(&s->channel_order_smpte, 0);

    // Screen baffling off by default; enabled automatically with Cinema/LF/GS
    // presets once the ControlPanel syncs.
    atomic_store(&s->screen_baffling, 0);

    // Frontal pinna boost on by default — single biggest lever for fixing
    // front/back confusion with a non-individual HRTF.
    atomic_store(&s->front_pinna_boost, 1);

    // IR reverb: no file loaded by default.  User picks one from the UI.
    s->ir_file_path[0] = '\0';
    atomic_store(&s->ir_changed, 0);
    atomic_store(&s->ir_wet, 0.35f);

    // Near-field compensation on by default — only active for sources
    // within 1.5 m, so it has no effect on far-field cinema content.
    atomic_store(&s->near_field_comp, 1);

    // Bauer crossfeed: gentle default, gives the contralateral ear body for
    // frontal sources without blurring lateralised side/rear sources.
    atomic_store(&s->bauer_crossfeed, 0.15f);

    hrtf_shared_state_init_714(s);
    return s;
}

void hrtf_shared_state_destroy(HrtfSharedState* state) {
    free(state);
}

void hrtf_shared_state_init_714(HrtfSharedState* state) {
    // 7.1.4 immersive spatial audio speaker layout
    // Channel order: FL FR FC LFE BL BR SL SR TFL TFR TBL TBR
    const float dist = 2.0f;

    //                     azimuth  elevation  distance
    state->speaker_pos[0]  = (HrtfPosition){  30.0f,  0.0f, dist};  // FL
    state->speaker_pos[1]  = (HrtfPosition){ -30.0f,  0.0f, dist};  // FR
    state->speaker_pos[2]  = (HrtfPosition){   0.0f,  0.0f, dist};  // FC
    state->speaker_pos[3]  = (HrtfPosition){   0.0f,  0.0f, dist};  // LFE (no HRTF)
    state->speaker_pos[4]  = (HrtfPosition){ 135.0f,  0.0f, dist};  // BL
    state->speaker_pos[5]  = (HrtfPosition){-135.0f,  0.0f, dist};  // BR
    state->speaker_pos[6]  = (HrtfPosition){  90.0f,  0.0f, dist};  // SL
    state->speaker_pos[7]  = (HrtfPosition){ -90.0f,  0.0f, dist};  // SR
    state->speaker_pos[8]  = (HrtfPosition){  45.0f, 45.0f, dist};  // TFL
    state->speaker_pos[9]  = (HrtfPosition){ -45.0f, 45.0f, dist};  // TFR
    state->speaker_pos[10] = (HrtfPosition){ 135.0f, 45.0f, dist};  // TBL
    state->speaker_pos[11] = (HrtfPosition){-135.0f, 45.0f, dist};  // TBR

    atomic_store(&state->num_channels, 12);
    atomic_store(&state->num_bed_channels, 12);
}
