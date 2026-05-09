#pragma once

#include "Window.h"
#include "renderer/Renderer.h"
#include "renderer/Camera.h"
#include "ui/ImGuiLayer.h"
#include "ui/ControlPanel.h"
#include "ui/TransportBar.h"
#include "ui/TrackPicker.h"
#include "ui/PreferencesDialog.h"
#include "audio/MpvPlayer.h"

#include <memory>
#include <string>

struct HrtfSharedState;

class Application {
public:
    Application();
    ~Application();

    bool init(int argc, char* argv[]);
    void run();
    void shutdown();

private:
    void processInput();
    void update(float dt);
    void render();
    void renderUI();

    // Big-button landing screen rendered when no file is loaded.  Acts
    // as the TV-mode entry point: Open / Recent / Settings / Exit, all
    // navigable from a gamepad's d-pad.
    void renderHomeScreen();
    bool homeButton(const char* icon, const char* label, float size);
    void openFileDialog();          // shared between menu, hotkey and home

    // Recent-files modal opened from the home screen.
    bool m_showRecentDialog = false;
    void renderRecentDialog();

    // True the first frame the home screen is visible after either
    // app startup or a transition from playing → idle.  Used to seed
    // the gamepad / keyboard nav focus on the first home button so
    // the d-pad has somewhere to start from.
    bool m_homeFreshlyShown = true;

    // Windows-style auto-hidden main menu bar.  Hidden by default; tap
    // Alt (keyboard) or the Start button (gamepad) to toggle, Escape
    // to dismiss.  Keeps the chrome out of the way during playback and
    // matches the muscle memory most Windows users already have.
    bool m_menuBarVisible = false;

    // Core systems
    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<ImGuiLayer> m_imgui;
    std::unique_ptr<MpvPlayer> m_player;

    // UI panels
    std::unique_ptr<ControlPanel> m_controlPanel;
    std::unique_ptr<TransportBar> m_transportBar;
    std::unique_ptr<TrackPicker>       m_trackPicker;
    std::unique_ptr<PreferencesDialog> m_prefsDialog;

    // Shared state with audio filter
    HrtfSharedState* m_sharedState = nullptr;

    // State
    bool m_running = false;
    float m_lastFrameTime = 0.0f;

    // Video FBO
    unsigned int m_videoFBO = 0;
    unsigned int m_videoTexture = 0;
    unsigned int m_videoDepth = 0;
    int m_videoWidth = 1280;
    int m_videoHeight = 720;

    // 3D Visualizer FBO
    unsigned int m_vizFBO = 0;
    unsigned int m_vizTexture = 0;
    unsigned int m_vizDepth = 0;
    int m_vizWidth = 640;
    int m_vizHeight = 480;

    void createFBOs();
    void resizeFBOs(int videoW, int videoH, int vizW, int vizH);

    // Speaker selection (click in visualizer)
    int m_selectedSpeaker = -1;  // -1 = none
    bool m_draggingSpeaker = false;

    // 3D visualizer panel — hidden by default, toggled via View menu or F2.
    bool m_show3DViz = false;
    bool m_f2KeyHeld = false;
    void toggle3DViz() { m_show3DViz = !m_show3DViz; }

    // Control panel — hidden by default; toggled via View menu, F3, or the
    // gear button on the transport bar.  Without it, the app reads as a
    // straightforward video player.
    bool m_showControlPanel = false;
    bool m_f3KeyHeld = false;
    void toggleControlPanel() { m_showControlPanel = !m_showControlPanel; }

    // Video fullscreen mode
    bool m_videoFullscreen = false;
    bool m_fullscreenKeyHeld = false;
    float m_fullscreenCursorTimer = 0.0f;  // hide UI after inactivity
    float m_lastMouseX = 0.0f, m_lastMouseY = 0.0f;
    bool  m_cursorHidden = false;          // current GLFW cursor state

    void toggleVideoFullscreen();

    // File to open (from command line or drag-drop)
    std::string m_pendingFile;
};
