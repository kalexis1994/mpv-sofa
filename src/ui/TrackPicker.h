#pragma once
// TrackPicker — robust modal that lays out the audio (or subtitle) tracks
// of the current file as a card grid.  Used both as the auto-fire panel
// after a fresh load (so the user picks the audio stream up-front while
// playback is still paused) and as the dialog the transport-bar buttons
// open to switch tracks during playback.

class MpvPlayer;

class TrackPicker {
public:
    enum class Mode { Audio, Subtitle };

    explicit TrackPicker(MpvPlayer* player);

    // Open the modal.  When isAutoLoad is true, picking a card additionally
    // resumes playback (the file was held paused from before loadfile).
    // When false (manual button press during playback), picking a card
    // only switches the track and leaves the play/pause state alone.
    void open(Mode mode = Mode::Audio, bool isAutoLoad = false);

    // Render the modal if open.
    void render();

    bool isOpen() const { return m_requestOpen || m_isOpen; }

private:
    MpvPlayer* m_player;
    Mode m_mode        = Mode::Audio;
    bool m_isAutoLoad  = false;
    bool m_requestOpen = false;   // set by open(); consumed by render()
    bool m_isOpen      = false;
};
