#pragma once
// PreferencesDialog — modal opened from the top Settings menu.  Houses
// user-level preferences that don't belong on the main control panel
// (preferred audio / subtitle language, subtitle styling, …).  Persisted
// through the Settings module's normal dirty + auto-save cycle.

class MpvPlayer;

class PreferencesDialog {
public:
    explicit PreferencesDialog(MpvPlayer* player = nullptr);

    void open();
    void render();
    bool isOpen() const { return m_requestOpen || m_isOpen; }

private:
    MpvPlayer* m_player = nullptr;
    bool m_requestOpen  = false;
    bool m_isOpen       = false;

    // Language combos use the OK / Cancel commit pattern (they only take
    // effect on the next file load anyway).  Subtitle styling commits
    // immediately for live preview.
    int  m_audioIdx = 0;
    int  m_subIdx   = 0;
};
