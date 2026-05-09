#include "Application.h"
#include "core/SharedState.h"
#include "core/Settings.h"
#include "renderer/Picking.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuiFileDialog.h>
#include <IconsLucide.h>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <algorithm>

static bool isSubtitleFile(const std::string& path) {
    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    return ext == ".srt" || ext == ".ass" || ext == ".ssa" ||
           ext == ".sub" || ext == ".vtt" || ext == ".idx";
}

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    void* addr = ep->ExceptionRecord->ExceptionAddress;

    // Find which module (DLL/EXE) the crash address belongs to
    HMODULE hMod = NULL;
    char modName[MAX_PATH] = "unknown";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hMod)) {
        GetModuleFileNameA(hMod, modName, sizeof(modName));
    }

    // Compute offset within the module
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)hMod;

    fprintf(stderr, "[CRASH] Exception 0x%08lX at %p (module: %s +0x%llX)\n",
            ep->ExceptionRecord->ExceptionCode, addr,
            modName, (unsigned long long)offset);
    fprintf(stderr, "[CRASH] RIP=%p RSP=%p\n",
            (void*)ep->ContextRecord->Rip,
            (void*)ep->ContextRecord->Rsp);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

Application::Application() = default;
Application::~Application() = default;

bool Application::init(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif
    fprintf(stderr, "[App] init() starting\n");

    // Parse command line
    for (int i = 1; i < argc; i++) {
        m_pendingFile = argv[i];
    }

    // Create shared state for communication with mpv's af_hrtf filter
    m_sharedState = hrtf_shared_state_create();
    if (!m_sharedState) {
        fprintf(stderr, "Failed to create shared state\n");
        return false;
    }

    // Create window
    m_window = std::make_unique<Window>();
    if (!m_window->init("HRTF Spatial Audio Virtualizer", 1600, 900)) {
        fprintf(stderr, "Failed to create window\n");
        return false;
    }

    // Setup file drop callback
    m_window->setDropCallback([this](const char* path) {
        fprintf(stderr, "[App] File dropped: %s\n", path);
        if (isSubtitleFile(path)) {
            if (m_player && m_player->hasVideo()) {
                m_player->loadSubtitleFile(path);
            }
        } else {
            m_pendingFile = path;
        }
    });

    // Initialize OpenGL
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        return false;
    }

    printf("OpenGL %s, GLSL %s\n",
           glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    // Create renderer and camera
    m_camera = std::make_unique<Camera>();
    m_camera->setPosition(glm::vec3(0.0f, 4.0f, 6.0f));
    m_camera->lookAt(glm::vec3(0.0f, 0.0f, 0.0f));

    m_renderer = std::make_unique<Renderer>();
    if (!m_renderer->init()) {
        fprintf(stderr, "Failed to initialize renderer\n");
        return false;
    }

    // Initialize ImGui
    m_imgui = std::make_unique<ImGuiLayer>();
    m_imgui->init(m_window->getHandle());

    // Create mpv player
    m_player = std::make_unique<MpvPlayer>();
    if (!m_player->init(m_sharedState)) {
        fprintf(stderr, "Warning: mpv player failed to initialize\n");
        // Continue without video - just show visualizer
    }

    // Create UI panels
    m_controlPanel = std::make_unique<ControlPanel>(m_sharedState, &m_selectedSpeaker,
                                                       m_player.get());
    m_transportBar = std::make_unique<TransportBar>(m_player.get());
    m_transportBar->setFullscreenCallback([this]() { toggleVideoFullscreen(); });
    m_transportBar->setControlsCallback([this]() { toggleControlPanel(); });

    m_trackPicker = std::make_unique<TrackPicker>(m_player.get());
    m_transportBar->setAudioPickerCallback([this]() {
        m_trackPicker->open(TrackPicker::Mode::Audio,    /*isAutoLoad=*/false);
    });
    m_transportBar->setSubPickerCallback([this]() {
        m_trackPicker->open(TrackPicker::Mode::Subtitle, /*isAutoLoad=*/false);
    });

    m_prefsDialog = std::make_unique<PreferencesDialog>();

    // Create FBOs for video and 3D visualizer
    createFBOs();

    // Restore persisted settings (sliders, paths, panel visibility) before
    // the first render so the UI reflects the user's last choices.
    Settings::load(m_sharedState, &m_showControlPanel, &m_show3DViz);

    // File-dialog typing.  Same accent colour for every entry so the only
    // visual differentiator between rows is the shape of the line-icon.
    {
        const ImVec4 sand(0.824f, 0.757f, 0.714f, 1.0f);   // #D2C1B6
        const ImVec4 dim (0.824f, 0.757f, 0.714f, 0.78f);
        auto* d = ImGuiFileDialog::Instance();

        // Pre-populate the left-hand "Bookmarks" group with common user
        // folders.  The "Devices" group is filled automatically by the
        // library with every mounted volume (C:, D:, F:, ...).
        if (auto* bm = d->GetPlacesGroupPtr("Bookmarks")) {
            const char* userProfile = std::getenv("USERPROFILE");
            if (userProfile && *userProfile) {
                std::string home = userProfile;
                bm->AddPlace("Home",      home,                 false);
                bm->AddPlace("Desktop",   home + "\\Desktop",   false);
                bm->AddPlace("Videos",    home + "\\Videos",    false);
                bm->AddPlace("Music",     home + "\\Music",     false);
                bm->AddPlace("Downloads", home + "\\Downloads", false);
            }
        }

        d->SetFileStyle(IGFD_FileStyleByTypeDir,  "", sand, ICON_LC_FOLDER);
        d->SetFileStyle(IGFD_FileStyleByTypeLink, "", dim,  ICON_LC_FOLDER);
        // Video
        for (auto* ext : {".mkv", ".mp4", ".avi", ".webm", ".mov",
                           ".m4v", ".ts",  ".m2ts", ".thd"})
            d->SetFileStyle(IGFD_FileStyleByExtention, ext, dim, ICON_LC_FILM);
        // Audio
        for (auto* ext : {".flac", ".wav", ".mp3", ".opus",
                           ".ogg",  ".aac", ".ac3"})
            d->SetFileStyle(IGFD_FileStyleByExtention, ext, dim, ICON_LC_MUSIC);
        // Subtitles
        for (auto* ext : {".srt", ".ass", ".ssa", ".sub", ".vtt", ".idx"})
            d->SetFileStyle(IGFD_FileStyleByExtention, ext, dim, ICON_LC_CAPTIONS);
        // SOFA / EQ profiles
        d->SetFileStyle(IGFD_FileStyleByExtention, ".sofa", dim, ICON_LC_AUDIO_LINES);
        d->SetFileStyle(IGFD_FileStyleByExtention, ".txt",  dim, ICON_LC_FILE_TEXT);
    }

    // Load file if specified
    if (!m_pendingFile.empty()) {
        m_player->loadFile(m_pendingFile);
        m_controlPanel->loadSidecar(m_pendingFile);
        m_pendingFile.clear();
    }

    m_running = true;
    m_lastFrameTime = (float)glfwGetTime();

    return true;
}

