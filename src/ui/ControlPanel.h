#pragma once

#include <string>
#include <vector>
#include <cstdint>
struct HrtfSharedState;
class MpvPlayer;

struct HrtfProfile {
    std::string name;        // Display name (derived from filename)
    std::string path;        // Full path to .sofa file
    std::string description; // Filename-guessed hint (pre-metadata fallback)
    bool metaLoaded = false; // Real AES69 metadata read lazily (see cards)
    // AES69 tag fields, shown as chips on the profile card:
    std::string subject;      // ListenerShortName (measured head)
    std::string database;     // DatabaseName
    std::string grid;         // "<M> dirs"
    std::string res;          // "<N> taps @ <sr> kHz"
    std::string license;
    std::string organization;
};

// Spatial object sidecar (.aobj) file format:
// Header: "AOBJ" magic (4 bytes), version (u8), num_frames (u32 LE), fps (f32 LE)
// Per frame: pts_ms (u32 LE), num_objects (u8)
// Per object: x (f32 LE), y (f32 LE), z (f32 LE)
struct AobjFrame {
    uint32_t pts_ms;
    uint8_t num_objects;
    struct { float x, y, z; } objects[128];
};

class ControlPanel {
public:
    ControlPanel(HrtfSharedState* state, int* selectedSpeaker = nullptr,
                 MpvPlayer* player = nullptr);

    // Renders the Spatial tab body inside the Preferences page — HRTF
    // profile, speaker layout, room preset, reverb, advanced toggles,
    // master volume, debug, status.  No Begin/End: caller owns the
    // window/child.
    void renderSpatialContent();

    // Renders the Headphone EQ tab body — EQ profile selection plus the
    // two crossfeed knobs.  These are pure post-processing on the
    // binaural mix, hence the split from the spatial environment knobs.
    void renderEqContent();

    // Speaker list with colour indicators, position sliders, RMS bars
    // and Test buttons.  Hosted by the 3D Visualizer panel as a sidebar
    // (positions only really make sense alongside the 3D view).
    void renderSpeakerList();

    // Call each frame to sync sidecar object positions with current PTS
    void updateObjectPositions();

    // Load/unload sidecar for a media file
    void loadSidecar(const std::string& mediaPath);
    void unloadSidecar();

    ~ControlPanel();

private:
    void scanProfiles();
    void loadProfile(int index);
    HrtfSharedState* m_state;
    int* m_selectedSpeaker = nullptr;
    MpvPlayer* m_player = nullptr;
    char m_sofaPath[512] = {};

    // HRTF ear profiles
    std::vector<HrtfProfile> m_profiles;
    int m_selectedProfile = 0;
    bool m_profilesScanned = false;

    // Convolution reverb IRs
    std::vector<std::string> m_irFiles;   // relative paths under assets/ir/
    int m_selectedIr = 0;                 // 0 = "None"
    bool m_irScanned = false;
    void scanIrs();

    // Headphone EQ profiles
    std::vector<std::string> m_hpEqFiles; // relative paths under assets/headphone_eq/
    int m_selectedHpEq = 0;               // 0 = "None"
    bool m_hpEqScanned = false;
    void scanHpEqs();

    // Spatial object sidecar
    std::vector<AobjFrame> m_sidecarFrames;
    bool m_sidecarLoaded = false;
    int m_lastSidecarIdx = -1;         // last frame index applied
    std::string m_sidecarPath;
};
