#include "PreferencesDialog.h"
#include "core/Settings.h"

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

PreferencesDialog::PreferencesDialog() = default;

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
    ImVec2 size(std::min(540.0f, vp->Size.x * 0.85f),
                 std::min(360.0f, vp->Size.y * 0.85f));
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

    ImGui::TextDisabled("Languages");
    ImGui::Spacing();

    auto langGetter = [](void* /*data*/, int idx, const char** out) -> bool {
        if (idx < 0 || idx >= kLangCount) return false;
        *out = kLangs[idx].name;
        return true;
    };

    // Two combos with a consistent label width so they line up.
    const float labelW = 160.0f;
    const float comboW = ImGui::GetContentRegionAvail().x - labelW - 12.0f;

    ImGui::TextUnformatted("Audio language");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(comboW);
    ImGui::Combo("##aud_lang", &m_audioIdx, langGetter, nullptr, kLangCount);

    ImGui::Spacing();

    ImGui::TextUnformatted("Subtitle language");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(comboW);
    ImGui::Combo("##sub_lang", &m_subIdx,   langGetter, nullptr, kLangCount);

    ImGui::Spacing();
    ImGui::TextDisabled(
        "When a file loads, the picker will pre-select the first track\n"
        "matching your preferred language.  Set to \"Any\" to disable.");

    // Footer: Cancel / OK aligned right.
    ImGui::Spacing();
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
