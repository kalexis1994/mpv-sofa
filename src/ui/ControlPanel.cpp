#include "ControlPanel.h"
#include "core/SharedState.h"
#include "audio/MpvPlayer.h"
#include <imgui.h>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static const char* speakerNames[] = {
    "Front Left (FL)", "Front Right (FR)", "Center (FC)", "LFE",
    "Back Left (BL)", "Back Right (BR)", "Side Left (SL)", "Side Right (SR)",
    "Top Front Left (TFL)", "Top Front Right (TFR)",
    "Top Back Left (TBL)", "Top Back Right (TBR)"
};

static const char* layoutNames[] = {
    "7.1.4 Spatial", "7.1 Surround", "6.1 Surround", "5.1 Surround", "Stereo"
};

// Per-channel label lookup.  7.1.4 uses the full speakerNames[] above; 6.1
// has a different tail (BC / SL / SR at indices 4..6).
static const char* speakerName61[] = {
    "Front Left (FL)", "Front Right (FR)", "Front Center (FC)", "LFE",
    "Back Center (BC)", "Side Left (SL)", "Side Right (SR)"
};

// Room/environment presets - positions for all 12 channels (7.1.4)
// Channels: FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR
// Distances and angles based on real-world room measurements.
// Reverb params derived from Sabine equation: RT60 = 0.161 * V / A
struct RoomPreset {
    const char* name;
    const char* description;
    float width, depth, height;   // room dimensions (meters)
    float absorption;             // average absorption coefficient
    // Reverb params (derived from dimensions)
    float reverb_decay;           // 0-1 (maps to feedback)
    float reverb_wet;             // 0-1 wet mix
    float reverb_damping;         // 0-1 HF absorption
    float reverb_predelay;        // ms
    float room_gain;              // volume compensation for distance (1.0 = nearfield ref)
    float er_level;               // 0-1 ambisonic ER send level
    HrtfPosition positions[12];
};

