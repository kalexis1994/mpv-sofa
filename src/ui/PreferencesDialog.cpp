#include "PreferencesDialog.h"
#include "core/Settings.h"
#include "audio/MpvPlayer.h"

#include <imgui.h>
#include <IconsLucide.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

// Curated language list using ISO 639-2/B/T canonical codes.  The empty
// code at index 0 means "no preference" (let the file's default play).
struct LangChoice { const char* code; const char* name; };
const LangChoice kLangs[] = {
    {"",    "Any (use file's default)"},
    {"eng", "English"},
    {"spa", "Spanish"},
    {"fre", "French"},
    {"ger", "German"},
    {"ita", "Italian"},
    {"por", "Portuguese"},
    {"dut", "Dutch"},
    {"swe", "Swedish"},
    {"nor", "Norwegian"},
    {"dan", "Danish"},
    {"fin", "Finnish"},
    {"pol", "Polish"},
    {"rus", "Russian"},
    {"tur", "Turkish"},
    {"ara", "Arabic"},
    {"hin", "Hindi"},
    {"jpn", "Japanese"},
    {"kor", "Korean"},
    {"chi", "Chinese"},
};
const int kLangCount = (int)(sizeof(kLangs) / sizeof(kLangs[0]));

int findLangIndex(const std::string& code) {
    if (code.empty()) return 0;
    for (int i = 1; i < kLangCount; i++) {
        if (Settings::langMatches(code, kLangs[i].code))
            return i;
    }
    return 0;
}

} // anonymous namespace

PreferencesDialog::PreferencesDialog(MpvPlayer* player) : m_player(player) {}

void PreferencesDialog::open() {
    // Snapshot the current preference into the editable index when opening.
    m_audioIdx    = findLangIndex(Settings::preferredAudioLang());
    m_subIdx      = findLangIndex(Settings::preferredSubLang());
    m_requestOpen = true;
}

