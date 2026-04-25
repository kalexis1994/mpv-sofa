#pragma once

#include <string>
#include <vector>
#include <cstdint>
struct HrtfSharedState;
class MpvPlayer;

struct HrtfProfile {
    std::string name;        // Display name (derived from filename)
    std::string path;        // Full path to .sofa file
    std::string description; // Optional description
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
    void render();

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
    int m_selectedLayout = 0;  // 0=7.1.4, 1=7.1, 2=6.1, 3=5.1, 4=stereo
    int m_selectedRoom = 1;    // 0=studio, 1=home, 2=cinema, 3=concert

    // Auto-sync the layout dropdown whenever the audio filter reports a
    // different channel count (e.g. when a new file/track is opened).
    int m_lastSeenNumChannels = -1;

    // HRTF ear profiles
    std::vector<HrtfProfile> m_profiles;
    int m_selectedProfile = 0;
    bool m_profilesScanned = false;

    // Convolution reverb IRs
    std::vector<std::string> m_irFiles;   // relative paths under assets/ir/
    int m_selectedIr = 0;                 // 0 = "None"
    bool m_irScanned = false;
    void scanIrs();

    // Spatial object sidecar
    std::vector<AobjFrame> m_sidecarFrames;
    bool m_sidecarLoaded = false;
    int m_lastSidecarIdx = -1;         // last frame index applied
    std::string m_sidecarPath;
};
