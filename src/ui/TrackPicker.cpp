#include "TrackPicker.h"
#include "audio/MpvPlayer.h"

#include <imgui.h>
#include <IconsLucide.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

// Friendly name for a channel layout.  Prefers mpv's channelLayout string
// (e.g. "5.1", "7.1+2") when present and meaningful, falls back to a
// count-based label.  mpv emits "unknownN" (e.g. "unknown6") for tracks
// whose layout metadata can't be mapped to a named layout — treat those
// as missing so the card shows "5.1" / "7.1" / etc. derived from the
// channel count instead of a meaningless label.
std::string layoutLabel(const AudioTrack& t) {
    bool layoutMeaningful = !t.channelLayout.empty() &&
                             t.channelLayout.compare(0, 7, "unknown") != 0;
    if (layoutMeaningful) {
        // mpv often returns "5.1(side)" or "7.1(wide)" — strip the
        // parenthetical so the card stays compact.
        std::string s = t.channelLayout;
        auto p = s.find('(');
        if (p != std::string::npos) s = s.substr(0, p);
        return s;
    }
    switch (t.channels) {
        case 1:  return "Mono";
        case 2:  return "Stereo";
        case 3:  return "2.1";
        case 4:  return "Quad";
        case 6:  return "5.1";
        case 7:  return "6.1";
        case 8:  return "7.1";
        case 10: return "7.1.2";
        case 12: return "7.1.4";
        case 14: return "7.1.6";
        case 16: return "9.1.6";
        default: {
            char b[16];
            snprintf(b, sizeof(b), "%d ch", t.channels);
            return b;
        }
    }
}