void PreferencesDialog::render() {
    if (m_requestOpen) {
        ImGui::OpenPopup("##preferences");
        m_isOpen      = true;
        m_requestOpen = false;
    }
    if (!m_isOpen) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 size(std::min(640.0f, vp->Size.x * 0.85f),
                 std::min(620.0f, vp->Size.y * 0.85f));
    ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
                vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    bool open = true;
    if (!ImGui::BeginPopupModal("##preferences", &open,
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize   |
                                 ImGuiWindowFlags_NoMove     |
                                 ImGuiWindowFlags_NoScrollbar)) {
        if (!open) m_isOpen = false;
        return;
    }

    ImGui::Text(ICON_LC_SETTINGS "  Preferences");
    ImGui::Separator();
    ImGui::Spacing();

    auto langGetter = [](void* /*data*/, int idx, const char** out) -> bool {
        if (idx < 0 || idx >= kLangCount) return false;
        *out = kLangs[idx].name;
        return true;
    };

    // Reserve room for the OK / Cancel footer so the inner sections
    // can scroll without pushing the buttons off-screen.
    const float footerH = ImGui::GetFrameHeightWithSpacing() +
                           ImGui::GetStyle().ItemSpacing.y * 2.0f + 4.0f;
    ImGui::BeginChild("##prefs_body", ImVec2(0, -footerH), false);

    if (ImGui::CollapsingHeader(ICON_LC_LANGUAGES "  Languages",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        const float labelW = 170.0f;
        const float comboW = ImGui::GetContentRegionAvail().x - labelW - 12.0f;

        ImGui::TextUnformatted("Audio language");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(comboW);
        ImGui::Combo("##aud_lang", &m_audioIdx, langGetter, nullptr, kLangCount);

        ImGui::Spacing();

        ImGui::TextUnformatted("Subtitle language");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(comboW);
        ImGui::Combo("##sub_lang", &m_subIdx, langGetter, nullptr, kLangCount);

        ImGui::Spacing();
        ImGui::TextDisabled(
            "Picker pre-selects the first track matching your preferred\n"
            "language when a file loads.  Set to \"Any\" to disable.");
        ImGui::Spacing();
    }

    if (ImGui::CollapsingHeader(ICON_LC_CAPTIONS "  Subtitle style",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        // Live preview: every change writes to Settings AND pushes to mpv
        // so the user sees the effect on currently-playing subs.  No
        // OK/Cancel for these — the global Settings dirty path handles
        // persistence on shutdown.
        Settings::SubtitleStyle s = Settings::subtitleStyle();
        bool dirty = false;

        // Font name with a small preset menu next to it.
        char fontBuf[128] = {0};
        strncpy(fontBuf, s.font.c_str(), sizeof(fontBuf) - 1);
        ImGui::TextUnformatted("Font");
        ImGui::SameLine(170.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
        if (ImGui::InputText("##sub_font", fontBuf, sizeof(fontBuf))) {
            s.font = fontBuf;
            dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Presets")) ImGui::OpenPopup("##font_presets");
        if (ImGui::BeginPopup("##font_presets")) {
            static const char* fonts[] = {
                "Sans", "Serif", "Monospace",
                "Liberation Sans", "Arial", "Tahoma", "Segoe UI",
                "Open Sans", "Roboto", "Helvetica",
                "Times New Roman", "Liberation Serif"
            };
            for (auto* f : fonts) {
                if (ImGui::Selectable(f)) {
                    s.font = f;
                    dirty  = true;
                }
            }
            ImGui::EndPopup();
        }

        ImGui::TextUnformatted("Size");
        ImGui::SameLine(170.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
        if (ImGui::SliderFloat("##sub_size", &s.sizePt, 18.0f, 80.0f, "%.0f pt"))
            dirty = true;

        ImGui::TextUnformatted("Bold");
        ImGui::SameLine(170.0f);
        if (ImGui::Checkbox("##sub_bold", &s.bold))
            dirty = true;

        ImGui::Spacing();

        ImGui::TextUnformatted("Color");
        ImGui::SameLine(170.0f);
        if (ImGui::ColorEdit4("##sub_color", s.color,
                                ImGuiColorEditFlags_NoInputs))
            dirty = true;

        ImGui::TextUnformatted("Border color");
        ImGui::SameLine(170.0f);
        if (ImGui::ColorEdit4("##sub_border_color", s.borderColor,
                                ImGuiColorEditFlags_NoInputs))
            dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::SliderFloat("##sub_border_size", &s.borderSize,
                                0.0f, 6.0f, "border %.1f px"))
            dirty = true;

        ImGui::TextUnformatted("Shadow color");
        ImGui::SameLine(170.0f);
        if (ImGui::ColorEdit4("##sub_shadow_color", s.shadowColor,
                                ImGuiColorEditFlags_NoInputs))
            dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::SliderFloat("##sub_shadow_offset", &s.shadowOffset,
                                0.0f, 6.0f, "offset %.1f px"))
            dirty = true;

        ImGui::TextUnformatted("Background");
        ImGui::SameLine(170.0f);
        if (ImGui::ColorEdit4("##sub_back_color", s.backColor,
                                ImGuiColorEditFlags_NoInputs))
            dirty = true;
        ImGui::SameLine();
        ImGui::TextDisabled("alpha=0 \xe2\x86\x92 transparent");

        ImGui::Spacing();

        ImGui::TextUnformatted("Bottom margin");
        ImGui::SameLine(170.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
        if (ImGui::SliderInt("##sub_margin", &s.marginY, 0, 120, "%d px"))
            dirty = true;

        ImGui::TextUnformatted("Vertical position");
        ImGui::SameLine(170.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
        if (ImGui::SliderInt("##sub_pos", &s.pos, 0, 100,
                              "%d (0=top, 100=bottom)"))
            dirty = true;

        ImGui::Spacing();
        if (ImGui::Button("Reset to defaults##sub_reset")) {
            s = Settings::SubtitleStyle{};
            dirty = true;
        }

        if (dirty) {
            Settings::setSubtitleStyle(s);
            Settings::applySubtitleStyleToPlayer(m_player);
        }
    }

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Spacing();

    const float btnW = 100.0f;
    const float endX = ImGui::GetContentRegionAvail().x +
                        ImGui::GetCursorPosX();
    ImGui::SameLine(endX - btnW * 2 - 8.0f);
    if (ImGui::Button("Cancel", ImVec2(btnW, 0))) {
        ImGui::CloseCurrentPopup();
        m_isOpen = false;
    }
    ImGui::SameLine(0, 8);
    if (ImGui::Button("OK", ImVec2(btnW, 0))) {
        Settings::setPreferredAudioLang(kLangs[m_audioIdx].code);
        Settings::setPreferredSubLang  (kLangs[m_subIdx  ].code);
        ImGui::CloseCurrentPopup();
        m_isOpen = false;
    }

    ImGui::EndPopup();
    if (!open) m_isOpen = false;
}
