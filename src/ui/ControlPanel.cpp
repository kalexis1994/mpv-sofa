#include "ControlPanel.h"
#include "core/SharedState.h"
#include "core/Settings.h"
#include "audio/MpvPlayer.h"
#include <imgui.h>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <mysofa.h>

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
    {"Dolby Cinema",
     "26x30x18m premium large format, 22m screen, seat at 2/3 depth",
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
    {"IMAX",
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

    // --- Living Room ---
    // Typical untreated domestic room ~5 x 4.2 x 2.5m
    // Volume=52.5m3, hard surfaces + furnishing -> RT60 ~0.55s
    {"Living Room",
     "5x4.2x2.5m untreated domestic room, TV setup",
     5.0f, 4.2f, 2.5f, 0.18f,
     0.18f, 0.035f, 0.65f, 7.0f,   // short but audible domestic reverb
     1.0f,   // room_gain: nearfield TV distances
     0.20f,  // er_level: bare walls reflect, furniture breaks it up
     {
        { 30.0f,   0.0f, 2.2f},  // FL  - flanking the TV
        {-30.0f,   0.0f, 2.2f},  // FR
        {  0.0f,   0.0f, 2.2f},  // FC  - soundbar/TV position
        {  0.0f, -10.0f, 2.0f},  // LFE - floor, corner
        {140.0f,   0.0f, 1.8f},  // BL  - behind sofa
        {-140.0f,  0.0f, 1.8f},  // BR
        { 90.0f,   5.0f, 1.6f},  // SL  - side walls, close
        {-90.0f,   5.0f, 1.6f},  // SR
        { 45.0f,  45.0f, 2.0f},  // TFL - low ceiling
        {-45.0f,  45.0f, 2.0f},  // TFR
        {135.0f,  45.0f, 2.0f},  // TBL
        {-135.0f, 45.0f, 2.0f},  // TBR
    }},

    // --- Screening Room ---
    // Post-production review room ~9 x 6.5 x 3.5m, treated
    // Volume=205m3, Absorption~0.49 -> RT60 ~0.3s
    {"Screening Room",
     "9x6.5x3.5m post-production review room, seat at 2/3 depth",
     9.0f, 6.5f, 3.5f, 0.49f,
     0.32f, 0.050f, 0.55f, 10.0f,  // tight, controlled decay
     1.3f,   // room_gain: mid-distance speakers
     0.35f,  // er_level: treated but present
     {
        { 28.0f,   8.0f, 4.5f},  // FL  - behind small screen L
        {-28.0f,   8.0f, 4.5f},  // FR
        {  0.0f,   8.0f, 4.2f},  // FC  - behind screen center
        {  0.0f,  -5.0f, 4.0f},  // LFE - front floor
        {145.0f,   5.0f, 3.0f},  // BL  - rear wall
        {-145.0f,  5.0f, 3.0f},  // BR
        { 95.0f,   8.0f, 3.5f},  // SL  - side wall pair
        {-95.0f,   8.0f, 3.5f},  // SR
        { 42.0f,  48.0f, 3.8f},  // TFL - ceiling
        {-42.0f,  48.0f, 3.8f},  // TFR
        {132.0f,  48.0f, 3.4f},  // TBL
        {-132.0f, 48.0f, 3.4f},  // TBR
    }},

    // --- None (Dry) ---
    // Room processing bypassed: pure HRTF at ITU reference angles.
    {"None (Dry)",
     "No room simulation - pure HRTF, ITU reference angles at 2m",
     0.0f, 0.0f, 0.0f, 1.0f,
     0.0f, 0.0f, 1.0f, 0.0f,       // reverb fully off
     1.0f,   // room_gain: unity
     0.0f,   // er_level: no reflections
     {
        { 30.0f,   0.0f, 2.0f},  // FL
        {-30.0f,   0.0f, 2.0f},  // FR
        {  0.0f,   0.0f, 2.0f},  // FC
        {  0.0f, -10.0f, 2.0f},  // LFE
        {135.0f,   0.0f, 2.0f},  // BL
        {-135.0f,  0.0f, 2.0f},  // BR
        { 90.0f,   0.0f, 2.0f},  // SL
        {-90.0f,   0.0f, 2.0f},  // SR
        { 45.0f,  45.0f, 2.0f},  // TFL
        {-45.0f,  45.0f, 2.0f},  // TFR
        {135.0f,  45.0f, 2.0f},  // TBL
        {-135.0f, 45.0f, 2.0f},  // TBR
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

// Real AES69 metadata straight from the .sofa file: subject/dummy head,
// database, measurement grid, license — beats guessing from the filename.
static void loadSofaMetadata(HrtfProfile& prof) {
    prof.metaLoaded = true;
    int err = 0;
    MYSOFA_HRTF* h = mysofa_load(prof.path.c_str(), &err);
    if (!h) return;

    auto attr = [&](const char* key) -> std::string {
        for (MYSOFA_ATTRIBUTE* a = h->attributes; a; a = a->next)
            if (a->name && a->value && !strcmp(a->name, key))
                return a->value;
        return "";
    };
    prof.subject      = attr("ListenerShortName");
    prof.database     = attr("DatabaseName");
    prof.organization = attr("Organization");
    prof.license      = attr("License");

    char buf[64];
    if (h->M) {
        snprintf(buf, sizeof(buf), "%u dirs", h->M);
        prof.grid = buf;
    }
    if (h->N && h->DataSamplingRate.values && h->DataSamplingRate.elements) {
        snprintf(buf, sizeof(buf), "%u taps @ %.0f kHz", h->N,
                 h->DataSamplingRate.values[0] / 1000.0);
        prof.res = buf;
    }
    mysofa_free(h);
}

/* ---- Profile cards ----------------------------------------------------- */

static void cardChip(ImDrawList* dl, float& cx, float cy, float xLimit,
                     const char* label, ImU32 bg, ImU32 fg) {
    ImVec2 ts = ImGui::CalcTextSize(label);
    float w = ts.x + 12.0f, h = ts.y + 4.0f;
    if (cx + w > xLimit) return;               /* drop chips that don't fit */
    dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + w, cy + h), bg, h * 0.5f);
    dl->AddText(ImVec2(cx + 6.0f, cy + 2.0f), fg, label);
    cx += w + 6.0f;
}

// One selectable profile card: title (measured subject when known) + a row
// of metadata chips. `metaBudget` throttles lazy SOFA parsing to a couple
// of files per frame so an 80-subject folder never hitches the UI.
static bool profileCard(HrtfProfile& prof, bool selected, float w, float h,
                        int& metaBudget) {
    if (!prof.metaLoaded && metaBudget > 0) {
        metaBudget--;
        loadSofaMetadata(prof);
    }

    ImGui::PushID(prof.path.c_str());
    ImVec2 origin = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bg     = ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered
                                              : ImGuiCol_FrameBg);
    ImU32 border = ImGui::GetColorU32(selected ? ImGuiCol_CheckMark
                                               : ImGuiCol_Border);
    ImVec2 pmax(origin.x + w, origin.y + h);
    dl->AddRectFilled(origin, pmax, bg, 8.0f);
    dl->AddRect(origin, pmax, border, 8.0f, 0, selected ? 2.5f : 1.0f);

    dl->PushClipRect(origin, pmax, true);
    const char* title = !prof.subject.empty() ? prof.subject.c_str()
                                              : prof.name.c_str();
    dl->AddText(ImVec2(origin.x + 10.0f, origin.y + 8.0f),
                ImGui::GetColorU32(ImGuiCol_Text), title);

    float cx = origin.x + 10.0f;
    float cy = pmax.y - 26.0f;
    ImU32 chipBg = ImGui::GetColorU32(ImGuiCol_Button, 0.55f);
    ImU32 chipFg = ImGui::GetColorU32(ImGuiCol_Text, 0.85f);
    float xLimit = pmax.x - 8.0f;
    if (!prof.metaLoaded) {
        cardChip(dl, cx, cy, xLimit, "reading metadata\xe2\x80\xa6", chipBg, chipFg);
    } else {
        bool any = false;
        auto chip = [&](const std::string& s) {
            if (s.empty()) return;
            cardChip(dl, cx, cy, xLimit, s.c_str(), chipBg, chipFg);
            any = true;
        };
        chip(prof.database);
        chip(prof.grid);
        chip(prof.res);
        chip(prof.license);
        if (!any)
            cardChip(dl, cx, cy, xLimit,
                     prof.description.empty() ? "no metadata"
                                              : prof.description.c_str(),
                     chipBg, chipFg);
    }
    dl->PopClipRect();

    if (hovered) {
        std::string tip = prof.name + "\n" + prof.path;
        if (!prof.organization.empty()) tip += "\n" + prof.organization;
        ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::PopID();
    return clicked;
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
            // Placeholder from the filename; the real AES69 metadata loads
            // lazily on selection (parsing ~80 SOFA files up-front would
            // block the UI for seconds).
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

    // Find the currently active profile.  m_state->sofa_path is the source
    // of truth (Settings::load() restores the persisted path into it before
    // the first render); the local m_sofaPath buffer is only used by the
    // "Custom SOFA path" input and is mirrored from the matched profile.
    std::string currentPath = (m_state && m_state->sofa_path[0] != '\0')
                                 ? std::string(m_state->sofa_path)
                                 : std::string(m_sofaPath);
    std::replace(currentPath.begin(), currentPath.end(), '\\', '/');
    for (int i = 0; i < (int)m_profiles.size(); i++) {
        if (m_profiles[i].path == currentPath) {
            m_selectedProfile = i;
            strncpy(m_sofaPath, m_profiles[i].path.c_str(),
                    sizeof(m_sofaPath) - 1);
            m_sofaPath[sizeof(m_sofaPath) - 1] = '\0';
            break;
        }
    }

    m_profilesScanned = true;
}

void ControlPanel::scanHpEqs() {
    m_hpEqFiles.clear();
    m_selectedHpEq = 0;  // 0 = "None"
    const char* dir = "assets/headphone_eq";
    try {
        if (!fs::exists(dir)) { m_hpEqScanned = true; return; }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".txt") continue;
            std::string p = entry.path().string();
            std::replace(p.begin(), p.end(), '\\', '/');
            m_hpEqFiles.push_back(p);
        }
    } catch (...) {}
    std::sort(m_hpEqFiles.begin(), m_hpEqFiles.end());
    // Match the path the audio filter is currently using (restored from
    // mpv-sofa.ini at startup) so the dropdown reflects reality.
    if (m_state && m_state->hp_eq_path[0] != '\0') {
        std::string current = m_state->hp_eq_path;
        std::replace(current.begin(), current.end(), '\\', '/');
        for (size_t i = 0; i < m_hpEqFiles.size(); i++) {
            if (m_hpEqFiles[i] == current) {
                m_selectedHpEq = (int)i + 1;  // +1 for "None" at index 0
                break;
            }
        }
    }
    m_hpEqScanned = true;
}