// Pretty codec.  mpv reports lowercase identifiers like "truehd",
// "eac3", "ac3", "aac" — show them with a more familiar capitalisation.
std::string codecLabel(const AudioTrack& t) {
    static const std::unordered_map<std::string, std::string> map = {
        {"truehd",   "Dolby TrueHD"},
        {"eac3",     "Dolby Digital Plus"},
        {"ac3",      "Dolby Digital"},
        {"dts",      "DTS"},
        {"dts-hd",   "DTS-HD"},
        {"flac",     "FLAC"},
        {"opus",     "Opus"},
        {"vorbis",   "Vorbis"},
        {"aac",      "AAC"},
        {"mp3",      "MP3"},
        {"pcm_s16le","PCM 16-bit"},
        {"pcm_s24le","PCM 24-bit"},
        {"pcm_s32le","PCM 32-bit"},
        {"pcm_f32le","PCM Float"},
    };
    std::string lc = t.codec;
    std::transform(lc.begin(), lc.end(), lc.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    auto it = map.find(lc);
    if (it != map.end()) return it->second;
    return t.codec.empty() ? std::string("Unknown") : t.codec;
}

// True when the codec + channel count strongly suggest the stream
// carries object-based audio (Dolby Atmos / DD+ JOC).
bool isSpatialCapable(const AudioTrack& t) {
    if (t.codec.empty()) return false;
    std::string lc = t.codec;
    std::transform(lc.begin(), lc.end(), lc.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (lc == "truehd" && t.channels >= 6) return true;     // Atmos
    if (lc == "eac3"   && t.channels >= 6) return true;     // DD+ JOC
    if (lc == "dts-hd" && t.channels >= 6) return true;     // DTS:X
    return false;
}

// Friendly language name from ISO 639-2/B/T or 639-1 code.  Falls back
// to the raw code when unknown.
std::string langLabel(const std::string& code) {
    if (code.empty()) return "";
    static const std::unordered_map<std::string, std::string> map = {
        {"eng","English"},     {"en","English"},
        {"spa","Spanish"},     {"es","Spanish"},
        {"fre","French"},      {"fra","French"}, {"fr","French"},
        {"ger","German"},      {"deu","German"}, {"de","German"},
        {"ita","Italian"},     {"it","Italian"},
        {"por","Portuguese"},  {"pt","Portuguese"},
        {"jpn","Japanese"},    {"ja","Japanese"},
        {"kor","Korean"},      {"ko","Korean"},
        {"chi","Chinese"},     {"zho","Chinese"}, {"zh","Chinese"},
        {"rus","Russian"},     {"ru","Russian"},
        {"ara","Arabic"},      {"ar","Arabic"},
        {"hin","Hindi"},       {"hi","Hindi"},
        {"dut","Dutch"},       {"nld","Dutch"},   {"nl","Dutch"},
        {"swe","Swedish"},     {"sv","Swedish"},
        {"nor","Norwegian"},   {"no","Norwegian"},
        {"dan","Danish"},      {"da","Danish"},
        {"fin","Finnish"},     {"fi","Finnish"},
        {"pol","Polish"},      {"pl","Polish"},
        {"tur","Turkish"},     {"tr","Turkish"},
        {"und","Undetermined"},
    };
    auto it = map.find(code);
    if (it != map.end()) return it->second;
    return code;
}

// Pill-shaped tag.  Returns the width consumed (including outer padding),
// so the caller can flow tags horizontally with wrapping.
struct TagStyle {
    ImU32 bg;
    ImU32 fg;
    ImU32 border;
};

float drawTag(ImDrawList* dl, ImVec2 pos, const char* text,
               const TagStyle& s) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float padX = 8.0f;
    const float padY = 3.0f;
    const ImVec2 pmin = pos;
    const ImVec2 pmax(pos.x + ts.x + padX * 2, pos.y + ts.y + padY * 2);
    const float rounding = (pmax.y - pmin.y) * 0.5f;
    dl->AddRectFilled(pmin, pmax, s.bg, rounding);
    if (s.border)
        dl->AddRect(pmin, pmax, s.border, rounding, 0, 1.0f);
    dl->AddText(ImVec2(pmin.x + padX, pmin.y + padY), s.fg, text);
    return ts.x + padX * 2;
}

// Render one card.  Everything inside the card is drawn through
// ImDrawList so the only ImGui-layout item we register is the
// InvisibleButton — otherwise ImGui's row-tracking gets confused by the
// cursor jumps and adjacent cards overlap.
bool drawCard(const AudioTrack& t, float w, float h) {
    ImGui::PushID(t.id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##card", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemActivated();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg      = ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered
                                                     : ImGuiCol_FrameBg);
    const ImU32 border  = ImGui::GetColorU32(hovered ? ImGuiCol_Text
                                                     : ImGuiCol_Border);
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImVec2 pmax(origin.x + w, origin.y + h);

    dl->AddRectFilled(origin, pmax, bg, 6.0f);
    dl->AddRect(origin, pmax, border, 6.0f, 0, hovered ? 1.5f : 1.0f);

    dl->PushClipRect(ImVec2(origin.x + 4, origin.y + 4),
                     ImVec2(pmax.x - 4, pmax.y - 4), true);

    const float lh   = ImGui::GetTextLineHeight();
    const float padX = 14.0f;
    const float padY = 12.0f;
    float       yc   = origin.y + padY;

    // Codec heading.
    const std::string codec = codecLabel(t);
    dl->AddText(ImVec2(origin.x + padX, yc), colText, codec.c_str());
    yc += lh + 6.0f;

    // Title, dimmed and truncated, just below the heading.
    if (!t.title.empty()) {
        std::string title = t.title;
        const size_t maxLen = 36;
        if (title.size() > maxLen)
            title = title.substr(0, maxLen - 1) + "\xe2\x80\xa6";
        dl->AddText(ImVec2(origin.x + padX, yc), colDim, title.c_str());
        yc += lh + 8.0f;
    } else {
        yc += 2.0f;
    }

    // Categorical pill palette.  Each tag type gets its own colour so the
    // grid reads at a glance; bg colours sit a step or two away from the
    // navy card background and the text colour is chosen for contrast on
    // each pill.
    const ImU32 sand = IM_COL32(210, 193, 182, 255);   // #D2C1B6
    const ImU32 deep = IM_COL32( 27,  60,  83, 255);   // #1B3C53

    const TagStyle tagTech    = { IM_COL32( 69, 104, 130, 255), sand, 0 }; // steel
    const TagStyle tagBitrate = { IM_COL32( 60, 120, 130, 255), sand, 0 }; // teal
    const TagStyle tagLang    = { IM_COL32(150, 110,  90, 255), sand, 0 }; // sienna
    const TagStyle tagSpatial = { IM_COL32(140, 175, 130, 255), deep, 0 }; // sage green
    const TagStyle tagDefault = { IM_COL32(210, 178,  95, 255), deep, 0 }; // amber gold
    const TagStyle tagForced  = { IM_COL32(180,  90,  80, 255), sand, 0 }; // muted red

    // Build the tag list in render order.  Strings stay alive until the
    // function returns since we hold them in std::string locals.
    struct TagEntry { std::string text; const TagStyle* style; };
    std::vector<TagEntry> tags;
    tags.push_back({layoutLabel(t),                                       &tagTech});
    {
        char b[16]; snprintf(b, sizeof(b), "%d ch", t.channels);
        tags.push_back({b,                                                 &tagTech});
    }
    if (t.samplerate > 0) {
        char b[16]; snprintf(b, sizeof(b), "%d kHz", t.samplerate / 1000);
        tags.push_back({b,                                                 &tagTech});
    }
    if (t.bitrate > 0) {
        char b[24];
        if (t.bitrate >= 1000000)
            snprintf(b, sizeof(b), "%.1f Mbps", t.bitrate / 1000000.0);
        else
            snprintf(b, sizeof(b), "%d kbps",   t.bitrate / 1000);
        tags.push_back({b,                                                 &tagBitrate});
    }
    {
        std::string lang = langLabel(t.lang);
        if (!lang.empty()) {
            char b[64]; snprintf(b, sizeof(b), ICON_LC_LANGUAGES "  %s",
                                  lang.c_str());
            tags.push_back({b,                                             &tagLang});
        }
    }
    if (isSpatialCapable(t))
        tags.push_back({ICON_LC_AUDIO_LINES "  Spatial",                   &tagSpatial});
    if (t.isDefault)
        tags.push_back({ICON_LC_STAR "  Default",                          &tagDefault});
    if (t.isForced)
        tags.push_back({"Forced",                                          &tagForced});
    (void)colDim;

    // Flow tags horizontally with line wrap inside the card.
    const float gapX = 6.0f;
    const float gapY = 6.0f;
    const float tagH = lh + 6.0f;
    const float maxX = origin.x + w - padX;
    float       cx   = origin.x + padX;
    for (const auto& tag : tags) {
        const ImVec2 ts = ImGui::CalcTextSize(tag.text.c_str());
        const float tw  = ts.x + 16.0f;
        if (cx + tw > maxX) {
            cx  = origin.x + padX;
            yc += tagH + gapY;
        }
        if (yc + tagH > origin.y + h - padY) break;   // ran out of room
        drawTag(dl, ImVec2(cx, yc), tag.text.c_str(), *tag.style);
        cx += tw + gapX;
    }

    dl->PopClipRect();
    ImGui::PopID();
    return clicked;
}

} // anonymous namespace

