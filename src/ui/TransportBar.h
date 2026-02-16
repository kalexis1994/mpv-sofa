#pragma once

#include <functional>

class MpvPlayer;

class TransportBar {
public:
    TransportBar(MpvPlayer* player);
    void render();
    void renderContent();  // render controls without Begin/End wrapper

    using FullscreenCallback = std::function<void()>;
    void setFullscreenCallback(FullscreenCallback cb) { m_fullscreenCb = cb; }
    void setFullscreenState(bool fs) { m_isFullscreen = fs; }

private:
    MpvPlayer* m_player;
    FullscreenCallback m_fullscreenCb;
    bool m_isFullscreen = false;

    void formatTime(double seconds, char* buf, int bufSize);
};
