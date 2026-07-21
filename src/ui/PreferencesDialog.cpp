#include "PreferencesDialog.h"
#include "ControlPanel.h"
#include "app/Window.h"
#include "core/Settings.h"
#include "core/MediaServer.h"
#include "audio/MpvPlayer.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <IconsLucide.h>
#include <ImGuiFileDialog.h>

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

bool langGetter(void* /*data*/, int idx, const char** out) {
    if (idx < 0 || idx >= kLangCount) return false;
    *out = kLangs[idx].name;
    return true;
}

} // anonymous namespace

PreferencesDialog::PreferencesDialog(MpvPlayer* player,
                                     ControlPanel* controlPanel,
                                     Window* window,
                                     MediaServer* mediaServer)
    : m_player(player), m_controlPanel(controlPanel), m_window(window),
      m_mediaServer(mediaServer) {}

void PreferencesDialog::open() {
    m_audioIdx     = findLangIndex(Settings::preferredAudioLang());
    m_subIdx       = findLangIndex(Settings::preferredSubLang());
    m_requestOpen  = true;
    m_freshlyShown = true;
}

void PreferencesDialog::render() {
    if (m_requestOpen) {
        m_isOpen      = true;
        m_requestOpen = false;
    }
    if (!m_isOpen) return;

    // Shoulder buttons cycle tabs — LB previous, RB next, wrapping at
    // the ends so a TV-mode user never lands at a dead stop.  Re-arming
    // m_freshlyShown nudges the gamepad focus onto the newly active
    // pill so the user can immediately bumper through several tabs
    // without losing the cursor down in the body.
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        m_currentTab   = (m_currentTab - 1 + TAB_COUNT) % TAB_COUNT;
        m_freshlyShown = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
        m_currentTab   = (m_currentTab + 1) % TAB_COUNT;
        m_freshlyShown = true;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (m_freshlyShown) ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(48, 32));
    ImGui::Begin("##preferences", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    // Three-level back navigation:
    //   popup open  → B closes the popup (handled by ImGui in NewFrame)
    //   focus body  → B bounces focus up to the active pill
    //   focus pill  → B closes the page
    //
    // We can't use ImGui::Shortcut() here: with the body child using
    // NavFlattened, the route doesn't fire when focus sits on a body
    // widget.  Raw IsKeyPressed() works, but we have to be careful not
    // to consume the same press twice — ImGui's own NavUpdateCancelRequest
    // closes any open popup *during NewFrame*, before this code runs,
    // so by the time we check OpenPopupStack the popup is already gone
    // but the press edge is still readable.  Tracking the previous
    // frame's popup count lets us detect that case and stay out of
    // ImGui's way.
    {
        ImGuiContext* g = ImGui::GetCurrentContext();
        const int  curPopupCount = g ? g->OpenPopupStack.Size : 0;
        const bool popupJustClosed = curPopupCount < m_lastPopupCount;
        const bool popupOpen       = curPopupCount > 0;
        m_lastPopupCount = curPopupCount;

        const bool cancelEdge =
            ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);

        if (cancelEdge && !popupOpen && !popupJustClosed) {
            if (m_focusOnPill) {
                m_isOpen = false;
            } else {
                m_freshlyShown = true;  // bounce up to the active pill
            }
        }
    }

    renderHeader();
    ImGui::Spacing();
    renderTabs();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Active section in a scrollable child so the page itself never
    // scrolls (header + tabs stay pinned at the top).
    ImGui::BeginChild("##prefs_body", ImVec2(0, 0),
                      ImGuiChildFlags_NavFlattened);
    switch (m_currentTab) {
        case TAB_LANGUAGES:  renderLanguages();   break;
        case TAB_SUBTITLES:  renderSubtitles();   break;
        case TAB_DISPLAY:    renderDisplay();     break;
        case TAB_SPATIAL:    renderSpatial();     break;
        case TAB_EQ:         renderEq();          break;
        case TAB_AUDIO_SYNC: renderAudioSync();   break;
        case TAB_GRAIN:      renderCinemaGrain(); break;
        case TAB_SERVER:     renderServer();      break;
        default: break;
    }
    ImGui::EndChild();

    m_freshlyShown = false;
    ImGui::End();
}

