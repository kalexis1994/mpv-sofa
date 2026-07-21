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
#include "audio/BinauralRenderer.h"
#include "core/MediaServer.h"

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

    // In-playback overlay menu — same four big tiles as the home
    // landing, but composited on top of the (paused) video so the
    // user can pop into Open / Recent / Settings / Exit without
    // tearing themselves out of the cinema view.  Triggered by Start
    // or B on the gamepad while a file is loaded.  m_pausedByMenu
    // tracks whether *we* paused playback to open the menu — if the
    // user already had it paused, we don't auto-resume on close.
    bool m_playbackMenuOpen          = false;
    bool m_playbackMenuPausedUs      = false;
    bool m_playbackMenuFreshlyShown  = true;
    void openPlaybackMenu();
    void closePlaybackMenu();
    void renderPlaybackMenu();
    // Modal layers shared by every UI mode (docked / home / fullscreen):
    // file dialogs, playback menu, track picker, preferences, recents.
    void renderModalLayers();

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
    BinauralRenderer m_binaural;
    std::string m_binauralMovie;   // path the sidecar render belongs to
    bool m_binauralArmed = false;  // user asked for object rendering
    // Detect the current file's TrueHD Atmos track (ff-index) or -1.
    int atmosTrackIndex() const;
    void updateBinaural();         // drive render + audio swap each frame
    void requestBinaural();        // user pressed "Render Atmos objects"
    void renderAtmosObjectsUi();   // Spatial-tab control (ImGui)

    // UI panels
    std::unique_ptr<ControlPanel> m_controlPanel;
    std::unique_ptr<TransportBar> m_transportBar;
    std::unique_ptr<TrackPicker>       m_trackPicker;
    std::unique_ptr<PreferencesDialog> m_prefsDialog;
    std::unique_ptr<MediaServer>       m_mediaServer;

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

    // Vestigial: Settings::load/save still take a showControls flag for
    // forward-compatible .ini layout.  The field stays here so those
    // signatures don't have to change all at once, but the legacy
    // dockable Control Panel is gone — its body migrated to the
    // Preferences page (Spatial / Headphone EQ tabs).
    bool m_showControlPanel = false;

    // Left-stick edge tracking — used by processInput()'s LStick →
    // arrow-key forwarder so the analog stick can drive ImGui nav the
    // same way the d-pad does.  Edge-detection avoids spamming events
    // every frame; ImGui's own repeat timing handles the held case.
    bool m_lstickLeft  = false;
    bool m_lstickRight = false;
    bool m_lstickUp    = false;
    bool m_lstickDown  = false;

    // File-dialog gamepad shortcuts.  When ImGuiFileDialog is open we
    // forward Y → Enter (confirm / open folder) and X → Backspace
    // (go up to parent dir).  Edge-tracked so the events queue cleanly
    // and don't get re-fired every frame the button is held.
    bool m_dialogYHeld = false;
    bool m_dialogXHeld = false;

    // Video fullscreen mode
    bool m_videoFullscreen = false;
    bool m_fullscreenKeyHeld = false;
    bool m_escKeyHeld = false;
    float m_fullscreenCursorTimer = 0.0f;  // hide UI after inactivity
    float m_lastMouseX = 0.0f, m_lastMouseY = 0.0f;
    bool  m_cursorHidden = false;          // current GLFW cursor state

    void toggleVideoFullscreen();

    // Auto-hide transport bar during docked playback.  Same idea as
    // the fullscreen cursor timer but applied to the bottom dock node:
    // after kTransportHideDelay seconds without input, the transport
    // window is dropped from the layout and the video reclaims its
    // height so the user sees the frame uninterrupted.  Any mouse
    // motion / key / gamepad input resets the timer.
    static constexpr float kTransportHideDelay = 3.0f;
    float m_transportIdleTimer = 0.0f;
    bool  m_transportVisible   = true;

    // File to open (from command line or drag-drop)
    std::string m_pendingFile;
};
