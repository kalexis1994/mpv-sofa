#pragma once
// PreferencesDialog — full-screen settings page opened from the home
// screen / Settings menu / Ctrl+,.  Replaces the older centred modal:
// the new layout is a single page with pill-style tabs at the top and
// the active section's controls below, sized to the full viewport so
// it reads as a proper screen on a TV-mode setup.  All sections live-
// apply to the current Settings + mpv player; there is no OK/Cancel.

class MpvPlayer;
class ControlPanel;

class PreferencesDialog {
public:
    PreferencesDialog(MpvPlayer* player = nullptr,
                       ControlPanel* controlPanel = nullptr);

    void open();
    void render();
    bool isOpen() const { return m_requestOpen || m_isOpen; }
    void close() { m_isOpen = false; m_requestOpen = false; }

private:
    enum Tab {
        TAB_LANGUAGES = 0,
        TAB_SUBTITLES,
        TAB_DISPLAY,
        TAB_SPATIAL,
        TAB_EQ,
        TAB_AUDIO_SYNC,
        TAB_GRAIN,
        TAB_COUNT
    };

    // Top chrome.
    void renderHeader();
    void renderTabs();

    // Active section bodies.
    void renderLanguages();
    void renderSubtitles();
    void renderDisplay();
    void renderSpatial();
    void renderEq();
    void renderAudioSync();
    void renderCinemaGrain();

    // Pill-style tab button.  Returns true on activation.
    bool tabPill(const char* icon, const char* label, bool active);

    MpvPlayer*    m_player       = nullptr;
    ControlPanel* m_controlPanel = nullptr;

    bool m_requestOpen  = false;
    bool m_isOpen       = false;

    // True the first frame the page is visible — used to seed nav focus
    // onto the active tab so the gamepad has somewhere to start from.
    bool m_freshlyShown = true;

    // True if any tab pill currently owns nav focus.  Updated each
    // frame inside tabPill() / renderTabs(); the back-nav handler reads
    // last frame's value to decide whether B should bounce focus up
    // from the body to the pills, or close the page entirely.
    // NavWindow name comparison is unreliable here because the body
    // child uses ImGuiChildFlags_NavFlattened — focus on a body widget
    // still reports NavWindow == ##preferences.
    bool m_focusOnPill = false;

    // Last frame's OpenPopupStack.Size, used to detect "ImGui just
    // closed a popup in NewFrame() this frame".  When that happens the
    // B/Esc press edge is still readable but we must NOT consume it as
    // a page-close gesture, since the same press already paid for the
    // popup's dismissal.
    int  m_lastPopupCount = 0;

    int  m_currentTab   = TAB_LANGUAGES;

    // Languages now write through directly on change (live apply), but
    // we still keep the editable indices as state so the combo boxes
    // have something stable to bind to between frames.
    int  m_audioIdx = 0;
    int  m_subIdx   = 0;
};