void PreferencesDialog::renderHeader() {
    // Back button (left) + Title (centred to the rest of the header).
    const float backW = 110.0f;
    const float titleH = ImGui::GetFontSize() * 1.6f;

    ImVec2 headerStart = ImGui::GetCursorPos();

    // Back button styled like a pill: rounded, with arrow icon.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 8));
    if (ImGui::Button(ICON_LC_ARROW_LEFT "  Back", ImVec2(backW, 0))) {
        m_isOpen = false;
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // Title — same vertical centre as the back button, anchored a bit
    // to the right so it doesn't crowd the button.
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::PushFont(nullptr, titleH);
    ImGui::TextUnformatted(ICON_LC_SETTINGS "  Preferences");
    ImGui::PopFont();
}

void PreferencesDialog::renderTabs() {
    struct TabDef { const char* icon; const char* label; };
    static const TabDef kTabs[TAB_COUNT] = {
        { ICON_LC_LANGUAGES,   "Languages"     },
        { ICON_LC_CAPTIONS,    "Subtitles"     },
        { ICON_LC_EYE,         "Display & HDR" },
        { ICON_LC_EAR,         "Spatial"       },
        { ICON_LC_HEADPHONES,  "Headphone EQ"  },
        { ICON_LC_AUDIO_LINES, "Audio sync"    },
        { ICON_LC_FILM,        "Cinema grain"  },
        { ICON_LC_SERVER,      "Media Server"  },
    };

    // Measure each pill, accumulate total width, then draw centred.
    const float pillPadX = 22.0f;
    const float pillGap  = 10.0f;

    float widths[TAB_COUNT];
    float totalW = 0.0f;
    for (int i = 0; i < TAB_COUNT; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s  %s", kTabs[i].icon, kTabs[i].label);
        widths[i] = ImGui::CalcTextSize(buf).x + pillPadX * 2.0f;
        totalW   += widths[i];
        if (i + 1 < TAB_COUNT) totalW += pillGap;
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    const float startX = ImGui::GetCursorPosX() +
                          std::max(0.0f, (avail - totalW) * 0.5f);
    ImGui::SetCursorPosX(startX);

    // Reset before the loop so any pill that takes focus this frame
    // can flip it back on.
    m_focusOnPill = false;

    for (int i = 0; i < TAB_COUNT; i++) {
        const bool active = (m_currentTab == i);
        if (tabPill(kTabs[i].icon, kTabs[i].label, active)) {
            m_currentTab = i;
        }
        // Seed gamepad / keyboard nav onto the currently active pill
        // the first frame the page is shown.  Doing it on the matching
        // pill (rather than always pill 0) means re-entering the page
        // brings focus straight to whatever the user was last on.
        if (m_freshlyShown && active) ImGui::FocusItem();
        if (i + 1 < TAB_COUNT) ImGui::SameLine(0.0f, pillGap);
    }
}

bool PreferencesDialog::tabPill(const char* icon, const char* label, bool active) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s  %s", icon, label);

    ImGui::PushID(label);
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGuiStyle& style = ImGui::GetStyle();
    const float padX = 22.0f;
    const float padY = 9.0f;
    ImVec2 textSz = ImGui::CalcTextSize(buf);
    ImVec2 size(textSz.x + padX * 2.0f, textSz.y + padY * 2.0f);

    // Suppress the auto nav cursor — same trick as the home tiles.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    ImGui::InvisibleButton("##pill", size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();

    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    const bool pressed = ImGui::IsItemActivated();
    const bool emph    = hovered || focused;
    if (focused) m_focusOnPill = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pmax(origin.x + size.x, origin.y + size.y);
    const float  rounding = size.y * 0.5f;

    // Active pill: filled with the theme's accent (Header colour reads
    // well against our navy palette).  Inactive: transparent body with
    // a subtle border that brightens on hover/focus.
    ImU32 bg     = active
        ? ImGui::GetColorU32(ImGuiCol_HeaderActive)
        : (emph ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered)
                 : IM_COL32(0, 0, 0, 0));
    ImU32 border = emph
        ? ImGui::GetColorU32(ImGuiCol_Text)
        : ImGui::GetColorU32(ImGuiCol_Border);
    ImU32 textCol= active
        ? ImGui::GetColorU32(ImGuiCol_Text)
        : ImGui::GetColorU32(emph ? ImGuiCol_Text : ImGuiCol_TextDisabled);

    dl->AddRectFilled(origin, pmax, bg, rounding);
    dl->AddRect(origin, pmax, border, rounding, 0,
                emph ? 2.0f : 1.0f);
    dl->AddText(ImVec2(origin.x + padX, origin.y + padY), textCol, buf);

    ImGui::PopID();
    return pressed;
}

