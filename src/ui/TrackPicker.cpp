#include "TrackPicker.h"
#include "ImGuiLayer.h"
#include "audio/MpvPlayer.h"
#include "core/Settings.h"

#include <imgui.h>
#include <IconsLucide.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

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

// Pretty codec for subtitle streams.
std::string subCodecLabel(const SubtitleTrack& s) {
    static const std::unordered_map<std::string, std::string> map = {
        {"subrip",                "SubRip"},
        {"ass",                   "Advanced SubStation"},
        {"ssa",                   "SubStation Alpha"},
        {"webvtt",                "WebVTT"},
        {"hdmv_pgs_subtitle",     "PGS (Blu-ray)"},
        {"dvd_subtitle",          "VobSub (DVD)"},
        {"dvb_subtitle",          "DVB"},
        {"mov_text",              "Mov Text"},
    };
    std::string lc = s.codec;
    std::transform(lc.begin(), lc.end(), lc.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    auto it = map.find(lc);
    if (it != map.end()) return it->second;
    return s.codec.empty() ? std::string("Subtitle") : s.codec;
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

// Loot-style rarity tiers, driven by channel count.  Spatial-capable
// streams (Atmos / DD+ JOC / DTS:X) bump the tier by one (capped at
// Legendary), so a "5.1 Atmos" track reads as Epic instead of Rare.
enum class Rarity { Common = 0, Uncommon, Rare, Epic, Legendary };

Rarity rarityFor(const AudioTrack& t) {
    Rarity r;
    if      (t.channels >= 10) r = Rarity::Legendary;   // 7.1.2 / 7.1.4 / 5.1.4 / 7.1.6 / 9.1.6
    else if (t.channels >=  8) r = Rarity::Epic;        // 7.1
    else if (t.channels >=  6) r = Rarity::Rare;        // 5.1 / 6.1
    else if (t.channels >=  4) r = Rarity::Uncommon;    // Quad / 4 ch
    else                        r = Rarity::Common;     // Mono / Stereo / 2.1
    if (isSpatialCapable(t) && r < Rarity::Legendary)
        r = (Rarity)((int)r + 1);
    return r;
}

// Tier border / accent colour.  Common returns 0 → caller falls back to
// the regular ImGuiCol_Border so the card looks unchanged.
ImU32 rarityColor(Rarity r) {
    switch (r) {
        case Rarity::Uncommon:  return IM_COL32( 92, 184,  92, 255); // green
        case Rarity::Rare:      return IM_COL32( 91, 192, 222, 255); // blue
        case Rarity::Epic:      return IM_COL32(157, 123, 216, 255); // purple
        case Rarity::Legendary: return IM_COL32(240, 173,  78, 255); // gold
        default:                return 0;
    }
}

// Slightly brighter sibling for the hover state.
ImU32 rarityColorHover(Rarity r) {
    switch (r) {
        case Rarity::Uncommon:  return IM_COL32(140, 220, 140, 255);
        case Rarity::Rare:      return IM_COL32(150, 220, 240, 255);
        case Rarity::Epic:      return IM_COL32(195, 165, 240, 255);
        case Rarity::Legendary: return IM_COL32(255, 210, 130, 255);
        default:                return 0;
    }
}

// Friendly language name from an ISO 639-1 / 639-2/B / 639-2/T code.
//
// Both 639-2 variants have to be here: the bibliographic codes (fre, ger,
// dut, chi, gre, …) and the terminological ones (fra, deu, nld, zho, ell,
// …) show up in the wild for the same language, sometimes in the same
// file.  A WEB-DL with 36 subtitle tracks will happily use the /T set
// throughout, and anything missing from this table surfaces in the UI as a
// bare three-letter code, which reads like the language is unknown.
//
// A region suffix ("pt-BR", "es-419", "zh-Hans") is resolved on the base
// code and the region kept as a qualifier.
std::string langLabel(const std::string& codeIn) {
    if (codeIn.empty()) return "";

    std::string code = codeIn;
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    std::string region;
    if (const size_t dash = code.find_first_of("-_"); dash != std::string::npos) {
        region = codeIn.substr(dash + 1);
        code   = code.substr(0, dash);
    }

    static const std::unordered_map<std::string, std::string> map = {
        {"ara","Arabic"},      {"ar","Arabic"},
        {"bul","Bulgarian"},   {"bg","Bulgarian"},
        {"cat","Catalan"},     {"ca","Catalan"},
        {"ces","Czech"},       {"cze","Czech"},   {"cs","Czech"},
        {"chi","Chinese"},     {"zho","Chinese"}, {"zh","Chinese"},
        {"dan","Danish"},      {"da","Danish"},
        {"deu","German"},      {"ger","German"},  {"de","German"},
        {"ell","Greek"},       {"gre","Greek"},   {"el","Greek"},
        {"eng","English"},     {"en","English"},
        {"est","Estonian"},    {"et","Estonian"},
        {"eus","Basque"},      {"baq","Basque"},  {"eu","Basque"},
        {"fas","Persian"},     {"per","Persian"}, {"fa","Persian"},
        {"fin","Finnish"},     {"fi","Finnish"},
        {"fra","French"},      {"fre","French"},  {"fr","French"},
        {"glg","Galician"},    {"gl","Galician"},
        {"heb","Hebrew"},      {"he","Hebrew"},
        {"hin","Hindi"},       {"hi","Hindi"},
        {"hrv","Croatian"},    {"hr","Croatian"},
        {"hun","Hungarian"},   {"hu","Hungarian"},
        {"ind","Indonesian"},  {"id","Indonesian"},
        {"isl","Icelandic"},   {"ice","Icelandic"},{"is","Icelandic"},
        {"ita","Italian"},     {"it","Italian"},
        {"jpn","Japanese"},    {"ja","Japanese"},
        {"kor","Korean"},      {"ko","Korean"},
        {"lav","Latvian"},     {"lv","Latvian"},
        {"lit","Lithuanian"},  {"lt","Lithuanian"},
        {"mkd","Macedonian"},  {"mac","Macedonian"},{"mk","Macedonian"},
        {"msa","Malay"},       {"may","Malay"},   {"ms","Malay"},
        {"nld","Dutch"},       {"dut","Dutch"},   {"nl","Dutch"},
        {"nno","Norwegian Nynorsk"},              {"nn","Norwegian Nynorsk"},
        {"nob","Norwegian Bokmal"},               {"nb","Norwegian Bokmal"},
        {"nor","Norwegian"},   {"no","Norwegian"},
        {"pol","Polish"},      {"pl","Polish"},
        {"por","Portuguese"},  {"pt","Portuguese"},
        {"ron","Romanian"},    {"rum","Romanian"},{"ro","Romanian"},
        {"rus","Russian"},     {"ru","Russian"},
        {"slk","Slovak"},      {"slo","Slovak"},  {"sk","Slovak"},
        {"slv","Slovenian"},   {"sl","Slovenian"},
        {"spa","Spanish"},     {"es","Spanish"},
        {"srp","Serbian"},     {"scc","Serbian"}, {"sr","Serbian"},
        {"swe","Swedish"},     {"sv","Swedish"},
        {"tha","Thai"},        {"th","Thai"},
        {"tur","Turkish"},     {"tr","Turkish"},
        {"ukr","Ukrainian"},   {"uk","Ukrainian"},
        {"vie","Vietnamese"},  {"vi","Vietnamese"},
        {"und","Undetermined"},
    };

    auto it = map.find(code);
    std::string name = (it != map.end()) ? it->second : code;
    if (!region.empty()) name += " (" + region + ")";
    return name;
}

// Pill-shaped tag.  Returns the width consumed (including outer padding),
// so the caller can flow tags horizontally with wrapping.
struct TagStyle {
    ImU32 bg;
    ImU32 fg;
    ImU32 border;
};

// Tag palettes derived from the live theme, so accents and neutrals track
// whatever the user picked in Preferences instead of being pinned to the
// colours of an older palette.
TagStyle accentTagStyle() {
    ImVec4 acc = ImGuiLayer::accentColor();
    ImVec4 soft = acc; soft.w = 0.22f;
    return { ImGui::GetColorU32(soft), ImGui::GetColorU32(acc), 0 };
}

TagStyle neutralTagStyle() {
    return { ImGui::GetColorU32(ImGuiCol_ScrollbarGrab),
             ImGui::GetColorU32(ImGuiCol_TextDisabled), 0 };
}

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

    // EnableNav lets DPad / left-stick land focus on each card; without
    // it InvisibleButton registers as ImGuiItemFlags_NoNav and the
    // gamepad cursor skips over the entire grid.  The transparent
    // NavCursor push suppresses ImGui's default focus rectangle so the
    // custom rarity-coloured ring below is the only highlight.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    ImGui::InvisibleButton("##card", ImVec2(w, h),
                            ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    const bool clicked = ImGui::IsItemActivated();
    const bool emph    = hovered || focused;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg      = ImGui::GetColorU32(emph ? ImGuiCol_FrameBgHovered
                                                  : ImGuiCol_FrameBg);
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim  = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    // Loot-tier outline: more channels (and spatial) → richer border.
    const Rarity rarity   = rarityFor(t);
    const ImU32 tierCol   = rarityColor(rarity);
    const ImU32 tierHov   = rarityColorHover(rarity);
    ImU32 border;
    float borderThick;
    if (tierCol) {
        border      = emph ? tierHov : tierCol;
        borderThick = (rarity >= Rarity::Epic) ? 2.5f
                    : (rarity >= Rarity::Uncommon) ? 2.0f : 1.0f;
        if (focused) borderThick += 1.0f;   // bump on nav-focus
    } else {
        border      = ImGui::GetColorU32(emph ? ImGuiCol_Text
                                              : ImGuiCol_Border);
        borderThick = emph ? 1.5f : 1.0f;
    }

    const ImVec2 pmax(origin.x + w, origin.y + h);
    dl->AddRectFilled(origin, pmax, bg, 6.0f);
    dl->AddRect(origin, pmax, border, 6.0f, 0, borderThick);

    dl->PushClipRect(ImVec2(origin.x + 4, origin.y + 4),
                     ImVec2(pmax.x - 4, pmax.y - 4), true);

    const float lh   = ImGui::GetTextLineHeight();
    const float padX = 14.0f;
    const float padY = 12.0f;
    float       yc   = origin.y + padY;

    // Codec heading — tinted toward the tier colour for Epic / Legendary
    // so the eye lands on the high-end tracks first.
    const std::string codec = codecLabel(t);
    const ImU32 codecCol = (rarity >= Rarity::Epic) ? tierCol : colText;
    dl->AddText(ImVec2(origin.x + padX, yc), codecCol, codec.c_str());
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

    // Technical facts stay neutral; the things worth spotting from across
    // the grid (spatial audio, the default track) carry the accent.
    const TagStyle tagTech    = neutralTagStyle();
    const TagStyle tagBitrate = neutralTagStyle();
    const TagStyle tagLang    = neutralTagStyle();
    const TagStyle tagSpatial = accentTagStyle();
    const TagStyle tagDefault = accentTagStyle();
    const TagStyle tagForced  = neutralTagStyle();

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

// Card for a subtitle track.  Same chrome as the audio card so the modal
// reads the same in either mode, just with subtitle-relevant info on it.
bool drawSubCard(const SubtitleTrack& t, float w, float h) {
    ImGui::PushID(0x10000 | t.id);  // separate ID-space from audio cards
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    ImGui::InvisibleButton("##subcard", ImVec2(w, h),
                            ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    const bool clicked = ImGui::IsItemActivated();
    const bool emph    = hovered || focused;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg      = ImGui::GetColorU32(emph ? ImGuiCol_FrameBgHovered
                                                  : ImGuiCol_FrameBg);
    const ImU32 border  = ImGui::GetColorU32(emph ? ImGuiCol_Text
                                                  : ImGuiCol_Border);
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImVec2 pmax(origin.x + w, origin.y + h);

    dl->AddRectFilled(origin, pmax, bg, 6.0f);
    dl->AddRect(origin, pmax, border, 6.0f, 0, focused ? 2.5f : (hovered ? 1.5f : 1.0f));

    dl->PushClipRect(ImVec2(origin.x + 4, origin.y + 4),
                     ImVec2(pmax.x - 4, pmax.y - 4), true);

    const float lh   = ImGui::GetTextLineHeight();
    const float padX = 14.0f;
    const float padY = 12.0f;
    float       yc   = origin.y + padY;

    // A subtitle track's identity is its language — every other field is
    // the same across the whole list (a WEB-DL ships 36 mov_text tracks),
    // so the language leads and the format drops to the dim second line.
    const std::string lang = langLabel(t.lang);
    dl->AddText(ImVec2(origin.x + padX, yc), colText,
                lang.empty() ? "Unknown language" : lang.c_str());
    yc += lh + 6.0f;

    {
        std::string second = subCodecLabel(t);
        if (!t.title.empty()) {
            std::string title = t.title;
            const size_t maxLen = 28;
            if (title.size() > maxLen)
                title = title.substr(0, maxLen - 1) + "\xe2\x80\xa6";
            second += "  \xc2\xb7  " + title;
        }
        dl->AddText(ImVec2(origin.x + padX, yc), colDim, second.c_str());
        yc += lh + 8.0f;
    }

    const TagStyle tagAccent  = accentTagStyle();
    const TagStyle tagNeutral = neutralTagStyle();

    struct TagEntry { std::string text; const TagStyle* style; };
    std::vector<TagEntry> tags;
    // Files routinely carry two tracks per language (regular + SDH) with no
    // titles to tell them apart; the id at least makes them addressable.
    {
        char b[16]; snprintf(b, sizeof(b), "#%d", t.id);
        tags.push_back({b, &tagNeutral});
    }
    if (t.isDefault) tags.push_back({ICON_LC_STAR "  Default", &tagAccent});
    if (t.isForced)  tags.push_back({"Forced",                 &tagNeutral});

    const float gapX = 6.0f, gapY = 6.0f;
    const float tagH = lh + 6.0f;
    const float maxX = origin.x + w - padX;
    float       cx   = origin.x + padX;
    for (const auto& tag : tags) {
        const ImVec2 ts = ImGui::CalcTextSize(tag.text.c_str());
        const float tw  = ts.x + 16.0f;
        if (cx + tw > maxX) { cx = origin.x + padX; yc += tagH + gapY; }
        if (yc + tagH > origin.y + h - padY) break;
        drawTag(dl, ImVec2(cx, yc), tag.text.c_str(), *tag.style);
        cx += tw + gapX;
    }

    dl->PopClipRect();
    ImGui::PopID();
    return clicked;
}

// Card representing "subtitles off" — visually identical chrome but with
// a single muted line so it sits naturally as the first option in the
// subtitle grid.
bool drawSubOffCard(float w, float h, bool selected) {
    ImGui::PushID("##suboff");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    ImGui::InvisibleButton("##suboffbtn", ImVec2(w, h),
                            ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    const bool clicked = ImGui::IsItemActivated();
    const bool emph    = hovered || focused;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg      = ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive
                                          : (emph     ? ImGuiCol_FrameBgHovered
                                                      : ImGuiCol_FrameBg));
    const ImU32 border  = ImGui::GetColorU32(emph || selected
                                              ? ImGuiCol_Text
                                              : ImGuiCol_Border);
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImVec2 pmax(origin.x + w, origin.y + h);

    dl->AddRectFilled(origin, pmax, bg, 6.0f);
    dl->AddRect(origin, pmax, border, 6.0f, 0, focused ? 2.5f : (hovered ? 1.5f : 1.0f));

    const float padX = 14.0f, padY = 12.0f;
    const float lh   = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(origin.x + padX, origin.y + padY),
                colText, ICON_LC_X "  Off");
    dl->AddText(ImVec2(origin.x + padX, origin.y + padY + lh + 6.0f),
                colDim, "No subtitles displayed");

    ImGui::PopID();
    return clicked;
}

} // anonymous namespace

TrackPicker::TrackPicker(MpvPlayer* player) : m_player(player) {}

void TrackPicker::open(Mode mode, bool isAutoLoad) {
    m_mode        = mode;
    m_isAutoLoad  = isAutoLoad;
    m_requestOpen = true;
}

namespace {

// Layout helper: compute card width using the same responsive
// "repeat(auto-fit, minmax(minW, 1fr))" pattern in both modes.
struct GridDims { int cols; float cardW; };
GridDims computeGrid(float availW, float spacingX,
                      float minCardW, float maxCardW) {
    int cols = std::max(1,
        (int)((availW + spacingX) / (minCardW + spacingX)));
    float cardW = (availW - spacingX * (cols - 1)) / (float)cols;
    if (cardW > maxCardW) cardW = maxCardW;
    return {cols, cardW};
}

} // anonymous namespace

void TrackPicker::render() {
    if (!m_player) return;

    if (m_requestOpen) {
        ImGui::OpenPopup("##track_picker");
        m_isOpen      = true;
        m_requestOpen = false;
    }
    if (!m_isOpen) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 size(std::min(1200.0f, vp->Size.x * 0.9f),
                 std::min(680.0f,  vp->Size.y * 0.85f));
    ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
                vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    bool open = true;
    if (!ImGui::BeginPopupModal("##track_picker", &open,
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize   |
                                 ImGuiWindowFlags_NoMove     |
                                 ImGuiWindowFlags_NoScrollbar)) {
        if (!open) m_isOpen = false;
        return;
    }

    // Header
    const char* title = (m_mode == Mode::Subtitle)
                          ? ICON_LC_CAPTIONS "  Select subtitle"
                          : ICON_LC_AUDIO_LINES "  Select audio track";
    ImGui::Text("%s", title);
    std::string fname = m_player->getFilename();
    if (!fname.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("\xc2\xb7  %s", fname.c_str());
    }
    ImGui::Separator();

    ImGuiStyle& style = ImGui::GetStyle();

    if (m_mode == Mode::Audio) {
        const auto& tracks = m_player->getAudioTracks();
        if (tracks.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("No audio tracks in this file.");
        } else {
            const float footerH = ImGui::GetFrameHeightWithSpacing() +
                                   style.ItemSpacing.y * 2.0f + 4.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::BeginChild("##cards", ImVec2(0, -footerH), false);
            ImGui::PopStyleVar();

            const float cardH = 170.0f;
            const auto g = computeGrid(ImGui::GetContentRegionAvail().x,
                                        style.ItemSpacing.x, 240.0f, 340.0f);

            // Render order: language match first, then most channels,
            // then by track ID for stability.  Keeps the user's preferred
            // language up top *and* leads each language block with its
            // best stream (Atmos / 7.1.4 / 7.1 / 5.1 / Stereo).
            const std::string& prefLang = Settings::preferredAudioLang();
            std::vector<size_t> order(tracks.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(),
                      [&](size_t a, size_t b) {
                bool aMatch = Settings::langMatches(tracks[a].lang, prefLang);
                bool bMatch = Settings::langMatches(tracks[b].lang, prefLang);
                if (aMatch != bMatch) return aMatch;
                if (tracks[a].channels != tracks[b].channels)
                    return tracks[a].channels > tracks[b].channels;
                return tracks[a].id < tracks[b].id;
            });

            int picked = -1;
            for (size_t i = 0; i < order.size(); i++) {
                if ((int)(i % g.cols) != 0) ImGui::SameLine();
                if (drawCard(tracks[order[i]], g.cardW, cardH))
                    picked = (int)order[i];
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::TextDisabled(m_isAutoLoad
                ? "Click a track to start playback."
                : "Click a track to switch.");
            const float btnW = 100.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x +
                             ImGui::GetCursorPosX() - btnW);
            if (ImGui::Button("Cancel", ImVec2(btnW, 0))) {
                ImGui::CloseCurrentPopup();
                m_isOpen = false;
            }

            if (picked >= 0 && picked < (int)tracks.size()) {
                m_player->setAudioTrack(tracks[picked].id);
                if (m_isAutoLoad) m_player->play();
                ImGui::CloseCurrentPopup();
                m_isOpen = false;
            }
        }
    } else {
        // Subtitle mode.
        const auto& subs = m_player->getSubtitleTracks();
        const int currentSubId = m_player->getCurrentSubtitleTrackId();

        // Footer here is two rows: a strip with visibility / delay /
        // load-file controls, separator, then the Text + Cancel button.
        const float strip1H = ImGui::GetFrameHeightWithSpacing();
        const float strip2H = ImGui::GetFrameHeightWithSpacing();
        const float footerH = strip1H + strip2H +
                               style.ItemSpacing.y * 4.0f + 4.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##subcards", ImVec2(0, -footerH), false);
        ImGui::PopStyleVar();

        const float cardH = 130.0f;     // shorter — sub cards carry less
        const auto g = computeGrid(ImGui::GetContentRegionAvail().x,
                                    style.ItemSpacing.x, 220.0f, 320.0f);

        // Sort subs by preferred-language match first, then by id.
        const std::string& prefSub = Settings::preferredSubLang();
        std::vector<size_t> order(subs.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) {
            bool aMatch = Settings::langMatches(subs[a].lang, prefSub);
            bool bMatch = Settings::langMatches(subs[b].lang, prefSub);
            if (aMatch != bMatch) return aMatch;
            return subs[a].id < subs[b].id;
        });

        int picked = -2;   // -2 = no pick, -1 = "Off", >=0 = sub index
        // "Off" card always first.
        if (drawSubOffCard(g.cardW, cardH, currentSubId == 0))
            picked = -1;
        for (size_t i = 0; i < order.size(); i++) {
            int idx = (int)(i + 1);            // +1 because Off is at 0
            if ((idx % g.cols) != 0) ImGui::SameLine();
            if (drawSubCard(subs[order[i]], g.cardW, cardH))
                picked = (int)order[i];
        }
        ImGui::EndChild();

        // Footer strip 1: visibility toggle + delay slider + reset
        bool subVis = m_player->areSubtitlesVisible();
        if (ImGui::Checkbox("Visible", &subVis))
            m_player->toggleSubtitles();
        ImGui::SameLine(0, style.ItemSpacing.x * 2);

        ImGui::TextDisabled("Sync");
        ImGui::SameLine();
        float delay = (float)m_player->getSubDelay();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("##subdelay", &delay, -10.0f, 10.0f, "%+.2f s"))
            m_player->adjustSubDelay(delay - (float)m_player->getSubDelay());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset"))
            m_player->resetSubDelay();
        ImGui::SameLine(0, style.ItemSpacing.x * 2);

        if (ImGui::Button(ICON_LC_FILE_PLUS "  Load file...")) {
#ifdef _WIN32
            char filePath[MAX_PATH] = {0};
            OPENFILENAMEA ofn = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = "Subtitle Files\0*.srt;*.ass;*.ssa;*.sub;*.vtt\0All Files\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrTitle = "Load Subtitle File";
            if (GetOpenFileNameA(&ofn))
                m_player->loadSubtitleFile(filePath);
#endif
        }

        ImGui::Separator();

        ImGui::TextDisabled("Click a card to switch the active subtitle.");
        const float btnW = 100.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x +
                         ImGui::GetCursorPosX() - btnW);
        if (ImGui::Button("Close", ImVec2(btnW, 0))) {
            ImGui::CloseCurrentPopup();
            m_isOpen = false;
        }

        // Apply selection.
        if (picked == -1) {
            m_player->setSubtitleTrack(0);
            ImGui::CloseCurrentPopup();
            m_isOpen = false;
        } else if (picked >= 0 && picked < (int)subs.size()) {
            m_player->setSubtitleTrack(subs[picked].id);
            ImGui::CloseCurrentPopup();
            m_isOpen = false;
        }
    }

    ImGui::EndPopup();
    if (!open) m_isOpen = false;
}
