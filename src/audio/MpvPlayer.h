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
    void setSubtitleTrack(int id);
    void toggleSubtitles();
    void loadSubtitleFile(const std::string& path);
    void adjustSubDelay(double deltaSec);
    void resetSubDelay();
    void playTestTone(int channel);

    void update();
    void renderToFBO(unsigned int fbo, int width, int height);
    bool needsRender() const { return m_renderRequested; }

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
    std::vector<AudioTrack> m_audioTracks;
    int m_currentAudioTrack = 0;
    std::vector<SubtitleTrack> m_subtitleTracks;
    int m_currentSubtitleTrack = 0;
    bool m_subtitlesVisible = true;
    double m_subDelay = 0.0;
    bool m_testToneWasPaused = false;
    bool m_testToneSilenceSource = false;
    std::string m_silencePath;
};