// ──────────────────────────────────────────────────────────────────
//  Section bodies
// ──────────────────────────────────────────────────────────────────

void PreferencesDialog::renderLanguages() {
    const float labelW = 200.0f;
    const float comboW = ImGui::GetContentRegionAvail().x - labelW - 12.0f;

    ImGui::TextUnformatted("Audio language");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("##aud_lang", &m_audioIdx, langGetter,
                      nullptr, kLangCount)) {
        Settings::setPreferredAudioLang(kLangs[m_audioIdx].code);
    }

    ImGui::Spacing();

    ImGui::TextUnformatted("Subtitle language");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("##sub_lang", &m_subIdx, langGetter,
                      nullptr, kLangCount)) {
        Settings::setPreferredSubLang(kLangs[m_subIdx].code);
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Picker pre-selects the first track matching your preferred\n"
        "language when a file loads.  Set to \"Any\" to disable.");
}

void PreferencesDialog::renderSubtitles() {
    const float labelW = 200.0f;
    Settings::SubtitleStyle s = Settings::subtitleStyle();
    bool dirty = false;

    char fontBuf[128] = {0};
    strncpy(fontBuf, s.font.c_str(), sizeof(fontBuf) - 1);
    ImGui::TextUnformatted("Font");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0f);
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
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderFloat("##sub_size", &s.sizePt, 18.0f, 80.0f, "%.0f pt"))
        dirty = true;

    ImGui::TextUnformatted("Bold");
    ImGui::SameLine(labelW);
    if (ImGui::Checkbox("##sub_bold", &s.bold))
        dirty = true;

    ImGui::Spacing();

    ImGui::TextUnformatted("Color");
    ImGui::SameLine(labelW);
    if (ImGui::ColorEdit4("##sub_color", s.color,
                            ImGuiColorEditFlags_NoInputs))
        dirty = true;

    ImGui::TextUnformatted("Border color");
    ImGui::SameLine(labelW);
    if (ImGui::ColorEdit4("##sub_border_color", s.borderColor,
                            ImGuiColorEditFlags_NoInputs))
        dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("##sub_border_size", &s.borderSize,
                            0.0f, 6.0f, "border %.1f px"))
        dirty = true;

    ImGui::TextUnformatted("Shadow color");
    ImGui::SameLine(labelW);
    if (ImGui::ColorEdit4("##sub_shadow_color", s.shadowColor,
                            ImGuiColorEditFlags_NoInputs))
        dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("##sub_shadow_offset", &s.shadowOffset,
                            0.0f, 6.0f, "offset %.1f px"))
        dirty = true;

    ImGui::TextUnformatted("Background");
    ImGui::SameLine(labelW);
    if (ImGui::ColorEdit4("##sub_back_color", s.backColor,
                            ImGuiColorEditFlags_NoInputs))
        dirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("alpha=0 \xe2\x86\x92 transparent");

    ImGui::Spacing();

    ImGui::TextUnformatted("Bottom margin");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderInt("##sub_margin", &s.marginY, 0, 120, "%d px"))
        dirty = true;

    ImGui::TextUnformatted("Vertical position");
    ImGui::SameLine(labelW);
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

