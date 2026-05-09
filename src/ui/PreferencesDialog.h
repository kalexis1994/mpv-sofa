#pragma once
// PreferencesDialog — modal opened from the top Settings menu.  Houses
// user-level preferences that don't belong on the main control panel
// (preferred audio / subtitle language for now; further sections can
// stack underneath).  Persisted through the Settings module's normal
// dirty + auto-save cycle.

class PreferencesDialog {
public:
    PreferencesDialog();

    void open();
    void render();
    bool isOpen() const { return m_requestOpen || m_isOpen; }

private:
    bool m_requestOpen = false;
    bool m_isOpen      = false;

    // Editable buffers populated on open(); committed to Settings on OK.
    int  m_audioIdx = 0;
    int  m_subIdx   = 0;
};