static const RoomPreset roomPresets[] = {
    // --- Studio (Nearfield) ---
    // Small control room ~4.5 x 3.5 x 2.8m, heavily treated
    // Volume=44m3, Absorption=0.65 -> RT60 ~0.15s
    {"Studio (Nearfield)",
     "4.5x3.5x2.8m control room, acoustic treatment",
     4.5f, 3.5f, 2.8f, 0.65f,
     0.10f, 0.014f, 0.7f, 2.0f,  // very dry, almost no reverb
     1.0f,  // room_gain: reference level (nearfield)
     0.05f, // er_level: tiny — treated room has few reflections
     {
        { 30.0f,   0.0f, 1.2f},  // FL  - nearfield monitors on desk
        {-30.0f,   0.0f, 1.2f},  // FR
        {  0.0f,   0.0f, 1.2f},  // FC  - center screen
        {  0.0f, -20.0f, 0.8f},  // LFE - under desk
        {135.0f,   0.0f, 1.5f},  // BL  - rear wall mounts
        {-135.0f,  0.0f, 1.5f},  // BR
        { 90.0f,   0.0f, 1.2f},  // SL  - side wall
        {-90.0f,   0.0f, 1.2f},  // SR
        { 45.0f,  40.0f, 1.3f},  // TFL - ceiling mounts
        {-45.0f,  40.0f, 1.3f},  // TFR
        {135.0f,  40.0f, 1.3f},  // TBL
        {-135.0f, 40.0f, 1.3f},  // TBR
    }},

    // --- Home Theater ---
    // Typical living room ~6.5 x 5 x 2.7m
    // Volume=88m3, Absorption=0.35 -> RT60 ~0.4s
    {"Home Theater",
     "6.5x5x2.7m living room, immersive recommended angles",
     6.5f, 5.0f, 2.7f, 0.35f,
     0.45f, 0.068f, 0.5f, 8.0f,  // moderate reverb, carpeted room
     1.0f,  // room_gain: standard home reference
     0.25f, // er_level: modest — domestic reflection density
     {
        { 30.0f,   0.0f, 3.0f},  // FL  - flanking TV/screen
        {-30.0f,   0.0f, 3.0f},  // FR
        {  0.0f,   0.0f, 3.0f},  // FC  - center channel above/below TV
        {  0.0f,   0.0f, 3.0f},  // LFE - front, near floor
        {135.0f,   0.0f, 2.5f},  // BL  - rear wall or stands
        {-135.0f,  0.0f, 2.5f},  // BR
        { 90.0f,   5.0f, 2.0f},  // SL  - side wall, ear level+
        {-90.0f,   5.0f, 2.0f},  // SR
        { 45.0f,  45.0f, 2.8f},  // TFL - ceiling or high wall mounts
        {-45.0f,  45.0f, 2.8f},  // TFR
        {135.0f,  45.0f, 2.8f},  // TBL
        {-135.0f, 45.0f, 2.8f},  // TBR
    }},

    // --- Cinema ---
    // Medium auditorium ~22 x 16 x 9m, listener at 2/3 depth (~7m from screen)
    // Volume=3168m3, Absorption=0.25 -> RT60 ~0.6s (modern cinemas are well-damped)
    {"Cinema",
     "22x16x9m auditorium, seat at 2/3 depth",
     22.0f, 16.0f, 9.0f, 0.25f,
     0.55f, 0.09f, 0.4f, 20.0f,  // noticeable reverb, long pre-delay
     2.0f,  // room_gain: +6dB compensates for ~3x speaker distances
     0.50f, // er_level: strong reflections, the "cinema envelopment"
     {
        { 25.0f,  15.0f, 10.0f}, // FL  - behind screen L (mid-screen ~15° above ear)
        {-25.0f,  15.0f, 10.0f}, // FR  - behind screen R
        {  0.0f,  15.0f,  9.0f}, // FC  - behind screen center
        {  0.0f,  -5.0f,  9.0f}, // LFE - subwoofer behind screen
        {150.0f,   5.0f,  6.0f}, // BL  - rear wall, slightly elevated
        {-150.0f,  5.0f,  6.0f}, // BR
        {100.0f,  10.0f,  7.5f}, // SL  - side wall arrays, above ear
        {-100.0f, 10.0f,  7.5f}, // SR
        { 40.0f,  50.0f,  8.5f}, // TFL - ceiling spatial modules
        {-40.0f,  50.0f,  8.5f}, // TFR
        {130.0f,  50.0f,  7.0f}, // TBL
        {-130.0f, 50.0f,  7.0f}, // TBR
    }},

    // --- Large Format Cinema ---
    // Large format auditorium ~26 x 30 x 18m, steep stadium seating
    // 22m wide screen (1.90:1), listener at 2/3 depth (~20m from screen)
    // Volume=14040m3, Absorption=0.28 -> RT60 ~0.7s (heavy acoustic treatment)
    {"Large Format",
     "26x30x18m large format, 22m screen, seat at 2/3 depth",
     26.0f, 30.0f, 18.0f, 0.28f,
     0.60f, 0.10f, 0.35f, 25.0f,  // controlled reverb, premium acoustic standards
     3.0f,  // room_gain: +9.5dB compensates for ~5x speaker distances
     0.55f, // er_level: bigger room, deeper pre-delay pocket
     {
        { 25.0f,  13.0f, 14.0f}, // FL  - behind screen L (22m screen ~13° above ear)
        {-25.0f,  13.0f, 14.0f}, // FR  - behind screen R
        {  0.0f,  13.0f, 13.0f}, // FC  - behind screen center
        {  0.0f,  -5.0f, 13.0f}, // LFE - sub array behind screen
        {150.0f,   5.0f,  9.0f}, // BL  - rear wall surrounds
        {-150.0f,  5.0f,  9.0f}, // BR
        {100.0f,  10.0f, 10.0f}, // SL  - side wall arrays
        {-100.0f, 10.0f, 10.0f}, // SR
        { 35.0f,  55.0f, 12.0f}, // TFL - overhead, steep due to 18m ceiling
        {-35.0f,  55.0f, 12.0f}, // TFR
        {130.0f,  50.0f, 10.0f}, // TBL
        {-130.0f, 50.0f, 10.0f}, // TBR
    }},

    // --- Giant Screen Theater ---
    // Giant screen theater ~36 x 42 x 26m, the largest premium format
    // 30m wide x 23m tall screen (1.43:1), listener at 2/3 depth (~28m)
    // Volume=39312m3, Absorption=0.25 -> RT60 ~0.9s
    {"Giant Screen",
     "36x42x26m giant screen, 30x23m screen, seat at 2/3 depth",
     36.0f, 42.0f, 26.0f, 0.25f,
     0.70f, 0.13f, 0.30f, 35.0f,  // more reverb from massive volume
     4.0f,  // room_gain: +12dB compensates for ~7x speaker distances
     0.60f, // er_level: massive volume, strong spatial envelopment
     {
        { 22.0f,  18.0f, 18.0f}, // FL  - behind 30m screen L (~18° above ear)
        {-22.0f,  18.0f, 18.0f}, // FR  - behind 30m screen R
        {  0.0f,  18.0f, 17.0f}, // FC  - behind screen center
        {  0.0f,  -5.0f, 17.0f}, // LFE - sub array behind screen
        {155.0f,   5.0f, 12.0f}, // BL  - rear wall surrounds
        {-155.0f,  5.0f, 12.0f}, // BR
        { 95.0f,  10.0f, 13.0f}, // SL  - side wall arrays, ~13m
        {-95.0f,  10.0f, 13.0f}, // SR
        { 30.0f,  60.0f, 16.0f}, // TFL - overhead, very steep (26m ceiling)
        {-30.0f,  60.0f, 16.0f}, // TFR
        {135.0f,  55.0f, 13.0f}, // TBL
        {-135.0f, 55.0f, 13.0f}, // TBR
    }},

    // --- Concert Hall ---
    // Large venue ~35 x 25 x 14m
    // Volume=12250m3, Absorption=0.15 -> RT60 ~1.9s
    {"Concert Hall",
     "35x25x14m concert venue, ~20m from stage",
     35.0f, 25.0f, 14.0f, 0.15f,
     0.82f, 0.16f, 0.25f, 35.0f,  // long reverb tail, warm, long pre-delay
     3.5f,  // room_gain: +11dB compensates for ~6x speaker distances
     0.65f, // er_level: lively hall, prominent early reflections
     {
        { 35.0f,   5.0f, 20.0f}, // FL  - stage PA left
        {-35.0f,   5.0f, 20.0f}, // FR  - stage PA right
        {  0.0f,   3.0f, 20.0f}, // FC  - center cluster
        {  0.0f,  -5.0f, 18.0f}, // LFE - sub stacks
        {155.0f,  10.0f, 10.0f}, // BL  - rear delay arrays
        {-155.0f, 10.0f, 10.0f}, // BR
        {110.0f,  15.0f,  8.0f}, // SL  - balcony fills
        {-110.0f, 15.0f,  8.0f}, // SR
        { 50.0f,  60.0f, 12.0f}, // TFL - ceiling rigging
        {-50.0f,  60.0f, 12.0f}, // TFR
        {140.0f,  55.0f, 10.0f}, // TBL
        {-140.0f, 55.0f, 10.0f}, // TBR
    }},
};

