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

    // Open the audio-track / subtitle pickers from the toolbar buttons.
    using PickerCallback = std::function<void()>;
    void setAudioPickerCallback(PickerCallback cb) { m_audioPickerCb = cb; }
    void setSubPickerCallback(PickerCallback cb)   { m_subPickerCb   = cb; }

    // Open the in-playback menu (Open / Recent / Settings / Exit).
    using MenuCallback = std::function<void()>;
    void setMenuCallback(MenuCallback cb) { m_menuCb = cb; }

private:
    MpvPlayer* m_player;
    FullscreenCallback m_fullscreenCb;
    ControlsCallback m_controlsCb;
    PickerCallback m_audioPickerCb;
    PickerCallback m_subPickerCb;
    MenuCallback m_menuCb;
    bool m_isFullscreen = false;

    void formatTime(double seconds, char* buf, int bufSize);
};
