#pragma once
// TrackPicker — modal that lists every audio track of a freshly loaded
// file as a card grid (codec / channel layout / language / title), so
// the user can pick the right track before playback starts.

class MpvPlayer;

class TrackPicker {
public:
    explicit TrackPicker(MpvPlayer* player);

    // Open the modal next frame.  Caller is responsible for triggering
    // this only when m_player has audio tracks worth picking from.
    void open();

    // Render the modal if open.  Click on a card → set audio track and
    // unpause; Cancel → leave paused with current default.
    void render();

    bool isOpen() const { return m_requestOpen || m_isOpen; }

private:
    MpvPlayer* m_player;
    bool m_requestOpen = false;   // Set by open(); consumed by render().
    bool m_isOpen      = false;   // Mirrors ImGui's modal state.
};