static const int numRoomPresets = sizeof(roomPresets) / sizeof(roomPresets[0]);

// Known HRTF database descriptions (matched by filename keywords)
static const char* guessDescription(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("kemar") != std::string::npos)
        return "MIT KEMAR mannequin, average-sized pinnae";
    if (lower.find("cipic") != std::string::npos)
        return "CIPIC (UC Davis) individual measurement";
    if (lower.find("listen") != std::string::npos || lower.find("ircam") != std::string::npos)
        return "LISTEN/IRCAM individual measurement";
    if (lower.find("ari") != std::string::npos)
        return "ARI (Austrian Academy of Sciences)";
    if (lower.find("sadie") != std::string::npos)
        return "SADIE II database (University of York)";
    if (lower.find("hutubs") != std::string::npos)
        return "HUTUBS database (TH Koeln)";
    if (lower.find("3d3a") != std::string::npos || lower.find("princeton") != std::string::npos)
        return "3D3A Lab (Princeton University)";
    if (lower.find("thu") != std::string::npos)
        return "Tsinghua University HRTF database";
    if (lower.find("sonicom") != std::string::npos)
        return "SONICOM project HRTF";
    if (lower.find("default") != std::string::npos)
        return "Default HRTF profile";
    if (lower.find("small") != std::string::npos)
        return "Small head/ears profile";
    if (lower.find("medium") != std::string::npos || lower.find("average") != std::string::npos)
        return "Average head/ears profile";
    if (lower.find("large") != std::string::npos)
        return "Large head/ears profile";
    return "";
}

// Convert filename to display name: "CIPIC_subject_003.sofa" -> "CIPIC Subject 003"
static std::string filenameToDisplayName(const std::string& filename) {
    // Remove .sofa extension
    std::string name = filename;
    auto dot = name.rfind('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);

    // Replace underscores and hyphens with spaces
    for (auto& c : name) {
        if (c == '_' || c == '-')
            c = ' ';
    }

    // Capitalize first letter of each word
    bool capitalize = true;
    for (auto& c : name) {
        if (capitalize && c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }
        capitalize = (c == ' ');
    }

    return name;
}

ControlPanel::ControlPanel(HrtfSharedState* state, int* selectedSpeaker,
                           MpvPlayer* player)
    : m_state(state), m_selectedSpeaker(selectedSpeaker), m_player(player) {
    strncpy(m_sofaPath, "assets/hrtf/default.sofa", sizeof(m_sofaPath) - 1);
}

ControlPanel::~ControlPanel() {
}

void ControlPanel::scanProfiles() {
    m_profiles.clear();
    m_selectedProfile = 0;

    const char* searchDir = "assets/hrtf";

    try {
        if (!fs::exists(searchDir))
            return;

        for (const auto& entry : fs::directory_iterator(searchDir)) {
            if (!entry.is_regular_file())
                continue;

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".sofa")
                continue;

            HrtfProfile profile;
            profile.path = entry.path().string();
            // Normalize to forward slashes for consistency
            std::replace(profile.path.begin(), profile.path.end(), '\\', '/');
            profile.name = filenameToDisplayName(entry.path().filename().string());
            profile.description = guessDescription(entry.path().filename().string());

            m_profiles.push_back(std::move(profile));
        }
    } catch (...) {
        // Filesystem errors - just leave empty
    }

    // Sort alphabetically by name
    std::sort(m_profiles.begin(), m_profiles.end(),
              [](const HrtfProfile& a, const HrtfProfile& b) {
                  return a.name < b.name;
              });

    // Find the currently active profile
    std::string currentPath = m_sofaPath;
    std::replace(currentPath.begin(), currentPath.end(), '\\', '/');
    for (int i = 0; i < (int)m_profiles.size(); i++) {
        if (m_profiles[i].path == currentPath) {
            m_selectedProfile = i;
            break;
        }
    }

    m_profilesScanned = true;
}

void ControlPanel::scanIrs() {
    m_irFiles.clear();
    m_selectedIr = 0;
    const char* dir = "assets/ir";
    try {
        if (!fs::exists(dir)) { m_irScanned = true; return; }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".wav") continue;
            std::string p = entry.path().string();
            std::replace(p.begin(), p.end(), '\\', '/');
            m_irFiles.push_back(p);
        }
    } catch (...) {}
    std::sort(m_irFiles.begin(), m_irFiles.end());
    m_irScanned = true;
}

void ControlPanel::loadProfile(int index) {
    if (index < 0 || index >= (int)m_profiles.size())
        return;

    const auto& profile = m_profiles[index];
    strncpy(m_sofaPath, profile.path.c_str(), sizeof(m_sofaPath) - 1);
    m_sofaPath[sizeof(m_sofaPath) - 1] = '\0';

    strncpy(m_state->sofa_path, m_sofaPath, sizeof(m_state->sofa_path) - 1);
    m_state->sofa_path[sizeof(m_state->sofa_path) - 1] = '\0';
    atomic_store(&m_state->sofa_path_changed, 1);
}