void PreferencesDialog::renderDisplay() {
    ImGui::TextWrapped(
        "How mpv targets the colour pipeline at output.  Defaults "
        "to SDR BT.709 because vo=libmpv can't probe your display; "
        "switch to HDR10 passthrough when your TV is in HDR mode.");
    ImGui::Spacing();

    Settings::DisplayConfig d = Settings::displayConfig();
    bool dirty = false;
    const float labelW = 200.0f;

    static const char* modeNames[] = {
        "Auto (BT.709 / SDR)",
        "Force SDR (BT.709)",
        "HDR10 passthrough (BT.2020 / PQ)"
    };
    ImGui::TextUnformatted("Display mode");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::Combo("##disp_mode", &d.mode,
                      modeNames, IM_ARRAYSIZE(modeNames)))
        dirty = true;

    ImGui::BeginDisabled(d.mode != 2);
    ImGui::TextUnformatted("Display peak");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderFloat("##disp_peak", &d.peakNits,
                            100.0f, 4000.0f, "%.0f nits"))
        dirty = true;
    ImGui::EndDisabled();

    // --- HDR output (automatic) -------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("HDR output");
    PreferencesDialog::HdrStatus hs = m_hdrStatus ? m_hdrStatus() : HdrStatus{};
    ImGui::TextWrapped(
        "OpenGL can't present HDR on Windows, so the final frame is handed "
        "to a Direct3D 11 HDR swapchain. This turns on automatically whenever "
        "Windows HDR is active on the display.");
    const bool forceSdr = d.hdrOutput == 1;
    if (!hs.available) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                           "Status: unavailable (no D3D11 interop).");
    } else if (forceSdr) {
        ImGui::TextDisabled("Status: forced SDR (override on).");
    } else if (hs.displayHdr) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                           "Status: ACTIVE — HDR present (%.0f nits).", hs.maxNits);
    } else {
        ImGui::TextDisabled("Status: idle — Windows HDR is off for this "
                            "display (SDR present). Enable it in Windows "
                            "Display settings and this switches on by itself.");
    }
    bool force = forceSdr;
    if (ImGui::Checkbox("Force SDR output (disable HDR present)", &force)) {
        d.hdrOutput = force ? 1 : 0;
        dirty = true;
    }
    if (d.mode == 2) {
        ImGui::TextDisabled(
            "    Typical OLED peak: 700-1000 nits.  Mini-LED: 1500-2000.");
    }

    static const char* toneNames[] = {
        "BT.2390 (recommended)",
        "Mobius (preserve highlights)",
        "Hable (filmic)",
        "Reinhard (classic)",
        "Clip (no tone mapping)"
    };
    ImGui::TextUnformatted("Tone mapping");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::Combo("##disp_tone", &d.toneAlg,
                      toneNames, IM_ARRAYSIZE(toneNames)))
        dirty = true;

    static const char* gamutNames[] = {
        "Auto",
        "Perceptual (smooth desaturate)",
        "Relative (clip out-of-gamut)",
        "Saturation (preserve hue)"
    };
    ImGui::TextUnformatted("Gamut mapping");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::Combo("##disp_gamut", &d.gamutMode,
                      gamutNames, IM_ARRAYSIZE(gamutNames)))
        dirty = true;

    ImGui::Spacing();
    ImGui::TextDisabled("Black-bar handling");
    ImGui::Spacing();

    ImGui::TextUnformatted("Letterbox crop");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderFloat("##disp_panscan", &d.panscan, 0.0f, 1.0f,
                            "%.2f  (0=preserve aspect, 1=fill screen)"))
        dirty = true;
    ImGui::TextDisabled(
        "    On OLED the bars are perfectly off; raise this only if\n"
        "    you want the image bigger at the cost of cropped edges.");

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults##disp_reset")) {
        d = Settings::DisplayConfig{};
        dirty = true;
    }

    if (dirty) {
        Settings::setDisplayConfig(d);
        Settings::applyDisplayConfigToPlayer(m_player);
    }

    // Window presentation — modelled on the AAA-game pattern.
    // Default Fullscreen so first-run reads as a TV-mode appliance;
    // Borderless trades a tiny amount of latency for friendlier
    // Alt-Tab + overlay behaviour; Windowed is the dev / multitasking
    // option.  Live-applies to the running window so the user sees
    // the swap immediately without a restart.
    ImGui::Spacing();
    ImGui::TextDisabled("Window presentation");
    ImGui::Spacing();

    static const char* winModeNames[] = {
        "Fullscreen (exclusive)",
        "Borderless window",
        "Windowed",
    };
    int winMode = Settings::windowMode();
    ImGui::TextUnformatted("Mode");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::Combo("##win_mode", &winMode,
                      winModeNames, IM_ARRAYSIZE(winModeNames))) {
        Settings::setWindowMode(winMode);
        if (m_window) m_window->setMode(static_cast<WindowMode>(winMode));
    }
    ImGui::TextDisabled(
        "    Fullscreen takes over the display.  Borderless covers the\n"
        "    monitor without the title bar but stays a regular window\n"
        "    (better Alt-Tab / overlay behaviour).  Windowed leaves a\n"
        "    resizable frame around the app.");
}