void ControlPanel::scanIrs() {
    m_irFiles.clear();
    m_selectedIr = 0;  // 0 = "None"
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
    // Match the path restored from mpv-sofa.ini.
    if (m_state && m_state->ir_file_path[0] != '\0') {
        std::string current = m_state->ir_file_path;
        std::replace(current.begin(), current.end(), '\\', '/');
        for (size_t i = 0; i < m_irFiles.size(); i++) {
            if (m_irFiles[i] == current) {
                m_selectedIr = (int)i + 1;  // +1 for "None"
                break;
            }
        }
    }
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

void ControlPanel::renderSpatialContent() {
    // Scan profiles on first render
    if (!m_profilesScanned)
        scanProfiles();

    // HRTF master toggle + master volume share the top "essentials" row
    // — the things you reach for most often live above any header.
    bool enabled = atomic_load(&m_state->hrtf_enabled) != 0;
    if (ImGui::Checkbox("HRTF Enabled", &enabled)) {
        atomic_store(&m_state->hrtf_enabled, enabled ? 1 : 0);
    }
    {
        float vol = atomic_load(&m_state->master_volume);
        if (ImGui::SliderFloat("Master Volume", &vol, 0.0f, 1.5f, "%.2f"))
            atomic_store(&m_state->master_volume, vol);
    }

    ImGui::SeparatorText("HRTF profile");

    if (m_profiles.empty()) {
        ImGui::TextDisabled("No .sofa files found in assets/hrtf/");
    } else {
        // Card grid: one card per profile, its AES69 metadata as chips.
        int metaBudget = 2;      /* lazy SOFA parses per frame */
        const float cardH = 64.0f;
        const float gapX  = 8.0f;
        const float rowH  = cardH + ImGui::GetStyle().ItemSpacing.y;

        ImGui::BeginChild("##profile_cards", ImVec2(0, 330.0f), false);
        float availW = ImGui::GetContentRegionAvail().x;
        int cols = availW > 540.0f ? 2 : 1;
        float cardW = (availW - gapX * (cols - 1)) / cols;
        int rows = ((int)m_profiles.size() + cols - 1) / cols;

        ImGuiListClipper clipper;
        clipper.Begin(rows, rowH);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                for (int c = 0; c < cols; c++) {
                    int idx = row * cols + c;
                    if (idx >= (int)m_profiles.size()) break;
                    if (c > 0) ImGui::SameLine(0.0f, gapX);
                    if (profileCard(m_profiles[idx],
                                    idx == m_selectedProfile,
                                    cardW, cardH, metaBudget)) {
                        m_selectedProfile = idx;
                        loadProfile(idx);
                    }
                }
            }
        }
        ImGui::EndChild();

        if (m_selectedProfile >= 0 && m_selectedProfile < (int)m_profiles.size())
            ImGui::TextDisabled("File: %s",
                                m_profiles[m_selectedProfile].path.c_str());
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

    ImGui::SeparatorText("Speaker layout");

    // Speaker layout — read-only display of what the audio filter is
    // actually processing.  The track determines the channel count, not
    // the user; offering an override here would just silently drop the
    // surplus channels (and lose surround / height content) since the
    // filter has no real downmix path of its own.  Use the Room Preset
    // below to reposition speakers in space.
    {
        int streamCh = atomic_load(&m_state->num_channels);
        const char* name = "—";
        switch (streamCh) {
            case 1:  name = "Mono";   break;
            case 2:  name = "Stereo"; break;
            case 3:  name = "2.1";    break;
            case 6:  name = "5.1";    break;
            case 7:  name = "6.1";    break;
            case 8:  name = "7.1";    break;
            case 10: name = "7.1.2";  break;
            case 12: name = "7.1.4";  break;
            case 14: name = "7.1.6";  break;
            case 16: name = "9.1.6";  break;
            default: break;
        }
        ImGui::Text("Layout:");
        ImGui::SameLine();
        if (streamCh > 0)
            ImGui::TextDisabled("%s  \xc2\xb7  %d ch", name, streamCh);
        else
            ImGui::TextDisabled("(no audio loaded)");
    }

    ImGui::SeparatorText("Room");

    // Room/environment preset.  Settings is the source of truth for the
    // index so it round-trips through mpv-sofa.ini; the geometry / reverb
    // values themselves are persisted as their own atomic fields.
    int roomIdx = Settings::roomPreset();
    ImGui::Text("Room Preset:");
    auto roomGetter = [](void* data, int idx, const char** out) -> bool {
        (void)data;
        if (idx < 0 || idx >= numRoomPresets) return false;
        *out = roomPresets[idx].name;
        return true;
    };
    if (ImGui::Combo("##room", &roomIdx, roomGetter, nullptr, numRoomPresets)) {
        Settings::setRoomPreset(roomIdx);
        const RoomPreset& room = roomPresets[roomIdx];
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
        // Dolby Cinema=3, IMAX=4).  Perforated projection screens add the
        // subtle HF rolloff that cues "speakers behind a screen".
        atomic_store(&m_state->screen_baffling,
                     (roomIdx >= 2 && roomIdx <= 4) ? 1 : 0);
    }
    if (roomIdx >= 0 && roomIdx < numRoomPresets) {
        const auto& room = roomPresets[roomIdx];
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

    ImGui::SeparatorText("Reverb");

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

    ImGui::Spacing();

    // Advanced HRTF / channel-order toggles — collapsed by default
    // because most users won't ever touch them, but they live alongside
    // the rest of the spatial pipeline so a power user doesn't have to
    // hunt for them.  Headphone EQ + crossfeed are intentionally NOT
    // here: they're post-processing on the binaural mix and have moved
    // to the dedicated EQ tab.
    if (ImGui::CollapsingHeader("Advanced")) {
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

        bool baffle = atomic_load(&m_state->screen_baffling) != 0;
        if (ImGui::Checkbox("Screen baffling (cinema HF rolloff on FL/FR/FC)", &baffle))
            atomic_store(&m_state->screen_baffling, baffle ? 1 : 0);

        bool pinna = atomic_load(&m_state->front_pinna_boost) != 0;
        if (ImGui::Checkbox("Frontal pinna boost (anti front-back confusion)", &pinna))
            atomic_store(&m_state->front_pinna_boost, pinna ? 1 : 0);

        bool nfc = atomic_load(&m_state->near_field_comp) != 0;
        if (ImGui::Checkbox("Near-field compensation (close-source body)", &nfc))
            atomic_store(&m_state->near_field_comp, nfc ? 1 : 0);

        bool mph = atomic_load(&m_state->direct_min_phase) != 0;
        if (ImGui::Checkbox("Direct HRIRs minimum-phase", &mph)) {
            atomic_store(&m_state->direct_min_phase, mph ? 1 : 0);
            atomic_store(&m_state->speaker_pos_changed, 1);
        }
    }

    ImGui::Spacing();

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

    ImGui::Spacing();

    // Reset button — restores Home Theater positions but does NOT touch
    // num_channels (the loaded track owns that).
    if (ImGui::Button("Reset to Home Theater defaults")) {
        Settings::setRoomPreset(1); // Home Theater
        const RoomPreset& room = roomPresets[1];
        int numCh = atomic_load(&m_state->num_channels);
        if (numCh <= 0 || numCh > 12) numCh = 12;
        for (int i = 0; i < numCh; i++)
            m_state->speaker_pos[i] = room.positions[i];
        atomic_store(&m_state->speaker_pos_changed, 1);

        atomic_store(&m_state->room_width, room.width);
        atomic_store(&m_state->room_depth, room.depth);
        atomic_store(&m_state->room_height, room.height);
        atomic_store(&m_state->room_absorption, room.absorption);
        atomic_store(&m_state->room_gain, room.room_gain);
        atomic_store(&m_state->room_changed, 1);
    }

    // Status footer.
    ImGui::SeparatorText("Status");
    bool active = atomic_load(&m_state->active) != 0;
    int sr = atomic_load(&m_state->sample_rate);
    ImGui::TextDisabled("Engine: %s", active ? "processing" : "idle");
    if (active) {
        ImGui::TextDisabled("Sample rate: %d Hz", sr);
        ImGui::TextDisabled("Channels: %d", atomic_load(&m_state->num_channels));
    }
    {
        int numObj = atomic_load(&m_state->num_objects);
        if (m_sidecarLoaded) {
            ImGui::TextDisabled("Spatial objects: %d (sidecar: %zu frames)",
                                 numObj, m_sidecarFrames.size());
        } else if (numObj > 0 && active) {
            ImGui::TextDisabled("Spatial objects: %d (ObjMeta)", numObj);
        }
    }
}

// ---------------------------------------------------------------------------
// EQ tab body — headphone calibration + lateral-mix knobs.  These touch
// the post-binaural signal only; nothing in this section reloads HRIRs.
// ---------------------------------------------------------------------------
void ControlPanel::renderEqContent() {
    ImGui::SeparatorText("Headphone EQ");
    ImGui::TextWrapped(
        "Neutralises the headphone's own coloration so the HRTF reaches "
        "the ear with the intended response.  Drop AutoEQ "
        "ParametricEq.txt files into assets/headphone_eq/ to populate.");
    ImGui::Spacing();

    if (!m_hpEqScanned) scanHpEqs();
    {
        std::vector<const char*> labels;
        labels.push_back("None");
        for (const auto& f : m_hpEqFiles) {
            size_t slash = f.find_last_of('/');
            const char* name = (slash == std::string::npos)
                                 ? f.c_str()
                                 : f.c_str() + slash + 1;
            labels.push_back(name);
        }
        int idx = m_selectedHpEq;
        if (ImGui::Combo("Profile", &idx,
                          labels.data(), (int)labels.size())) {
            m_selectedHpEq = idx;
            if (idx == 0) {
                m_state->hp_eq_path[0] = '\0';
            } else {
                const std::string& p = m_hpEqFiles[idx - 1];
                strncpy(m_state->hp_eq_path, p.c_str(),
                        sizeof(m_state->hp_eq_path) - 1);
                m_state->hp_eq_path[sizeof(m_state->hp_eq_path) - 1] = '\0';
            }
            atomic_store(&m_state->hp_eq_changed, 1);
        }
        bool hp_on = atomic_load(&m_state->hp_eq_enabled) != 0;
        if (ImGui::Checkbox("Enabled", &hp_on))
            atomic_store(&m_state->hp_eq_enabled, hp_on ? 1 : 0);
    }

    ImGui::SeparatorText("Crossfeed");
    ImGui::TextWrapped(
        "Broadband (signed): positive narrows the stereo image, negative "
        "widens it.  Bauer is LF-only contralateral bleed — fixes "
        "\"frontals collapsed to one side\" without softening the "
        "high-frequency localisation cues.");
    ImGui::Spacing();

    float xfeed = atomic_load(&m_state->crossfeed);
    if (ImGui::SliderFloat("Broadband", &xfeed, -0.3f, 0.3f, "%.2f"))
        atomic_store(&m_state->crossfeed, xfeed);

    float bauer = atomic_load(&m_state->bauer_crossfeed);
    if (ImGui::SliderFloat("Bauer (LF only)", &bauer, 0.0f, 0.5f, "%.2f"))
        atomic_store(&m_state->bauer_crossfeed, bauer);
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

void ControlPanel::renderSpeakerList() {
    if (!m_state) return;

    int numCh = atomic_load(&m_state->num_channels);
    int bedCount = atomic_load(&m_state->num_bed_channels);
    if (bedCount < 0 || bedCount > numCh)
        bedCount = (numCh > 8) ? 8 : numCh;

    if (numCh <= 0) {
        ImGui::TextDisabled("No audio loaded.");
        return;
    }

    for (int i = 0; i < numCh && i < HRTF_MAX_CHANNELS; i++) {
        ImGui::PushID(i);

        // Color indicator (matches the 3D visualizer's speaker colours).
        ImVec4 color;
        switch (i) {
            case 0: case 1: case 2: color = ImVec4(0.3f, 0.5f, 1.0f, 1.0f); break;
            case 3:                  color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); break;
            case 4: case 5:         color = ImVec4(1.0f, 0.6f, 0.2f, 1.0f); break;
            case 6: case 7:         color = ImVec4(0.3f, 0.8f, 0.3f, 1.0f); break;
            default:                 color = ImVec4(0.7f, 0.3f, 0.9f, 1.0f); break;
        }
        bool isSelected = m_selectedSpeaker && (*m_selectedSpeaker == i);

        ImGui::ColorButton("##color", color, ImGuiColorEditFlags_NoTooltip,
                            ImVec2(12, 12));
        ImGui::SameLine();

        if (isSelected)
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);

        bool isObjectChannel = i >= bedCount;
        bool is61 = (numCh == 7);
        const char* spkName;
        if (isObjectChannel)                       spkName = "Object";
        else if (is61 && i < 7)                    spkName = speakerName61[i];
        else if (i < 12)                           spkName = speakerNames[i];
        else                                       spkName = "Unknown";
        char spkLabel[64];
        if (isObjectChannel)
            snprintf(spkLabel, sizeof(spkLabel), "Object %d (ch %d)",
                     i - bedCount, i);
        else
            snprintf(spkLabel, sizeof(spkLabel), "%s", spkName);

        if (ImGui::TreeNode(spkLabel)) {
            if (m_selectedSpeaker && ImGui::IsItemClicked())
                *m_selectedSpeaker = i;

            bool changed = false;
            float az   = m_state->speaker_pos[i].azimuth;
            float el   = m_state->speaker_pos[i].elevation;
            float dist = m_state->speaker_pos[i].distance;

            changed |= ImGui::SliderFloat("Azimuth",   &az,   -180.0f, 180.0f, "%.1f deg");
            changed |= ImGui::SliderFloat("Elevation", &el,    -90.0f,  90.0f, "%.1f deg");
            changed |= ImGui::SliderFloat("Distance",  &dist,    0.5f,  15.0f, "%.2f m");

            if (changed) {
                m_state->speaker_pos[i].azimuth   = az;
                m_state->speaker_pos[i].elevation = el;
                m_state->speaker_pos[i].distance  = dist;
                atomic_store(&m_state->speaker_pos_changed, 1);
            }

            // RMS level meter + per-channel Test button.
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
                bool toneActive =
                    atomic_load(&m_state->test_tone_active) != 0 &&
                    atomic_load(&m_state->test_tone_channel) == i;
                if (toneActive) {
                    ImGui::TextDisabled("Playing...");
                } else if (ImGui::SmallButton("Test")) {
                    m_player->playTestTone(i);
                }
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }
}
