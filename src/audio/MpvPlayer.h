#pragma once

#include <string>
#include <vector>

struct HrtfSharedState;

#ifdef HAVE_MPV
struct mpv_handle;
struct mpv_render_context;
#endif

struct AudioTrack {
    int id;
    int ffIndex = -1;   // absolute ffmpeg stream index (for external tools)
    std::string lang;
    std::string title;
    std::string codec;
    std::string channelLayout;
    int channels;
    int samplerate;
    int bitrate;
    bool isDefault;
    bool isForced;
    bool selected;
    bool isExternal = false;   // audio-add'ed file (e.g. binaural sidecar)
};

struct SubtitleTrack {
    int id;
    std::string lang;
    std::string title;
    std::string codec;
    bool isDefault;
    bool isForced;
    bool selected;
};

struct Chapter {
    double time;        // seconds
    std::string title;  // empty if untitled
};

class MpvPlayer {
public:
    MpvPlayer();
    ~MpvPlayer();

    bool init(HrtfSharedState* sharedState);
    void shutdown();

    void loadFile(const std::string& path);
    void play();
    void pause();
    void togglePause();
    void seek(double seconds);
    void seekRelative(double seconds);
    void setAudioTrack(int id);
    // Play a pre-rendered binaural sidecar (external audio track, HRTF
    // filter bypassed) / return to internal track + HRTF.
    // Switch playback to the binaural sidecar (raw f32le stereo 48k, may
    // still be growing) / back to the embedded track + HRTF filter. The
    // revert really re-selects the internal track, so it also serves as the
    // live fallback when a seek outruns a still-rendering sidecar.
    void useExternalBinaural(const std::string& pcmPath);
    void revertInternalAudio();
    bool usingExternalBinaural() const { return m_externalBinaural; }
    void setSubtitleTrack(int id);
    void toggleSubtitles();
    void loadSubtitleFile(const std::string& path);
    void adjustSubDelay(double deltaSec);
    void resetSubDelay();
    void playTestTone(int channel);

    // Volume & mute (mpv volume is 0-100, we clamp on the way in).
    void   setVolume(double v);
    double getVolume() const { return m_volume; }
    void   setMute(bool m);
    void   toggleMute() { setMute(!m_muted); }
    bool   isMuted() const { return m_muted; }

    // Playback speed (0.25 - 4.0 typical).
    void   setSpeed(double s);
    double getSpeed() const { return m_speed; }

    // Chapter navigation.  m_chapters is refreshed on FILE_LOADED and on
    // chapter-list property changes.
    const std::vector<Chapter>& getChapters() const { return m_chapters; }
    int  getCurrentChapterIndex() const;  // -1 if none
    void seekToChapter(int idx);
    void prevChapter();
    void nextChapter();

    // Frame step (only meaningful while paused).
    void frameStep();
    void frameStepBack();

    // Generic property setters used by Settings to push subtitle styling
    // (sub-font / sub-color / sub-bold / …) without coupling Settings to
    // libmpv directly.  Silently no-op when the player isn't initialised.
    void setStringProperty(const char* name, const char* value);
    void setDoubleProperty(const char* name, double value);
    void setIntProperty   (const char* name, int   value);
    void setFlagProperty  (const char* name, bool  value);

    void update();
    void renderToFBO(unsigned int fbo, int width, int height);
    bool needsRender() const { return m_renderRequested; }

    // Returns true once after a fresh load completes; the flag self-clears
    // so the host app can use it as a one-shot "show track picker" trigger.
    bool consumeFreshFileLoaded();

    bool hasVideo() const { return m_hasVideo; }
    bool isPaused() const { return m_paused; }
    double getPosition() const { return m_position; }
    double getDuration() const { return m_duration; }
    std::string getFilename() const { return m_filename; }
    const std::vector<AudioTrack>& getAudioTracks() const { return m_audioTracks; }
    int getCurrentAudioTrackId() const { return m_currentAudioTrack; }
    const std::vector<SubtitleTrack>& getSubtitleTracks() const { return m_subtitleTracks; }
    int getCurrentSubtitleTrackId() const { return m_currentSubtitleTrack; }
    bool areSubtitlesVisible() const { return m_subtitlesVisible; }
    double getSubDelay() const { return m_subDelay; }

private:
    void refreshTrackList();
    void refreshChapterList();

#ifdef HAVE_MPV
    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_renderCtx = nullptr;
#endif

    HrtfSharedState* m_sharedState = nullptr;
    bool m_hasVideo = false;
    bool m_renderRequested = false;
    bool m_paused = true;
    double m_position = 0.0;
    double m_duration = 0.0;
    std::string m_filename;
    std::string m_afChain;               // saved HRTF filter option
    bool        m_externalBinaural = false;
    int         m_internalAid = -1;      // track to return to on revert
    std::vector<AudioTrack> m_audioTracks;
    int m_currentAudioTrack = 0;
    std::vector<SubtitleTrack> m_subtitleTracks;
    int m_currentSubtitleTrack = 0;
    bool m_subtitlesVisible = true;
    double m_subDelay = 0.0;
    bool m_testToneWasPaused = false;
    bool m_testToneSilenceSource = false;
    bool m_verboseMpvLogs = false;
    std::string m_silencePath;

    double m_volume = 100.0;
    bool   m_muted  = false;
    double m_speed  = 1.0;
    std::vector<Chapter> m_chapters;

    bool m_freshFileLoaded = false;
};