void PreferencesDialog::renderAudioSync() {
    ImGui::TextWrapped(
        "Global offset between audio and video.  Useful when listening "
        "through a Bluetooth headset (audio arrives late) or with a "
        "wired DAC + monitor combo where one path lags the other.");
    ImGui::Spacing();

    Settings::PlaybackConfig pc = Settings::playbackConfig();
    bool dirty = false;
    const float labelW = 200.0f;

    ImGui::TextUnformatted("Audio delay");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderFloat("##aud_delay", &pc.audioDelay,
                            -2.0f, 2.0f, "%+.3f s"))
        dirty = true;
    ImGui::TextDisabled(
        "    Negative = audio arrives earlier (compensates a slow video\n"
        "    path); positive = audio arrives later (compensates BT lag).");

    ImGui::Spacing();
    if (ImGui::Button("Reset##aud_reset")) {
        pc.audioDelay = 0.0f;
        dirty = true;
    }

    if (dirty) {
        Settings::setPlaybackConfig(pc);
        Settings::applyPlaybackConfigToPlayer(m_player);
    }
}

void PreferencesDialog::renderCinemaGrain() {
    ImGui::TextWrapped(
        "Real-time emulation of a 35mm release print being projected. "
        "This is GPU-bound — works fine on dGPUs and modern APUs (e.g. "
        "ROG Ally Z2 Extreme).  Older integrated graphics may drop "
        "frames at 4K.");
    ImGui::Spacing();

    Settings::CinemaGrain g = Settings::cinemaGrain();
    bool dirty = false;
    const float labelW = 200.0f;

    if (ImGui::Checkbox("Enable", &g.enabled))
        dirty = true;

    ImGui::BeginDisabled(!g.enabled);

    static const char* stockNames[] = {
        "New release print",
        "Repertory print",
        "Worn 16mm",
        "Custom"
    };
    struct StockPreset {
        float intensity;
        float grainSize;
        float lumAdaptive;
        float chroma;
    };
    static const StockPreset stockPresets[] = {
        { 0.05f, 1.2f, 0.20f, 0.30f },
        { 0.10f, 1.6f, 0.30f, 0.40f },
        { 0.18f, 2.4f, 0.35f, 0.55f },
    };
    ImGui::TextUnformatted("Stock");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::Combo("##grain_stock", &g.stock,
                      stockNames, IM_ARRAYSIZE(stockNames))) {
        if (g.stock >= 0 && g.stock < 3) {
            const auto& p   = stockPresets[g.stock];
            g.intensity     = p.intensity;
            g.grainSize     = p.grainSize;
            g.lumAdaptive   = p.lumAdaptive;
            g.chroma        = p.chroma;
        }
        dirty = true;
    }

    auto sliderTouched = [&]() {
        g.stock = 3;
        dirty   = true;
    };

    ImGui::TextUnformatted("Intensity");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderFloat("##grain_intensity", &g.intensity,
                            0.0f, 0.30f, "%.3f"))
        sliderTouched();

    ImGui::TextUnformatted("Grain size");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderFloat("##grain_size", &g.grainSize,
                            0.5f, 4.0f, "%.2f px"))
        sliderTouched();

    ImGui::TextUnformatted("Tonal adaptive");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderFloat("##grain_lum", &g.lumAdaptive,
                            0.0f, 1.0f,
                            "%.2f  (0=uniform print, 1=peaks at midtones)"))
        sliderTouched();

    ImGui::TextUnformatted("Color decorrelation");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
    if (ImGui::SliderFloat("##grain_chroma", &g.chroma,
                            0.0f, 1.0f,
                            "%.2f  (0=mono noise, 1=full RGB)"))
        sliderTouched();

    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults##grain_reset")) {
        g = Settings::CinemaGrain{};
        dirty = true;
    }

    if (dirty) {
        Settings::setCinemaGrain(g);
        Settings::applyCinemaGrainToPlayer(m_player);
    }
}

// ──────────────────────────────────────────────────────────────────
//  Spatial / EQ — bodies live on ControlPanel so the same widgets can
//  also be hosted from the 3D Visualizer panel without duplicating the
//  state-binding logic for every atomic field on HrtfSharedState.
// ──────────────────────────────────────────────────────────────────