void Application::createFBOs() {
    // Video FBO (with depth/stencil for mpv)
    glGenFramebuffers(1, &m_videoFBO);
    glGenTextures(1, &m_videoTexture);
    glGenRenderbuffers(1, &m_videoDepth);

    glBindTexture(GL_TEXTURE_2D, m_videoTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_videoWidth, m_videoHeight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindRenderbuffer(GL_RENDERBUFFER, m_videoDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          m_videoWidth, m_videoHeight);

    glBindFramebuffer(GL_FRAMEBUFFER, m_videoFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_videoTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, m_videoDepth);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[App] Video FBO incomplete: 0x%X\n", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 3D Visualizer FBO
    glGenFramebuffers(1, &m_vizFBO);
    glGenTextures(1, &m_vizTexture);
    glGenRenderbuffers(1, &m_vizDepth);

    glBindTexture(GL_TEXTURE_2D, m_vizTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_vizWidth, m_vizHeight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindRenderbuffer(GL_RENDERBUFFER, m_vizDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          m_vizWidth, m_vizHeight);

    glBindFramebuffer(GL_FRAMEBUFFER, m_vizFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_vizTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, m_vizDepth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Application::resizeFBOs(int videoW, int videoH, int vizW, int vizH) {
    // Only resize if change is significant (>4px) to avoid clearing texture content
    if (abs(videoW - m_videoWidth) > 4 || abs(videoH - m_videoHeight) > 4) {
        fprintf(stderr, "[App] Video FBO resize: %dx%d -> %dx%d\n",
                m_videoWidth, m_videoHeight, videoW, videoH);
        m_videoWidth = videoW;
        m_videoHeight = videoH;
        glBindTexture(GL_TEXTURE_2D, m_videoTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, videoW, videoH,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindRenderbuffer(GL_RENDERBUFFER, m_videoDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, videoW, videoH);
    }
    if (vizW != m_vizWidth || vizH != m_vizHeight) {
        m_vizWidth = vizW;
        m_vizHeight = vizH;
        glBindTexture(GL_TEXTURE_2D, m_vizTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vizW, vizH,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindRenderbuffer(GL_RENDERBUFFER, m_vizDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, vizW, vizH);
    }
}

void Application::run() {
    int frameCount = 0;
    fprintf(stderr, "[App] Entering main loop\n");
    while (m_running && !m_window->shouldClose()) {
        float currentTime = (float)glfwGetTime();
        float dt = currentTime - m_lastFrameTime;
        m_lastFrameTime = currentTime;

        m_window->pollEvents();
        processInput();
        update(dt);
        render();
        renderUI();
        m_window->swapBuffers();

        frameCount++;
    }
    fprintf(stderr, "[App] Exiting main loop after %d frames\n", frameCount);
}

void Application::toggleVideoFullscreen() {
    m_videoFullscreen = !m_videoFullscreen;
    m_window->toggleFullscreen();
    m_fullscreenCursorTimer = 3.0f;
}

void Application::processInput() {
    // Handle pending file drops
    if (!m_pendingFile.empty() && m_player) {
        m_player->loadFile(m_pendingFile);
        m_controlPanel->loadSidecar(m_pendingFile);
        m_pendingFile.clear();
    }

    GLFWwindow* handle = m_window->getHandle();

    // F11: toggle video fullscreen
    if (glfwGetKey(handle, GLFW_KEY_F11) == GLFW_PRESS) {
        if (!m_fullscreenKeyHeld) {
            toggleVideoFullscreen();
            m_fullscreenKeyHeld = true;
        }
    } else {
        m_fullscreenKeyHeld = false;
    }

    // Escape: exit video fullscreen
    if (m_videoFullscreen && glfwGetKey(handle, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        toggleVideoFullscreen();
    }

    // F2: toggle 3D visualizer panel
    if (glfwGetKey(handle, GLFW_KEY_F2) == GLFW_PRESS) {
        if (!m_f2KeyHeld) {
            toggle3DViz();
            m_f2KeyHeld = true;
        }
    } else {
        m_f2KeyHeld = false;
    }

    // F3: toggle Control Panel drawer
    if (glfwGetKey(handle, GLFW_KEY_F3) == GLFW_PRESS) {
        if (!m_f3KeyHeld) {
            toggleControlPanel();
            m_f3KeyHeld = true;
        }
    } else {
        m_f3KeyHeld = false;
    }
}

void Application::update(float dt) {
    if (m_player)
        m_player->update();

    m_transportBar->setFullscreenState(m_videoFullscreen);

    // Sync spatial object positions from sidecar to shared state
    if (m_controlPanel)
        m_controlPanel->updateObjectPositions();

    // Open the audio-track picker the first frame after a fresh load.
    // mpv has been kept paused so we can take our time before the first
    // audible frame.
    if (m_player && m_trackPicker &&
        m_player->consumeFreshFileLoaded() &&
        !m_player->getAudioTracks().empty()) {
        // Apply language preferences: pick the first track whose lang
        // matches Settings::preferredAudio/SubLang.  The user can still
        // override from the picker that opens right after.
        const std::string& prefAud = Settings::preferredAudioLang();
        if (!prefAud.empty()) {
            for (const auto& t : m_player->getAudioTracks())
                if (Settings::langMatches(t.lang, prefAud)) {
                    m_player->setAudioTrack(t.id);
                    break;
                }
        }
        const std::string& prefSub = Settings::preferredSubLang();
        if (!prefSub.empty()) {
            for (const auto& s : m_player->getSubtitleTracks())
                if (Settings::langMatches(s.lang, prefSub)) {
                    m_player->setSubtitleTrack(s.id);
                    break;
                }
        }
        m_trackPicker->open(TrackPicker::Mode::Audio, /*isAutoLoad=*/true);
    }
}

void Application::render() {
    // Video is rendered in renderUI() after FBO resize to avoid content erasure

    // Render 3D visualizer to FBO only when its panel is visible — saves a
    // pass over every speaker/object every frame when the user has it hidden.
    if (!m_show3DViz)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, m_vizFBO);
    glViewport(0, 0, m_vizWidth, m_vizHeight);
    glClearColor(0.12f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    m_renderer->render(*m_camera, m_sharedState,
                       (float)m_vizWidth / (float)m_vizHeight, m_selectedSpeaker);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Application::renderUI() {
    m_imgui->beginFrame();

    // Track mouse movement for auto-hiding transport in fullscreen
    ImVec2 mousePos = ImGui::GetMousePos();
    if (m_videoFullscreen) {
        if (fabsf(mousePos.x - m_lastMouseX) > 1.0f ||
            fabsf(mousePos.y - m_lastMouseY) > 1.0f) {
            m_fullscreenCursorTimer = 3.0f;
        }
        float dt = ImGui::GetIO().DeltaTime;
        m_fullscreenCursorTimer -= dt;
        if (m_fullscreenCursorTimer < 0) m_fullscreenCursorTimer = 0;
    }
    m_lastMouseX = mousePos.x;
    m_lastMouseY = mousePos.y;

    // --- Video fullscreen mode ---
    if (m_videoFullscreen) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 vpSize = vp->Size;

#ifdef HAVE_MPV
        if (m_player && m_player->hasVideo()) {
            int newW = (int)vpSize.x;
            int newH = (int)vpSize.y;
            resizeFBOs(newW, newH, m_vizWidth, m_vizHeight);
            m_player->renderToFBO(m_videoFBO, m_videoWidth, m_videoHeight);

            // Draw video filling the entire viewport
            ImGui::SetNextWindowPos(vp->Pos);
            ImGui::SetNextWindowSize(vpSize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::Begin("##fullscreen_video", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking);
            ImGui::Image((ImTextureID)(intptr_t)m_videoTexture, vpSize,
                         ImVec2(0, 1), ImVec2(1, 0));

            // Double-click to exit fullscreen
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                toggleVideoFullscreen();
            }
            ImGui::End();
            ImGui::PopStyleVar(2);

            // Show transport bar overlay at bottom when mouse is active.
            // The new transport bar has three rows (header / timeline /
            // controls) and varies with theme padding, so we anchor the
            // window's bottom-left to the viewport's bottom-left
            // (pivot 0,1) and let AlwaysAutoResize compute the height.
            // Width is pinned to the viewport via size constraints.
            if (m_fullscreenCursorTimer > 0) {
                float fadeAlpha = m_fullscreenCursorTimer < 1.0f ? m_fullscreenCursorTimer : 1.0f;

                ImGui::SetNextWindowPos(
                    ImVec2(vp->Pos.x, vp->Pos.y + vpSize.y),
                    ImGuiCond_Always, ImVec2(0.0f, 1.0f));
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(vpSize.x, 0.0f),
                    ImVec2(vpSize.x, vpSize.y));
                ImGui::SetNextWindowBgAlpha(0.7f * fadeAlpha);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fadeAlpha);
                ImGui::Begin("##fullscreen_transport", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_AlwaysAutoResize);
                m_transportBar->renderContent();
                ImGui::End();
                ImGui::PopStyleVar();
            }
        }
#endif
        m_imgui->endFrame();
        return;
    }

    // --- Normal docked mode ---

    // Top menu bar — exposes panels and the persisted-config controls.
    bool dirty = Settings::isDirty(m_sharedState,
                                    m_showControlPanel, m_show3DViz);
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(ICON_LC_FILE "  File")) {
            if (ImGui::MenuItem(ICON_LC_FOLDER_OPEN "  Open...", "Ctrl+O")) {
                IGFD::FileDialogConfig cfg;
                cfg.path = ".";
                cfg.flags = ImGuiFileDialogFlags_Modal;
                ImGuiFileDialog::Instance()->OpenDialog(
                    "open_media",
                    "Open media file",
                    "Video/Audio{.mkv,.mp4,.avi,.webm,.mov,.m4v,"
                                ".ts,.m2ts,.flac,.wav,.mp3,.opus,.ogg,.aac,.ac3,.thd},"
                    "All files{.*}",
                    cfg);
            }
            if (ImGui::MenuItem(ICON_LC_CAPTIONS "  Load Subtitle...",
                                 nullptr, false,
                                 m_player && m_player->hasVideo())) {
                IGFD::FileDialogConfig cfg;
                cfg.path = ".";
                cfg.flags = ImGuiFileDialogFlags_Modal;
                ImGuiFileDialog::Instance()->OpenDialog(
                    "open_subtitle",
                    "Load subtitle",
                    "Subtitles{.srt,.ass,.ssa,.sub,.vtt,.idx},All files{.*}",
                    cfg);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_LC_DOOR_OPEN "  Exit", "Alt+F4")) {
                m_running = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_LC_EYE "  View")) {
            ImGui::MenuItem(ICON_LC_SLIDERS  "  Controls",      "F3",
                             &m_showControlPanel);
            ImGui::MenuItem(ICON_LC_FILM     "  3D Visualizer", "F2",
                             &m_show3DViz);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_LC_SETTINGS "  Settings")) {
            if (ImGui::MenuItem(ICON_LC_SLIDERS "  Preferences...",
                                 "Ctrl+,")) {
                if (m_prefsDialog) m_prefsDialog->open();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_LC_SAVE "  Save Now",
                                 "Ctrl+S", false, dirty)) {
                Settings::save(m_sharedState,
                                m_showControlPanel, m_show3DViz);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_LC_ROTATE_CCW "  Reset to Defaults")) {
                Settings::resetToDefaults(m_sharedState,
                                           &m_showControlPanel, &m_show3DViz);
            }
            ImGui::Separator();
            ImGui::TextDisabled("File:");
            ImGui::TextDisabled("%s", Settings::filePath());
            ImGui::EndMenu();
        }
        // Right-aligned status: shown only while there are unsaved changes.
        if (dirty) {
            const char* tag = "[unsaved]";
            float w = ImGui::CalcTextSize(tag).x + 16.0f;
            ImGui::SameLine(ImGui::GetWindowWidth() - w);
            ImGui::TextDisabled("%s", tag);
        }
        ImGui::EndMainMenuBar();
    }

    // Ctrl+S: save now (only if dirty).
    {
        ImGuiIO& io = ImGui::GetIO();
        if (dirty && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            Settings::save(m_sharedState, m_showControlPanel, m_show3DViz);
        }
        // Ctrl+, : open preferences dialog (mirrors Settings > Preferences).
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
            if (m_prefsDialog) m_prefsDialog->open();
        }
        // Ctrl+O: open file dialog (mirrors File > Open).
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            IGFD::FileDialogConfig cfg;
            cfg.path = ".";
            cfg.flags = ImGuiFileDialogFlags_Modal;
            ImGuiFileDialog::Instance()->OpenDialog(
                "open_media",
                "Open media file",
                "Video/Audio{.mkv,.mp4,.avi,.webm,.mov,.m4v,"
                            ".ts,.m2ts,.flac,.wav,.mp3,.opus,.ogg,.aac,.ac3,.thd},"
                "All files{.*}",
                cfg);
        }
    }

    // Render and dispatch the file dialogs.  Sized so they're roomy enough
    // to navigate but not full-screen.
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    ImVec2 dlgMin(640, 420);
    ImVec2 dlgMax(vpSize.x * 0.9f, vpSize.y * 0.9f);
    if (ImGuiFileDialog::Instance()->Display("open_media", 0, dlgMin, dlgMax)) {
        if (ImGuiFileDialog::Instance()->IsOk() && m_player) {
            std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
            m_pendingFile = path;
        }
        ImGuiFileDialog::Instance()->Close();
    }
    if (ImGuiFileDialog::Instance()->Display("open_subtitle", 0, dlgMin, dlgMax)) {
        if (ImGuiFileDialog::Instance()->IsOk() && m_player) {
            std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
            m_player->loadSubtitleFile(path);
        }
        ImGuiFileDialog::Instance()->Close();
    }

    // Enable docking
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // Rebuild the dock layout whenever the set of opt-in side panels changes.
    // The right column only exists while Controls and/or 3D Viz are open, so
    // the video grows to the full width when both are hidden.
    static bool layoutBuilt = false;
    static bool prevShowCtrl = false;
    static bool prevShowViz  = false;
    bool wantRight = m_showControlPanel || m_show3DViz;
    if (!layoutBuilt ||
        prevShowCtrl != m_showControlPanel ||
        prevShowViz  != m_show3DViz) {

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        // The "left column" hosts video + transport.  When no side panels are
        // open it occupies the full dockspace, so we only split off the right
        // 25% drawer when at least one panel is visible.
        ImGuiID leftCol = dockspace_id;
        ImGuiID rightCol = 0;
        if (wantRight) {
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left,
                                         0.75f, &leftCol, &rightCol);
        }

        // Left: video on top, transport docked below.  The transport now
        // hosts three rows of content (header, full-width timeline,
        // controls), so a fixed pixel height is more honest than a
        // ratio — at 8% on a 900 px viewport it overflowed and forced
        // the whole panel into a scrollbar.  Convert the desired height
        // to a ratio of the current dock space and clamp so we never
        // squeeze the video on a tiny viewport.
        const float transportPx = 150.0f;   // ≈ 3 rows + tab bar + padding
        const float dockPx      = ImGui::GetMainViewport()->Size.y -
                                   ImGui::GetFrameHeight();   // sub menu bar
        float transportRatio = transportPx / dockPx;
        if (transportRatio < 0.08f) transportRatio = 0.08f;
        if (transportRatio > 0.45f) transportRatio = 0.45f;

        ImGuiID videoNode, transportNode;
        ImGui::DockBuilderSplitNode(leftCol, ImGuiDir_Down,
                                     transportRatio,
                                     &transportNode, &videoNode);
        ImGui::DockBuilderDockWindow("Video", videoNode);
        ImGui::DockBuilderDockWindow("Transport", transportNode);

        // Right column: 50/50 split when both side panels open, otherwise
        // the single visible panel takes the full drawer width.
        if (m_showControlPanel && m_show3DViz) {
            ImGuiID vizNode, ctrlNode;
            ImGui::DockBuilderSplitNode(rightCol, ImGuiDir_Up, 0.50f,
                                         &vizNode, &ctrlNode);
            ImGui::DockBuilderDockWindow("HRTF Visualizer", vizNode);
            ImGui::DockBuilderDockWindow("Control Panel",   ctrlNode);
        } else if (m_show3DViz) {
            ImGui::DockBuilderDockWindow("HRTF Visualizer", rightCol);
        } else if (m_showControlPanel) {
            ImGui::DockBuilderDockWindow("Control Panel",   rightCol);
        }

        ImGui::DockBuilderFinish(dockspace_id);
        layoutBuilt = true;
        prevShowCtrl = m_showControlPanel;
        prevShowViz  = m_show3DViz;
    }

    // Video panel
    ImGui::Begin("Video");
    ImVec2 videoSize = ImGui::GetContentRegionAvail();

    if (videoSize.x > 0 && videoSize.y > 0) {
        int newW = (int)videoSize.x;
        int newH = (int)videoSize.y;
        resizeFBOs(newW, newH, m_vizWidth, m_vizHeight);

#ifdef HAVE_MPV
        if (m_player && m_player->hasVideo()) {
            m_player->renderToFBO(m_videoFBO, m_videoWidth, m_videoHeight);
            ImGui::Image((ImTextureID)(intptr_t)m_videoTexture, videoSize,
                         ImVec2(0, 1), ImVec2(1, 0));
            // Double-click on video: enter fullscreen
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                toggleVideoFullscreen();
            }
        } else {
            ImGui::TextWrapped("Drag and drop a video file to start playback.");
            ImGui::TextWrapped("Supports: MKV, MP4, AVI, WEBM, and more.");
        }