TrackPicker::TrackPicker(MpvPlayer* player) : m_player(player) {}

void TrackPicker::open() { m_requestOpen = true; }

void TrackPicker::render() {
    if (!m_player) return;

    if (m_requestOpen) {
        ImGui::OpenPopup("##track_picker");
        m_isOpen      = true;
        m_requestOpen = false;
    }
    if (!m_isOpen) return;

    // Sized as a comfortable but not full-screen modal.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 size(std::min(1200.0f, vp->Size.x * 0.9f),
                 std::min(680.0f,  vp->Size.y * 0.85f));
    ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
                vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    bool open = true;
    if (ImGui::BeginPopupModal("##track_picker", &open,
                                ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoResize   |
                                ImGuiWindowFlags_NoMove     |
                                ImGuiWindowFlags_NoScrollbar)) {
        // Header
        ImGui::Text(ICON_LC_AUDIO_LINES "  Select audio track");
        std::string fname = m_player->getFilename();
        if (!fname.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("·  %s", fname.c_str());
        }
        ImGui::Separator();

        const auto& tracks = m_player->getAudioTracks();
        if (tracks.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("No audio tracks in this file.");
        } else {
            ImGuiStyle& style = ImGui::GetStyle();

            // Reserve space for the footer so the grid scrolls if needed.
            // Footer layout: Separator (≈ ItemSpacing.y * 2 + 1) followed
            // by a Text/Button row sharing one frame height.  Reserving
            // only a single frame height (the original mistake) left the
            // modal a few pixels short and gave it its own scrollbar.
            const float footerH = ImGui::GetFrameHeightWithSpacing() +
                                   style.ItemSpacing.y * 2.0f + 4.0f;
            // Drop the child window's own padding so the responsive math
            // gets every pixel of horizontal real estate the modal offers.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::BeginChild("##cards", ImVec2(0, -footerH), false);
            ImGui::PopStyleVar();

            // Responsive grid à la CSS `repeat(auto-fit, minmax(minW, 1fr))`:
            // pick as many columns as fit at minCardW, then stretch each
            // card so the row fills the available width — no awkward gap
            // on the right that begs the eye for "one more card".
            const float minCardW = 240.0f;
            const float maxCardW = 340.0f;
            const float cardH    = 170.0f;
            const float spacingX = style.ItemSpacing.x;
            const float availW   = ImGui::GetContentRegionAvail().x;
            int cols = std::max(1,
                (int)((availW + spacingX) / (minCardW + spacingX)));
            float cardW = (availW - spacingX * (cols - 1)) / (float)cols;
            if (cardW > maxCardW) cardW = maxCardW;

            int picked = -1;
            for (size_t i = 0; i < tracks.size(); i++) {
                if ((int)(i % cols) != 0)
                    ImGui::SameLine();
                if (drawCard(tracks[i], cardW, cardH))
                    picked = (int)i;
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::TextDisabled("Click a track to start playback.");
            float btnW = 100.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x +
                             ImGui::GetCursorPosX() - btnW);
            if (ImGui::Button("Cancel", ImVec2(btnW, 0))) {
                ImGui::CloseCurrentPopup();
                m_isOpen = false;
            }

            if (picked >= 0 && picked < (int)tracks.size()) {
                m_player->setAudioTrack(tracks[picked].id);
                m_player->play();
                ImGui::CloseCurrentPopup();
                m_isOpen = false;
            }
        }

        ImGui::EndPopup();
    } else if (!open) {
        m_isOpen = false;
    }
}