void ControlPanel::render() {
    ImGui::Begin("Control Panel");

    // Scan profiles on first render
    if (!m_profilesScanned)
        scanProfiles();

    // HRTF enable toggle
    bool enabled = atomic_load(&m_state->hrtf_enabled) != 0;
    if (ImGui::Checkbox("HRTF Enabled", &enabled)) {
        atomic_store(&m_state->hrtf_enabled, enabled ? 1 : 0);
    }

    ImGui::Separator();

    // --- HRTF Ear Profile selector ---
    ImGui::Text("Ear Profile (HRTF):");

    if (m_profiles.empty()) {
        ImGui::TextDisabled("No .sofa files found in assets/hrtf/");
    } else {
        auto profileGetter = [](void* data, int idx, const char** out) -> bool {
            auto* profiles = (std::vector<HrtfProfile>*)data;
            if (idx < 0 || idx >= (int)profiles->size()) return false;
            *out = (*profiles)[idx].name.c_str();
            return true;
        };

        if (ImGui::Combo("##profile", &m_selectedProfile, profileGetter,
                          &m_profiles, (int)m_profiles.size())) {
            loadProfile(m_selectedProfile);
        }

        // Show description
        if (m_selectedProfile >= 0 && m_selectedProfile < (int)m_profiles.size()) {
            const auto& prof = m_profiles[m_selectedProfile];
            if (!prof.description.empty())
                ImGui::TextDisabled("%s", prof.description.c_str());
            ImGui::TextDisabled("File: %s", prof.path.c_str());
        }
    }

    // Rescan button
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) {
        scanProfiles();
    }
    // Manual path input (collapsible for advanced users)
    if (ImGui::TreeNode("Custom SOFA path...")) {
        if (ImGui::InputText("##sofa", m_sofaPath, sizeof(m_sofaPath),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            strncpy(m_state->sofa_path, m_sofaPath, sizeof(m_state->sofa_path) - 1);
            atomic_store(&m_state->sofa_path_changed, 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            strncpy(m_state->sofa_path, m_sofaPath, sizeof(m_state->sofa_path) - 1);
            atomic_store(&m_state->sofa_path_changed, 1);
        }
        ImGui::TreePop();
    }

    ImGui::Separator();

    // Auto-sync the layout dropdown to whatever the audio filter is actually
    // processing.  When mpv opens a track the filter writes num_channels to
    // SharedState; we reflect that in the UI so the dropdown doesn't lie
    // about the current layout.  Only updates when num_channels changes, so
    // manual dropdown edits aren't clobbered every frame.
    {
        int streamCh = atomic_load(&m_state->num_channels);
        if (streamCh != m_lastSeenNumChannels) {
            m_lastSeenNumChannels = streamCh;
            int autoLayout = -1;
            switch (streamCh) {
                case 2:  autoLayout = 4; break;  // Stereo
                case 6:  autoLayout = 3; break;  // 5.1
                case 7:  autoLayout = 2; break;  // 6.1
                case 8:  autoLayout = 1; break;  // 7.1
                case 12: autoLayout = 0; break;  // 7.1.4
                // 10 (7.1.2), 16 (9.1.6), etc. — leave dropdown alone.
            }
            if (autoLayout >= 0)
                m_selectedLayout = autoLayout;
        }
    }

    // Speaker layout (channel count)
    ImGui::Text("Speaker Layout:");
    if (ImGui::Combo("##layout", &m_selectedLayout, layoutNames,
                      (int)(sizeof(layoutNames) / sizeof(layoutNames[0])))) {
        int numCh = 12;
        switch (m_selectedLayout) {
            case 0: numCh = 12; break; // 7.1.4
            case 1: numCh = 8;  break; // 7.1
            case 2: numCh = 7;  break; // 6.1
            case 3: numCh = 6;  break; // 5.1
            case 4: numCh = 2;  break; // Stereo
        }
        const RoomPreset& room = roomPresets[m_selectedRoom];
        if (numCh == 7) {
            // 6.1 = FL FR FC LFE BC SL SR.  Derive BC from the preset's back
            // wall geometry (reuse BL distance/elevation but force az=180°)
            // so the back-centre speaker sits where the rear wall is.
            m_state->speaker_pos[0] = room.positions[0];               // FL
            m_state->speaker_pos[1] = room.positions[1];               // FR
            m_state->speaker_pos[2] = room.positions[2];               // FC
            m_state->speaker_pos[3] = room.positions[3];               // LFE
            HrtfPosition bc = room.positions[4];                        // use BL as base
            bc.azimuth = 180.0f;
            m_state->speaker_pos[4] = bc;                               // BC
            m_state->speaker_pos[5] = room.positions[6];               // SL
            m_state->speaker_pos[6] = room.positions[7];               // SR
        } else {
            for (int i = 0; i < numCh && i < 12; i++)
                m_state->speaker_pos[i] = room.positions[i];
        }
        atomic_store(&m_state->num_channels, numCh);
        atomic_store(&m_state->num_bed_channels, numCh);
        atomic_store(&m_state->speaker_pos_changed, 1);
    }

    // Room/environment preset
    ImGui::Text("Room Preset:");
    auto roomGetter = [](void* data, int idx, const char** out) -> bool {
        (void)data;
        if (idx < 0 || idx >= numRoomPresets) return false;
        *out = roomPresets[idx].name;
        return true;
    };
    if (ImGui::Combo("##room", &m_selectedRoom, roomGetter, nullptr, numRoomPresets)) {
        const RoomPreset& room = roomPresets[m_selectedRoom];
        int numCh = atomic_load(&m_state->num_channels);
        for (int i = 0; i < numCh && i < 12; i++)
            m_state->speaker_pos[i] = room.positions[i];
        atomic_store(&m_state->speaker_pos_changed, 1);

        // Apply reverb params from room preset
        atomic_store(&m_state->reverb_decay, room.reverb_decay);
        atomic_store(&m_state->reverb_wet, room.reverb_wet);
        atomic_store(&m_state->reverb_damping, room.reverb_damping);
        atomic_store(&m_state->reverb_predelay, room.reverb_predelay);
        atomic_store(&m_state->reverb_changed, 1);

        // Apply room geometry for early reflections
        atomic_store(&m_state->room_width, room.width);
        atomic_store(&m_state->room_depth, room.depth);
        atomic_store(&m_state->room_height, room.height);
        atomic_store(&m_state->room_absorption, room.absorption);
        atomic_store(&m_state->room_gain, room.room_gain);
        atomic_store(&m_state->room_changed, 1);

        // Apply ambisonic ER send — bigger rooms → more reflection energy
        atomic_store(&m_state->er_level, room.er_level);

        // Screen baffling: auto-enable for cinema-class presets (Cinema=2,
        // Large Format=3, Giant Screen=4).  Perforated projection screens
        // add the subtle HF rolloff that cues "speakers behind a screen".
        atomic_store(&m_state->screen_baffling,
                     (m_selectedRoom >= 2 && m_selectedRoom <= 4) ? 1 : 0);
    }
    if (m_selectedRoom >= 0 && m_selectedRoom < numRoomPresets) {
        const auto& room = roomPresets[m_selectedRoom];
        ImGui::TextDisabled("%s", room.description);
        // Compute and show RT60 from Sabine equation
        float volume = room.width * room.depth * room.height;
        float surface = 2.0f * (room.width * room.depth +
                                 room.width * room.height +
                                 room.depth * room.height);
        float totalAbsorption = surface * room.absorption;
        float rt60 = (totalAbsorption > 0) ? 0.161f * volume / totalAbsorption : 0;
        ImGui::TextDisabled("Vol: %.0fm3  RT60: %.2fs", volume, rt60);
    }

    ImGui::Separator();

    // Room reverb controls
    ImGui::Text("Room Reverb:");
    bool reverbOn = atomic_load(&m_state->reverb_enabled) != 0;
    if (ImGui::Checkbox("Reverb Enabled", &reverbOn)) {
        atomic_store(&m_state->reverb_enabled, reverbOn ? 1 : 0);
        atomic_store(&m_state->reverb_changed, 1);
    }

    if (reverbOn) {
        bool reverbChanged = false;
        float decay = atomic_load(&m_state->reverb_decay);
        float wet = atomic_load(&m_state->reverb_wet);
        float damp = atomic_load(&m_state->reverb_damping);
        float predelay = atomic_load(&m_state->reverb_predelay);

        reverbChanged |= ImGui::SliderFloat("Decay", &decay, 0.0f, 1.0f, "%.2f");
        reverbChanged |= ImGui::SliderFloat("Wet Mix", &wet, 0.0f, 1.0f, "%.2f");
        reverbChanged |= ImGui::SliderFloat("Damping", &damp, 0.0f, 1.0f, "%.2f");
        reverbChanged |= ImGui::SliderFloat("Pre-delay", &predelay, 0.0f, 80.0f, "%.1f ms");

        if (reverbChanged) {
            atomic_store(&m_state->reverb_decay, decay);
            atomic_store(&m_state->reverb_wet, wet);
            atomic_store(&m_state->reverb_damping, damp);
            atomic_store(&m_state->reverb_predelay, predelay);
            atomic_store(&m_state->reverb_changed, 1);
        }
    }

    // Ambisonic early reflections — spatialised first-order reflections
    // (Steam Audio-style).  Sends a mono bus of the dry sources through six
    // image-source taps, each encoded into B-format (W,Y,Z,X) by its image
    // direction, then decoded to binaural via precomputed SH-HRIR filters.
    float erLevel = atomic_load(&m_state->er_level);
    if (ImGui::SliderFloat("ER Wet (Ambisonic)", &erLevel, 0.0f, 1.0f, "%.2f")) {
        atomic_store(&m_state->er_level, erLevel);
    }

    // Convolution-reverb IR dropdown (scans assets/ir/*.wav).
    // Users drop a stereo 48 kHz WAV IR into that folder and pick it here.
    if (!m_irScanned) scanIrs();
    {
        std::vector<const char*> labels;
        labels.push_back("None");
        for (const auto& f : m_irFiles) {
            // Show just the filename for brevity
            size_t slash = f.find_last_of('/');
            const char *name = (slash == std::string::npos)
                                 ? f.c_str()
                                 : f.c_str() + slash + 1;
            labels.push_back(name);
        }
        int idx = m_selectedIr;
        if (ImGui::Combo("Conv reverb IR", &idx,
                          labels.data(), (int)labels.size())) {
            m_selectedIr = idx;
            if (idx == 0) {
                m_state->ir_file_path[0] = '\0';
            } else {
                const std::string& p = m_irFiles[idx - 1];
                strncpy(m_state->ir_file_path, p.c_str(),
                        sizeof(m_state->ir_file_path) - 1);
                m_state->ir_file_path[sizeof(m_state->ir_file_path) - 1] = '\0';
            }
            atomic_store(&m_state->ir_changed, 1);
        }
        float irWet = atomic_load(&m_state->ir_wet);
        if (ImGui::SliderFloat("Conv reverb wet", &irWet, 0.0f, 1.0f, "%.2f")) {
            atomic_store(&m_state->ir_wet, irWet);
        }
    }

    // Crossfeed (signed).  Positive = classical narrowing (dilutes HRTF cues,
    // pulls sides back).  Negative = stereo widener (amplifies ILD, pushes
    // sides outward).  Useful when a generic HRTF doesn't lateralise enough.
    float xfeed = atomic_load(&m_state->crossfeed);
    if (ImGui::SliderFloat("Crossfeed (broadband)", &xfeed, -0.3f, 0.3f, "%.2f")) {
        atomic_store(&m_state->crossfeed, xfeed);
    }

    // Bauer crossfeed: LF-only contralateral bleed.  Fixes "frontals feel
    // collapsed to one side" without blurring side/rear sources — the HF
    // localisation cues stay intact.
    float bauer = atomic_load(&m_state->bauer_crossfeed);
    if (ImGui::SliderFloat("Bauer crossfeed (LF only)", &bauer, 0.0f, 0.5f, "%.2f")) {
        atomic_store(&m_state->bauer_crossfeed, bauer);
    }

    // Channel order: SMPTE (Atmos / DCI / cinema) vs WAVE (raw 7.1 files).
    // FFmpeg exposes Dolby content in WAVE order but the audio payload is
    // still in SMPTE order — the SL/BL and SR/BR pairs end up at the wrong
    // angle unless we swap them.  Toggling this swaps speaker positions 4↔6
    // and 5↔7 in the shared state and signals the filter to reload HRIRs.
    bool smpte = atomic_load(&m_state->channel_order_smpte) != 0;
    if (ImGui::Checkbox("SMPTE channel order (Atmos / cinema)", &smpte)) {
        atomic_store(&m_state->channel_order_smpte, smpte ? 1 : 0);
        int numCh = atomic_load(&m_state->num_channels);
        if (numCh == 7) {
            // 6.1 rotation: ch4→ch5→ch6→ch4 (SL, SR, BC) so the three tail
            // positions match where Dolby places the audio in SMPTE order.
            HrtfPosition t = m_state->speaker_pos[4];
            m_state->speaker_pos[4] = m_state->speaker_pos[5];
            m_state->speaker_pos[5] = m_state->speaker_pos[6];
            m_state->speaker_pos[6] = t;
            atomic_store(&m_state->speaker_pos_changed, 1);
        } else if (numCh >= 8) {
            HrtfPosition t;
            t = m_state->speaker_pos[4]; m_state->speaker_pos[4] = m_state->speaker_pos[6]; m_state->speaker_pos[6] = t;
            t = m_state->speaker_pos[5]; m_state->speaker_pos[5] = m_state->speaker_pos[7]; m_state->speaker_pos[7] = t;
            atomic_store(&m_state->speaker_pos_changed, 1);
        }
    }

    // Screen baffling — simulate perforated cinema screen on FL/FR/FC.
    bool baffle = atomic_load(&m_state->screen_baffling) != 0;
    if (ImGui::Checkbox("Screen baffling (cinema HF rolloff on FL/FR/FC)", &baffle)) {
        atomic_store(&m_state->screen_baffling, baffle ? 1 : 0);
    }

    // Frontal pinna boost — fights HRTF front/back confusion by injecting
    // the characteristic pinna resonance of a frontal source.
    bool pinna = atomic_load(&m_state->front_pinna_boost) != 0;
    if (ImGui::Checkbox("Frontal pinna boost (anti front-back confusion)", &pinna)) {
        atomic_store(&m_state->front_pinna_boost, pinna ? 1 : 0);
    }

    ImGui::Separator();

    // Master volume
    float vol = atomic_load(&m_state->master_volume);
    if (ImGui::SliderFloat("Master Volume", &vol, 0.0f, 1.5f, "%.2f")) {
        atomic_store(&m_state->master_volume, vol);
    }

    ImGui::Separator();

    // Speaker list with editable positions
    ImGui::Text("Speakers:");
    int numCh = atomic_load(&m_state->num_channels);
    int bedCount = atomic_load(&m_state->num_bed_channels);
    if (bedCount < 0 || bedCount > numCh)
        bedCount = (numCh > 8) ? 8 : numCh;

    for (int i = 0; i < numCh && i < HRTF_MAX_CHANNELS; i++) {
        ImGui::PushID(i);

        // Color indicator
        ImVec4 color;
        switch (i) {
            case 0: case 1: case 2: color = ImVec4(0.3f, 0.5f, 1.0f, 1.0f); break;
            case 3:                  color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); break;
            case 4: case 5:         color = ImVec4(1.0f, 0.6f, 0.2f, 1.0f); break;
            case 6: case 7:         color = ImVec4(0.3f, 0.8f, 0.3f, 1.0f); break;
            default:                 color = ImVec4(0.7f, 0.3f, 0.9f, 1.0f); break;
        }
        bool isSelected = m_selectedSpeaker && (*m_selectedSpeaker == i);

        ImGui::ColorButton("##color", color, ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
        ImGui::SameLine();

        // Auto-open selected speaker's tree node
        if (isSelected)
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);

        bool isObjectChannel = i >= bedCount;
        // Pick name array based on active layout: 6.1 has a different tail.
        bool is61 = (numCh == 7);
        const char* spkName;
        if (isObjectChannel)                       spkName = "Object";
        else if (is61 && i < 7)                    spkName = speakerName61[i];
        else if (i < 12)                           spkName = speakerNames[i];
        else                                       spkName = "Unknown";
        char spkLabel[64];
        if (isObjectChannel)
            snprintf(spkLabel, sizeof(spkLabel), "Object %d (ch %d)", i - bedCount, i);
        else
            snprintf(spkLabel, sizeof(spkLabel), "%s", spkName);
        if (ImGui::TreeNode(spkLabel)) {
            // Click the header to select this speaker
            if (m_selectedSpeaker && ImGui::IsItemClicked())
                *m_selectedSpeaker = i;
            bool changed = false;

            float az = m_state->speaker_pos[i].azimuth;
            float el = m_state->speaker_pos[i].elevation;
            float dist = m_state->speaker_pos[i].distance;

            changed |= ImGui::SliderFloat("Azimuth", &az, -180.0f, 180.0f, "%.1f deg");
            changed |= ImGui::SliderFloat("Elevation", &el, -90.0f, 90.0f, "%.1f deg");
            changed |= ImGui::SliderFloat("Distance", &dist, 0.5f, 15.0f, "%.2f m");

            if (changed) {
                m_state->speaker_pos[i].azimuth = az;
                m_state->speaker_pos[i].elevation = el;
                m_state->speaker_pos[i].distance = dist;
                atomic_store(&m_state->speaker_pos_changed, 1);
            }

            // Show RMS level + Test button on same line
            {
                float rms = atomic_load(&m_state->channel_rms[i]);
                float buttonW = 0;
                if (m_player) {
                    buttonW = ImGui::CalcTextSize("Playing...").x +
                              ImGui::GetStyle().FramePadding.x * 2 +
                              ImGui::GetStyle().ItemSpacing.x;
                }
                float barW = ImGui::GetContentRegionAvail().x - buttonW;
                ImGui::ProgressBar(rms, ImVec2(barW, 0), "");

                if (m_player) {
                    ImGui::SameLine();
                    bool toneActive = atomic_load(&m_state->test_tone_active) != 0 &&
                                      atomic_load(&m_state->test_tone_channel) == i;
                    if (toneActive) {
                        ImGui::TextDisabled("Playing...");
                    } else {
                        if (ImGui::SmallButton("Test")) {
                            m_player->playTestTone(i);
                        }
                    }
                }
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    ImGui::Separator();

    // --- Debug section ---
    if (ImGui::CollapsingHeader("Debug")) {
        int numChNow = atomic_load(&m_state->num_channels);
        int bedCountNow = atomic_load(&m_state->num_bed_channels);
        if (bedCountNow < 0 || bedCountNow > numChNow)
            bedCountNow = (numChNow > 8) ? 8 : numChNow;

        bool muteBed = atomic_load(&m_state->mute_bed) != 0;
        bool muteObj = atomic_load(&m_state->mute_objects) != 0;

        char muteBedLabel[64];
        char muteObjLabel[64];
        snprintf(muteBedLabel, sizeof(muteBedLabel), "Mute Bed (ch 0-%d)",
                 bedCountNow > 0 ? bedCountNow - 1 : 0);
        snprintf(muteObjLabel, sizeof(muteObjLabel), "Mute Objects (ch %d+)",
                 bedCountNow);

        if (ImGui::Checkbox(muteBedLabel, &muteBed))
            atomic_store(&m_state->mute_bed, muteBed ? 1 : 0);

        if (ImGui::Checkbox(muteObjLabel, &muteObj))
            atomic_store(&m_state->mute_objects, muteObj ? 1 : 0);

        if (numChNow > bedCountNow) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                "Spatial: %d bed + %d object channels",
                bedCountNow, numChNow - bedCountNow);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                "Standard: %d channels (no objects)", numChNow);
        }

        // Per-channel levels for all channels including objects
        if (ImGui::TreeNode("Channel Levels")) {
            for (int i = 0; i < numChNow && i < HRTF_MAX_CHANNELS; i++) {
                float rms = atomic_load(&m_state->channel_rms[i]);
                float peak = atomic_load(&m_state->channel_peak[i]);
                bool isObj = i >= bedCountNow;
                bool is61 = (numChNow == 7);
                const char* label;
                if (isObj)                 label = "Object";
                else if (is61 && i < 7)    label = speakerName61[i];
                else if (i < 12)           label = speakerNames[i];
                else                       label = "Unknown";
                char buf[64];
                if (isObj)
                    snprintf(buf, sizeof(buf), "Obj %d", i - bedCountNow);
                else
                    snprintf(buf, sizeof(buf), "%s", label);
                ImGui::Text("%-20s RMS: %.4f  Peak: %.4f", buf, rms, peak);
            }
            ImGui::TreePop();
        }
    }

    ImGui::Separator();

    // Reset button
    if (ImGui::Button("Reset to Default")) {
        m_selectedLayout = 0;
        m_selectedRoom = 1; // Home Theater
        const RoomPreset& room = roomPresets[m_selectedRoom];
        atomic_store(&m_state->num_channels, 12);
        atomic_store(&m_state->num_bed_channels, 12);
        for (int i = 0; i < 12; i++)
            m_state->speaker_pos[i] = room.positions[i];
        atomic_store(&m_state->speaker_pos_changed, 1);

        // Reset room geometry for early reflections
        atomic_store(&m_state->room_width, room.width);
        atomic_store(&m_state->room_depth, room.depth);
        atomic_store(&m_state->room_height, room.height);
        atomic_store(&m_state->room_absorption, room.absorption);
        atomic_store(&m_state->room_gain, room.room_gain);
        atomic_store(&m_state->room_changed, 1);
    }

    // Status
    ImGui::Separator();
    bool active = atomic_load(&m_state->active) != 0;
    int sr = atomic_load(&m_state->sample_rate);
    ImGui::Text("Status: %s", active ? "Processing" : "Idle");
    if (active) {
        ImGui::Text("Sample Rate: %d Hz", sr);
        ImGui::Text("Channels: %d", numCh);
    }

    // Spatial object status (sidecar or real-time ObjMeta from decoder)
    {
        int numObj = atomic_load(&m_state->num_objects);
        if (m_sidecarLoaded) {
            ImGui::Separator();
            ImGui::Text("Spatial Objects: %d (sidecar: %zu frames)",
                         numObj, m_sidecarFrames.size());
        } else if (numObj > 0 && active) {
            ImGui::Separator();
            ImGui::Text("Spatial Objects: %d (ObjMeta)", numObj);
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Spatial object sidecar (.aobj) loader
// ---------------------------------------------------------------------------

void ControlPanel::loadSidecar(const std::string& mediaPath) {
    unloadSidecar();

    // Replace media extension with .aobj
    std::string sidecarPath = mediaPath;
    auto dot = sidecarPath.rfind('.');
    if (dot != std::string::npos)
        sidecarPath = sidecarPath.substr(0, dot);
    sidecarPath += ".aobj";

    FILE* f = fopen(sidecarPath.c_str(), "rb");
    if (!f) return;

    // Read header: magic "AOBJ" (4), version (1), num_frames (4), fps (4)
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "AOBJ", 4) != 0) {
        fclose(f);
        return;
    }

    uint8_t version;
    uint32_t num_frames;
    float fps;
    if (fread(&version, 1, 1, f) != 1 || version > 1) { fclose(f); return; }
    if (fread(&num_frames, 4, 1, f) != 1) { fclose(f); return; }
    if (fread(&fps, 4, 1, f) != 1) { fclose(f); return; }

    m_sidecarFrames.resize(num_frames);
    for (uint32_t i = 0; i < num_frames; i++) {
        AobjFrame& frame = m_sidecarFrames[i];
        if (fread(&frame.pts_ms, 4, 1, f) != 1) break;
        if (fread(&frame.num_objects, 1, 1, f) != 1) break;
        if (frame.num_objects > 128) frame.num_objects = 128;
        for (int j = 0; j < frame.num_objects; j++) {
            if (fread(&frame.objects[j].x, 4, 1, f) != 1) break;
            if (fread(&frame.objects[j].y, 4, 1, f) != 1) break;
            if (fread(&frame.objects[j].z, 4, 1, f) != 1) break;
        }
    }

    fclose(f);
    m_sidecarLoaded = true;
    m_sidecarPath = sidecarPath;
    m_lastSidecarIdx = -1;
    fprintf(stderr, "[ControlPanel] Loaded sidecar: %s (%u frames, %.1f fps)\n",
            sidecarPath.c_str(), num_frames, fps);
}

void ControlPanel::unloadSidecar() {
    m_sidecarFrames.clear();
    m_sidecarLoaded = false;
    m_lastSidecarIdx = -1;
    m_sidecarPath.clear();
}

void ControlPanel::updateObjectPositions() {
    if (!m_sidecarLoaded || !m_state || m_sidecarFrames.empty())
        return;

    // Read current PTS from shared state (written by audio filter)
    double pts = atomic_load(&m_state->current_pts);
    if (pts < 0) return;

    uint32_t pts_ms = (uint32_t)(pts * 1000.0);

    // Binary search for the frame closest to (but not after) current PTS
    int lo = 0, hi = (int)m_sidecarFrames.size() - 1;
    int best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m_sidecarFrames[mid].pts_ms <= pts_ms) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    // Skip if same frame as last time
    if (best == m_lastSidecarIdx)
        return;
    m_lastSidecarIdx = best;

    const AobjFrame& frame = m_sidecarFrames[best];
    int numObj = frame.num_objects;
    if (numObj > 128) numObj = 128;

    atomic_store(&m_state->num_objects, (int32_t)numObj);
    for (int i = 0; i < HRTF_MAX_OBJECTS; i++) {
        if (i < numObj) {
            atomic_store(&m_state->object_x[i], frame.objects[i].x);
            atomic_store(&m_state->object_y[i], frame.objects[i].y);
            atomic_store(&m_state->object_z[i], frame.objects[i].z);
            atomic_store(&m_state->object_active[i], 1);
        } else {
            atomic_store(&m_state->object_active[i], 0);
        }
        atomic_store(&m_state->object_gain[i], 0.0f);
    }
    atomic_store(&m_state->objects_changed, 1);
}
