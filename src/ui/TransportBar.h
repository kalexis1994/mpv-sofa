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

    using ControlsCallback = std::function<void()>;
    void setControlsCallback(ControlsCallback cb) { m_controlsCb = cb; }

private:
    MpvPlayer* m_player;
    FullscreenCallback m_fullscreenCb;
    ControlsCallback m_controlsCb;
    bool m_isFullscreen = false;

    void formatTime(double seconds, char* buf, int bufSize);
};