#else
        ImGui::TextWrapped("libmpv not available. Build with HAVE_MPV=1.");
#endif
    }
    ImGui::End();

    // 3D Visualizer panel — opt-in via View menu / F2.  When hidden we skip
    // the whole Begin/End so ImGui collapses the slot and the control panel
    // expands to fill the right side of the dockspace.
    if (m_show3DViz) {
    ImGui::Begin("HRTF Visualizer", &m_show3DViz);
    ImVec2 vizSize = ImGui::GetContentRegionAvail();
    if (vizSize.x > 0 && vizSize.y > 0) {
        int newW = (int)vizSize.x;
        int newH = (int)vizSize.y;
        resizeFBOs(m_videoWidth, m_videoHeight, newW, newH);

        ImGui::Image((ImTextureID)(intptr_t)m_vizTexture, vizSize,
                     ImVec2(0, 1), ImVec2(1, 0));

        // Handle mouse interaction in the visualizer
        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();

            // Right-click drag: orbit camera
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
                m_camera->orbit(delta.x * 0.5f, delta.y * 0.5f);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
            }

            // Scroll: zoom
            if (io.MouseWheel != 0.0f) {
                m_camera->zoom(io.MouseWheel * -0.5f);
            }

            // Middle-click drag: pan
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
                m_camera->pan(delta.x * 0.01f, delta.y * 0.01f);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            }

            // Left-click: pick speaker / drag to move
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // Convert mouse pos to NDC relative to the viz image
                ImVec2 imageMin = ImGui::GetItemRectMin();
                ImVec2 imageMax = ImGui::GetItemRectMax();
                ImVec2 mousePos = ImGui::GetMousePos();

                float ndcX = ((mousePos.x - imageMin.x) / (imageMax.x - imageMin.x)) * 2.0f - 1.0f;
                float ndcY = 1.0f - ((mousePos.y - imageMin.y) / (imageMax.y - imageMin.y)) * 2.0f;

                float vizAspect = vizSize.x / vizSize.y;
                glm::vec3 rayOrigin, rayDir;
                m_camera->screenToRay(ndcX, ndcY, vizAspect, rayOrigin, rayDir);

                // Build world positions array for picking
                int numCh = m_sharedState ? atomic_load(&m_sharedState->num_channels) : 0;
                glm::vec3 positions[HRTF_MAX_CHANNELS];
                for (int i = 0; i < numCh && i < HRTF_MAX_CHANNELS; i++) {
                    HrtfPosition sp = m_sharedState->speaker_pos[i];
                    positions[i] = Renderer::speakerToWorldPos(sp.azimuth, sp.elevation, sp.distance);
                }

                PickResult pick = pickSpeaker(rayOrigin, rayDir, positions, numCh, 0.3f);
                if (pick.hit) {
                    m_selectedSpeaker = pick.speakerIndex;
                    m_draggingSpeaker = true;
                } else {
                    m_selectedSpeaker = -1;
                    m_draggingSpeaker = false;
                }
            }

            // Left drag: move selected speaker (azimuth/elevation)
            if (m_draggingSpeaker && m_selectedSpeaker >= 0 &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                if (m_sharedState) {
                    m_sharedState->speaker_pos[m_selectedSpeaker].azimuth -= delta.x * 0.3f;
                    m_sharedState->speaker_pos[m_selectedSpeaker].elevation += delta.y * 0.3f;

                    // Clamp elevation
                    float& el = m_sharedState->speaker_pos[m_selectedSpeaker].elevation;
                    if (el > 90.0f) el = 90.0f;
                    if (el < -90.0f) el = -90.0f;

                    atomic_store(&m_sharedState->speaker_pos_changed, 1);
                }
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                m_draggingSpeaker = false;
            }

            // Double-click: play test tone on the picked speaker
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ImVec2 imageMin = ImGui::GetItemRectMin();
                ImVec2 imageMax = ImGui::GetItemRectMax();
                ImVec2 mousePos = ImGui::GetMousePos();

                float ndcX = ((mousePos.x - imageMin.x) / (imageMax.x - imageMin.x)) * 2.0f - 1.0f;
                float ndcY = 1.0f - ((mousePos.y - imageMin.y) / (imageMax.y - imageMin.y)) * 2.0f;

                float vizAspect = vizSize.x / vizSize.y;
                glm::vec3 rayOrigin, rayDir;
                m_camera->screenToRay(ndcX, ndcY, vizAspect, rayOrigin, rayDir);

                int numCh = m_sharedState ? atomic_load(&m_sharedState->num_channels) : 0;
                glm::vec3 positions[HRTF_MAX_CHANNELS];
                for (int i = 0; i < numCh && i < HRTF_MAX_CHANNELS; i++) {
                    HrtfPosition sp = m_sharedState->speaker_pos[i];
                    positions[i] = Renderer::speakerToWorldPos(sp.azimuth, sp.elevation, sp.distance);
                }

                PickResult pick = pickSpeaker(rayOrigin, rayDir, positions, numCh, 0.3f);
                if (pick.hit && m_player) {
                    m_player->playTestTone(pick.speakerIndex);
                }
            }
        }
    }
    ImGui::End();
    } // if (m_show3DViz)

    // Control panel — opt-in drawer; F3 / View menu / gear button on the
    // transport bar.  When hidden, the right-side dock node collapses and
    // the video reclaims the freed width.
    if (m_showControlPanel) {
        m_controlPanel->render(&m_showControlPanel);
    }

    // Transport bar
    m_transportBar->render();

    // Track picker modal — fires after a fresh load while playback is
    // still paused, so the user picks the audio stream up-front.
    if (m_trackPicker)
        m_trackPicker->render();

    // Preferences modal (Settings > Preferences / Ctrl+,)
    if (m_prefsDialog)
        m_prefsDialog->render();

    m_imgui->endFrame();
}

void Application::shutdown() {
    // Persist settings on clean exit if the user has touched anything.
    if (m_sharedState &&
        Settings::isDirty(m_sharedState, m_showControlPanel, m_show3DViz)) {
        Settings::save(m_sharedState, m_showControlPanel, m_show3DViz);
    }

    if (m_videoFBO) glDeleteFramebuffers(1, &m_videoFBO);
    if (m_videoTexture) glDeleteTextures(1, &m_videoTexture);
    if (m_videoDepth) glDeleteRenderbuffers(1, &m_videoDepth);
    if (m_vizFBO) glDeleteFramebuffers(1, &m_vizFBO);
    if (m_vizTexture) glDeleteTextures(1, &m_vizTexture);
    if (m_vizDepth) glDeleteRenderbuffers(1, &m_vizDepth);

    m_prefsDialog.reset();
    m_trackPicker.reset();
    m_transportBar.reset();
    m_controlPanel.reset();
    m_player.reset();
    m_imgui.reset();
    m_renderer.reset();
    m_camera.reset();
    m_window.reset();

    if (m_sharedState)
        hrtf_shared_state_destroy(m_sharedState);
}