void PreferencesDialog::renderSpatial() {
    if (m_controlPanel) m_controlPanel->renderSpatialContent();
    else                ImGui::TextDisabled("(spatial controls unavailable)");
}

void PreferencesDialog::renderEq() {
    if (m_controlPanel) m_controlPanel->renderEqContent();
    else                ImGui::TextDisabled("(EQ controls unavailable)");
}

void PreferencesDialog::renderServer() {
    ImGui::TextWrapped(
        "Host the HaloSound media server: the TV app connects to this "
        "machine over the local network, video streams straight from "
        "the source file and the multichannel audio is spatialized on "
        "the TV.  Requires Node.js on this machine.");
    ImGui::Spacing();

    if (!m_mediaServer) {
        ImGui::TextDisabled("(server management unavailable)");
        return;
    }

    Settings::ServerConfig cfg = Settings::serverConfig();
    bool dirty = false;
    const float labelW = 200.0f;
    const bool running = m_mediaServer->isRunning();

    // --- Status line -----------------------------------------------------
    {
        const ImVec4 green(0.35f, 0.85f, 0.45f, 1.0f);
        const ImVec4 grey (0.55f, 0.55f, 0.58f, 1.0f);
        ImGui::TextUnformatted("Status");
        ImGui::SameLine(labelW);
        ImGui::TextColored(running ? green : grey,
                           running ? ICON_LC_SERVER "  Running" : ICON_LC_SERVER_OFF "  Stopped");
        if (running) {
            const std::string ip = MediaServer::lanAddress();
            if (!ip.empty()) {
                ImGui::TextUnformatted("TV address");
                ImGui::SameLine(labelW);
                ImGui::Text("%s : %d", ip.c_str(), m_mediaServer->port());
                ImGui::SameLine();
                ImGui::TextDisabled("(enter this in the HaloSound TV app)");
            }
        } else if (!m_mediaServer->lastError().empty()) {
            ImGui::TextUnformatted(" ");
            ImGui::SameLine(labelW);
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s",
                               m_mediaServer->lastError().c_str());
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Config ----------------------------------------------------------
    ImGui::TextUnformatted("Media folder");
    ImGui::SameLine(labelW);
    {
        const float browseW = 130.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseW - 16.0f);
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", cfg.mediaDir.c_str());
        if (ImGui::InputText("##srv_dir", buf, sizeof(buf))) {
            cfg.mediaDir = buf;
            dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_LC_FOLDER "  Browse", ImVec2(browseW, 0))) {
            IGFD::FileDialogConfig fdCfg;
            fdCfg.path = cfg.mediaDir.empty() ? "." : cfg.mediaDir;
            fdCfg.flags = ImGuiFileDialogFlags_Modal;
            ImGuiFileDialog::Instance()->OpenDialog(
                "choose_media_dir", "Select media folder", nullptr, fdCfg);
        }
    }

    ImGui::TextUnformatted("HTTP port");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputInt("##srv_port", &cfg.port, 0, 0)) {
        if (cfg.port < 1)     cfg.port = 1;
        if (cfg.port > 65534) cfg.port = 65534;
        dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(audio WebSocket uses port + 1)");

    if (ImGui::Checkbox("Start server automatically with the app", &cfg.enabled))
        dirty = true;

    ImGui::Spacing();

    // --- Start / Stop ------------------------------------------------------
    if (!running) {
        ImGui::BeginDisabled(cfg.mediaDir.empty());
        if (ImGui::Button(ICON_LC_SERVER "  Start server", ImVec2(220, 0)))
            m_mediaServer->start(cfg.mediaDir, cfg.port);
        ImGui::EndDisabled();
        if (cfg.mediaDir.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("select a media folder first");
        }
    } else {
        if (ImGui::Button(ICON_LC_SERVER_OFF "  Stop server", ImVec2(220, 0)))
            m_mediaServer->stop();
    }

    // Folder-picker result.
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    ImVec2 dlgMin(640, 420);
    ImVec2 dlgMax(vpSize.x * 0.9f, vpSize.y * 0.9f);
    if (ImGuiFileDialog::Instance()->Display("choose_media_dir", 0, dlgMin, dlgMax)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            cfg.mediaDir = ImGuiFileDialog::Instance()->GetCurrentPath();
            dirty = true;
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (dirty)
        Settings::setServerConfig(cfg);
}
