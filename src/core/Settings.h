#pragma once
// Settings.h — persists user-tweakable state to `mpv-sofa.ini` alongside
// the executable.  Loaded once at startup, saved automatically on shutdown
// (and on demand from the Settings menu) when the in-memory state has
// drifted from the last saved snapshot.

#include <string>

struct HrtfSharedState;
class  MpvPlayer;

namespace Settings {

// Visual styling for rendered subtitles.  Values map straight onto mpv
// properties (`sub-font`, `sub-font-size`, `sub-color`, `sub-border-*`,
// `sub-shadow-*`, `sub-back-color`, `sub-bold`, `sub-margin-y`, `sub-pos`).
// Colours are stored as RGBA 0..1 float arrays so ImGui ColorEdit can
// bind to them directly; the apply-to-player path serialises them as
// `#RRGGBBAA` hex strings before pushing to mpv.
struct SubtitleStyle {
    std::string font          = "";   // "" → mpv default font (Sans)
    float       sizePt        = 42.0f;
    float       color[4]      = { 1.0f, 1.0f, 1.0f, 1.0f };
    float       borderColor[4]= { 0.0f, 0.0f, 0.0f, 1.0f };
    float       borderSize    = 2.5f;
    float       shadowColor[4]= { 0.0f, 0.0f, 0.0f, 0.6f };
    float       shadowOffset  = 2.0f;
    float       backColor[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool        bold          = false;
    int         marginY       = 22;
    int         pos           = 100;  // 0=top, 100=bottom (mpv convention)
};

// Display / HDR pipeline settings — what mpv targets at output.
// mpv with vo=libmpv has no way to detect the host display's
// capabilities, so by default it assumes SDR BT.709 and tone-maps any
// HDR source down.  These knobs let the user opt into HDR10 passthrough
// (e.g. an OLED in filmmaker / HDR mode), pick the tone-mapping
// algorithm used when SDR-mapping is needed, and choose how
// out-of-gamut colours are handled.
struct DisplayConfig {
    int   mode      = 0;       // 0=Auto, 1=Force SDR (BT.709), 2=HDR10 passthrough
    float peakNits  = 1000.0f; // display peak luminance for HDR target
    int   toneAlg   = 0;       // 0=bt.2390 (default), 1=mobius, 2=hable, 3=reinhard, 4=clip
    int   gamutMode = 0;       // 0=auto, 1=perceptual, 2=relative, 3=saturation
};

// Real-time 35mm release-print projection grain emulation, applied
// through mpv's user shader pipeline (assets/shaders/cinema_grain.glsl).
// Models the grain accumulated on the duplicate positive that ran
// through the projector, not the cleaner camera-negative grain.
// GPU-bound — disabled by default; OK on a ROG Ally Z2 Extreme, modern
// dGPUs and APUs, will hurt on older integrated graphics.
struct CinemaGrain {
    bool  enabled     = false;
    int   stock       = 1;     // 0=New release print, 1=Repertory print, 2=Worn 16mm, 3=Custom
    float intensity   = 0.10f; // 0..0.30
    float grainSize   = 1.6f;  // 0.5..4.0 px
    float lumAdaptive = 0.30f; // 0=uniform across tonal range (print-like), 1=peaks at midtones (negative-like)
    float chroma      = 0.4f;  // 0=mono noise, 1=fully decorrelated RGB
};

// Read mpv-sofa.ini.  Missing/unreadable file is not an error — caller's
// defaults stay in place and a fresh snapshot is taken so isDirty() returns
// false until the user changes something.
void load(HrtfSharedState* state, bool* showControls, bool* show3DViz);

// Write the current state to mpv-sofa.ini and refresh the snapshot.
// Returns true on success.
bool save(const HrtfSharedState* state, bool showControls, bool show3DViz);

// True when any tracked field has changed since the last load() or save().
bool isDirty(const HrtfSharedState* state, bool showControls, bool show3DViz);

// Restore factory defaults for every persisted field.  Marks the state
// dirty so the next save() picks up the change.
void resetToDefaults(HrtfSharedState* state, bool* showControls, bool* show3DViz);

// Absolute path the module reads from / writes to (for status display).
const char* filePath();

// User-level preferences applied when a file loads (track picker
// pre-selects the matching audio / subtitle stream when one of these
// is non-empty).  Stored as ISO 639-2/B/T codes ("eng", "spa", …) or
// empty for "no preference".  Values flow through the same dirty +
// auto-save path as the rest of the persisted state.
const std::string& preferredAudioLang();
const std::string& preferredSubLang();
void setPreferredAudioLang(std::string lang);
void setPreferredSubLang(std::string lang);

// True when a track-list lang code (whatever the file has, e.g. "es" or
// "spa") matches a preferences code, including the common 2 ↔ 3 letter
// aliases ("eng" ↔ "en", "spa" ↔ "es", "fre"/"fra" ↔ "fr", …).
bool langMatches(const std::string& trackLang, const std::string& prefLang);

// Last room preset chosen from the Control Panel.  Persisted only as
// a UI hint — the actual room geometry / reverb values are saved as
// individual fields and may have drifted from the named preset if the
// user customised them.
int  roomPreset();
void setRoomPreset(int idx);

// Live subtitle styling — read the current values, write a new copy,
// and push them to a running mpv player.  Persisted in [subtitle_style].
const SubtitleStyle& subtitleStyle();
void                 setSubtitleStyle(const SubtitleStyle& s);
void                 applySubtitleStyleToPlayer(MpvPlayer* p);

// Display / HDR pipeline.  Pushes target-prim, target-trc, target-peak,
// tone-mapping and gamut-mapping-mode to mpv.  Persisted in [display].
const DisplayConfig& displayConfig();
void                 setDisplayConfig(const DisplayConfig& c);
void                 applyDisplayConfigToPlayer(MpvPlayer* p);

// Real-time cinema grain.  Pushes glsl-shaders + glsl-shader-opts onto
// the mpv pipeline.  Persisted in [cinema_grain].
const CinemaGrain& cinemaGrain();
void               setCinemaGrain(const CinemaGrain& g);
void               applyCinemaGrainToPlayer(MpvPlayer* p);

// Advance the grain's time_seed param so the noise pattern changes
// every frame (avoids the "frozen grain" look).  Call once per render
// from the host loop while grain is enabled.  No-op when disabled.
void               tickCinemaGrain(MpvPlayer* p);

} // namespace Settings
