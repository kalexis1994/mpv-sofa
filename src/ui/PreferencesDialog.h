#pragma once
// PreferencesDialog — full-screen settings page opened from the home
// screen / Settings menu / Ctrl+,.  Replaces the older centred modal:
// the new layout is a single page with pill-style tabs at the top and
// the active section's controls below, sized to the full viewport so
// it reads as a proper screen on a TV-mode setup.  All sections live-
// apply to the current Settings + mpv player; there is no OK/Cancel.

class MpvPlayer;

class PreferencesDialog {
public:
    explicit PreferencesDialog(MpvPlayer* player = nullptr);

    void open();
    void render();
    bool isOpen() const { return m_requestOpen || m_isOpen; }
    void close() { m_isOpen = false; m_requestOpen = false; }

private:
    enum Tab {
        TAB_LANGUAGES = 0,
        TAB_SUBTITLES,
        TAB_DISPLAY,
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
    void renderAudioSync();
    void renderCinemaGrain();

    // Pill-style tab button.  Returns true on activation.
    bool tabPill(const char* icon, const char* label, bool active);

    MpvPlayer* m_player = nullptr;

    bool m_requestOpen  = false;
    bool m_isOpen       = false;

    // True the first frame the page is visible — used to seed nav focus
    // onto the active tab so the gamepad has somewhere to start from.
    bool m_freshlyShown = true;

    int  m_currentTab   = TAB_LANGUAGES;

    // Languages now write through directly on change (live apply), but
    // we still keep the editable indices as state so the combo boxes
    // have something stable to bind to between frames.
    int  m_audioIdx = 0;
    int  m_subIdx   = 0;
};
