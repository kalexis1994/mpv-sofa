#include "Application.h"
#include "core/SharedState.h"
#include "core/Settings.h"
#include "core/ResumeStore.h"
#include "core/WatchHistoryStore.h"
#include "core/BookmarkStore.h"
#include "platform/Platform.h"
#include "renderer/Picking.h"
#include "ui/ImageTexture.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_opengl3.h>
#include <ImGuiFileDialog.h>
#include <IconsLucide.h>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <ctime>

// Note: gamepad input is fed into ImGui by imgui_impl_glfw's built-in
// ImGui_ImplGlfw_UpdateGamepads(), which is invoked from NewFrame.  An
// earlier manual polling helper that ran here pre-NewFrame raced with
// the backend on the same ImGuiKey_Gamepad* keys and broke directional
// nav for some controllers (A/B activated buttons but d-pad / stick did
// not move focus).  Removing it lets the backend be the single source.

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

static std::string resumeTimeLabel(double seconds);

// Draw-list callback used immediately before an mpv video image. The texture
// is already linear scRGB, unlike normal ImGui colors/textures which are sRGB
// and use the backend's per-frame UI conversion.
static void setHdrVideoColorMode(const ImDrawList*, const ImDrawCmd* cmd) {
    const float scale = cmd->UserCallbackData
        ? *static_cast<const float*>(cmd->UserCallbackData) : 1.0f;
    ImGui_ImplOpenGL3_SetHdrColorMode(2, scale);
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
#else
#include <dlfcn.h>

// The point of this handler is the same as the Win32 one above: say *which*
// library the fault address belongs to.  A raw address is useless once mpv,
// FFmpeg and the driver stack are all loaded; the module plus offset is what
// makes a stack address resolvable with addr2line afterwards.  dladdr() is
// the direct analogue of GetModuleHandleEx-from-address.
//
// Nothing here is async-signal-safe, which is the same trade the Windows side
// makes: the process is already dying, and a readable line on stderr is worth
// more than a guaranteed-clean abort.
static void crashHandler(int sig, siginfo_t* info, void* /*ctx*/) {
    void* addr = info ? info->si_addr : nullptr;

    const char* name = "unknown";
    uintptr_t   off  = 0;
    Dl_info     dli;
    if (addr && dladdr(addr, &dli) && dli.dli_fname) {
        name = dli.dli_fname;
        off  = (uintptr_t)addr - (uintptr_t)dli.dli_fbase;
    }

    fprintf(stderr, "[CRASH] signal %d (%s) at %p (module: %s +0x%llx)\n",
            sig, strsignal(sig), addr, name, (unsigned long long)off);
    fflush(stderr);

    // Restore the default disposition and re-raise so the shell still sees a
    // signal death and any core dump is written as usual.
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashHandler() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashHandler;
    sa.sa_flags     = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
        sigaction(s, &sa, nullptr);
}
#endif

Application::Application() = default;
Application::~Application() = default;

bool Application::init(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#else
    installCrashHandler();
#endif
    fprintf(stderr, "[App] init() starting\n");
    ResumeStore::init();
    WatchHistoryStore::init();

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

    // Restore persisted settings before the window is created so the
    // saved window-mode (Fullscreen / Borderless / Windowed) applies
    // on first show — otherwise the app would always flash through a
    // 1600×900 windowed frame on its way to the user's preferred mode.
    // The HRTF / subtitle / display knobs all need a player or window
    // to apply against, so the apply*ToPlayer calls stay later in
    // init; only the load itself moves up.
    Settings::load(m_sharedState, &m_showControlPanel, &m_show3DViz);

    auto initialMode = static_cast<WindowMode>(Settings::windowMode());

    // Create window
    m_window = std::make_unique<Window>();
    if (!m_window->init("MPV Sofa", 1600, 900,
                         initialMode)) {
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

    // HDR presentation (D3D11 interop on Windows, Vulkan on Linux) — optional
    // and fail-safe. Only takes effect when the user enables it and the
    // display reports HDR; otherwise the plain SDR swap path is used.
    {
        m_hdrPresenter = createHdrPresenter();
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(m_window->getHandle(), &fbw, &fbh);
        if (!m_hdrPresenter->init(m_window->getHandle(), fbw, fbh))
            fprintf(stderr, "[HDR] presenter unavailable — SDR present path\n");
    }

    // Create mpv player
    m_player = std::make_unique<MpvPlayer>();
    if (!m_player->init(m_sharedState)) {
        fprintf(stderr, "Warning: mpv player failed to initialize\n");
        // Continue without video - just show visualizer
    }

    // Create UI panels
    m_controlPanel = std::make_unique<ControlPanel>(m_sharedState, &m_selectedSpeaker,
                                                       m_player.get());
    // Atmos object-render control at the top of the Spatial tab.
    m_controlPanel->setSpatialExtraUi([this]() { renderAtmosObjectsUi(); });
    m_transportBar = std::make_unique<TransportBar>(m_player.get());
    m_transportBar->setFullscreenCallback([this]() { toggleVideoFullscreen(); });

    m_trackPicker = std::make_unique<TrackPicker>(m_player.get());
    m_transportBar->setAudioPickerCallback([this]() {
        m_trackPicker->open(TrackPicker::Mode::Audio,    /*isAutoLoad=*/false);
    });
    m_transportBar->setMenuCallback([this]() { openPlaybackMenu(); });
    // The sliders button had no callback registered at all, so it did nothing
    // and its tooltip advertised an F3 shortcut that no longer exists.  The
    // Controls drawer it used to open now lives as the Spatial tab, so point
    // it there rather than dropping the button.
    m_transportBar->setControlsCallback([this]() {
        if (m_prefsDialog) m_prefsDialog->open();
    });
    m_transportBar->setSubPickerCallback([this]() {
        m_trackPicker->open(TrackPicker::Mode::Subtitle, /*isAutoLoad=*/false);
    });

    m_mediaServer = std::make_unique<MediaServer>();

    m_prefsDialog = std::make_unique<PreferencesDialog>(m_player.get(),
                                                          m_controlPanel.get(),
                                                          m_window.get(),
                                                          m_mediaServer.get());
    m_prefsDialog->setHdrStatusProvider([this]() {
        PreferencesDialog::HdrStatus s;
        s.available  = m_hdrPresenter->available();
        s.displayHdr = m_hdrPresenter->displayIsHdr();
        s.maxNits    = m_hdrPresenter->displayMaxNits();
        s.sdrWhiteNits = m_hdrPresenter->sdrWhiteNits();
        return s;
    });

    // Auto-start the HaloSound media server when the user enabled it and
    // a library folder is configured.  Failure is non-fatal — status and
    // error live in Preferences > Media Server.
    {
        const Settings::ServerConfig& sc = Settings::serverConfig();
        if (sc.enabled && !sc.mediaDir.empty())
            m_mediaServer->start(sc.mediaDir, sc.port);
    }

    // Create FBOs for video and 3D visualizer
    createFBOs();

    m_logoTex = loadTextureFromFile((plat::assetsDir() + "icons/mpv-sofa-logo.png").c_str(),
                                    &m_logoW, &m_logoH);

    // Settings::load already ran before window creation; here we just
    // push the persisted preferences that need a live player into mpv
    // so the first file played already inherits the user's choices.
    Settings::applySubtitleStyleToPlayer(m_player.get());

    // Same for the 35mm projection-grain user shader.
    Settings::applyCinemaGrainToPlayer(m_player.get());

    // And for the HDR / display target pipeline.
    Settings::applyDisplayConfigToPlayer(m_player.get());

    // Audio sync (BT lip-sync offset etc.).
    Settings::applyPlaybackConfigToPlayer(m_player.get());

    // File-dialog typing.  Same accent colour for every entry so the only
    // visual differentiator between rows is the shape of the line-icon.
    {
        const ImVec4 sand(0.824f, 0.757f, 0.714f, 1.0f);   // #D2C1B6
        const ImVec4 dim (0.824f, 0.757f, 0.714f, 0.78f);
        auto* d = ImGuiFileDialog::Instance();

        // Places groups used to be created lazily by OpenDialog().  That
        // meant DeserializePlaces() below had no target on application
        // startup and silently ignored every saved bookmark.  Create the
        // editable group first; the library's lazy initialization is now
        // idempotent and will preserve it on every later opening.
        if (d->GetPlacesGroupPtr("Bookmarks") == nullptr)
            d->AddPlacesGroup("Bookmarks", 0, true, true);

        // Pre-populate the left-hand "Bookmarks" group with common user
        // folders.  The "Devices" group is filled automatically by the
        // library with every mounted volume (C:, D:, F:, ... on Windows,
        // mount points elsewhere).
        //
        // Only offer a folder that actually exists: on Linux these are
        // localised and optional (XDG user dirs), so a hardcoded ~/Videos is
        // as likely to be missing as present, and a bookmark that leads
        // nowhere is worse than no bookmark.
        if (auto* bm = d->GetPlacesGroupPtr("Bookmarks")) {
#ifdef _WIN32
            const char* homeEnv = std::getenv("USERPROFILE");
            const char  sep     = '\\';
#else
            const char* homeEnv = std::getenv("HOME");
            const char  sep     = '/';
#endif
            if (homeEnv && *homeEnv) {
                const std::string home = homeEnv;
                bm->AddPlace("Home", home, false);
                for (const char* sub : {"Desktop", "Videos", "Music", "Downloads"}) {
                    const std::string path = home + sep + sub;
                    if (plat::exists(path))
                        bm->AddPlace(sub, path, false);
                }
            }
        }

        // ImGuiFileDialog deliberately leaves persistence to its host. Load
        // user-created Places after the non-serializable built-ins so both
        // sets coexist without duplicating Home/Videos on every launch.
        std::string savedBookmarks;
        if (BookmarkStore::load(&savedBookmarks))
            d->DeserializePlaces(savedBookmarks);
        m_fileDialogBookmarksSnapshot = d->SerializePlaces();

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
        m_currentMediaPath = m_pendingFile;
        m_player->loadFile(m_pendingFile);
        m_controlPanel->loadSidecar(m_pendingFile);
        Settings::pushRecent(m_pendingFile);
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

        // HDR present path: automatic. Whenever Windows HDR is active on
        // the display, composite the final frame into the D3D11 interop
        // framebuffer and present through DXGI; otherwise the default
        // glfwSwapBuffers SDR path. Re-checks the display's HDR state about
        // once a second so toggling Windows HDR takes effect live. The
        // Preferences "Force SDR output" override (hdrOutput==1) opts out.
        if ((++m_hdrPollCounter % 60) == 0)
            m_hdrPresenter->refreshDisplayHdr();
        const bool forceSdr = Settings::displayConfig().hdrOutput == 1;
        const bool hdrWanted = !forceSdr && m_hdrPresenter->available() &&
                               m_hdrPresenter->displayIsHdr();
        m_finalFbo = 0;
        if (hdrWanted) {
            int fbw = 0, fbh = 0;
            glfwGetFramebufferSize(m_window->getHandle(), &fbw, &fbh);
            m_hdrPresenter->resize(fbw, fbh);
            m_finalFbo = m_hdrPresenter->beginFrame();
        }
        m_hdrActive = m_finalFbo != 0;

        // Normal ImGui data is authored in sRGB. In HDR mode the backend
        // linearizes it and maps white to Windows' per-monitor SDR white
        // level. mpv video is switched to its native scRGB target below and
        // is marked separately with a draw callback around ImGui::Image.
        const float uiScale = m_hdrActive
            ? m_hdrPresenter->sdrWhiteNits() / 80.0f
            : 1.0f;
        ImGui_ImplOpenGL3_SetHdrColorDefaults(m_hdrActive ? 1 : 0, uiScale);

        // Reconfigure mpv only when the effective target changes. Preferences
        // can apply their normal SDR/PQ settings directly; including every
        // relevant setting in this key makes the scRGB override win again on
        // the following frame while HDR presentation remains active.
        const Settings::DisplayConfig& dc = Settings::displayConfig();
        const float targetPeak = m_hdrPresenter->displayMaxNits() > 0.0f
            ? m_hdrPresenter->displayMaxNits() : dc.peakNits;
        const float referenceWhite = std::clamp(dc.hdrRefWhite, 80.0f, 400.0f);
        // The classic libmpv render backend normalizes linear output so 1.0
        // equals target-peak. Windows scRGB instead defines 1.0 as 80 nits.
        // Multiplying by displayPeak/80 restores absolute luminance. Adjusting
        // mpv's mapping peak inversely changes diffuse/reference white without
        // ever moving the physical display peak.
        m_hdrVideoScale = m_hdrActive ? targetPeak / 80.0f : 1.0f;
        const float mappingPeak = std::clamp(
            targetPeak * 203.0f / referenceWhite, 10.0f, 10000.0f);
        char hdrKey[200];
        snprintf(hdrKey, sizeof(hdrKey),
                 "%d/%.3f/%d/%.3f/%.3f/%d/%d/%d/%.3f",
                 m_hdrActive ? 1 : 0, targetPeak, dc.mode, dc.peakNits,
                 dc.hdrRefWhite, dc.hdrOutput, dc.toneAlg, dc.gamutMode,
                 dc.panscan);
        if (m_hdrPlayerConfigKey != hdrKey) {
            if (m_hdrActive) {
                m_player->setStringProperty("target-prim", "bt.709");
                m_player->setStringProperty("target-trc", "linear");
                m_player->setDoubleProperty("target-peak", mappingPeak);
                m_player->setDoubleProperty("hdr-reference-white", dc.hdrRefWhite);
            } else {
                Settings::applyDisplayConfigToPlayer(m_player.get());
            }
            m_hdrPlayerConfigKey = hdrKey;
        }

        render();
        renderUI();

        if (m_finalFbo != 0) {
            m_hdrPresenter->present();
        } else {
            m_window->swapBuffers();
        }
        // Advanced libmpv render control needs the time of the actual
        // presentation, not the earlier render-to-FBO call.  Gamescope and
        // output devices can run from different clocks; reporting each swap
        // gives mpv the feedback required to keep video timed to audio.
        if (m_player)
            m_player->reportSwap();

        frameCount++;
    }
    fprintf(stderr, "[App] Exiting main loop after %d frames\n", frameCount);
}

void Application::toggleVideoFullscreen() {
    m_videoFullscreen = !m_videoFullscreen;
    m_window->toggleFullscreen();
    m_fullscreenCursorTimer = 3.0f;

    // The fullscreen swap resizes the video FBO from panel-size to
    // viewport-size, which makes libplacebo rebuild its render pipeline
    // — and silently drops the active user shader / target overrides on
    // the way.  Re-push everything so the new pipeline picks it up.
    if (m_player) {
        Settings::applySubtitleStyleToPlayer(m_player.get());
        Settings::applyCinemaGrainToPlayer (m_player.get());
        Settings::applyDisplayConfigToPlayer(m_player.get());
        Settings::applyPlaybackConfigToPlayer(m_player.get());
    }
}

// Absolute ffmpeg stream index of a TrueHD Atmos track in the current
// file, or -1. "Atmos" here = TrueHD with >6 channels (7.1 bed + objects);
// a plain TrueHD 5.1 has nothing to extract.
int Application::atmosTrackIndex() const {
    if (!m_player) return -1;
    for (const auto& t : m_player->getAudioTracks()) {
        if (t.codec == "truehd" && t.ffIndex >= 0 &&
            (t.channels >= 8 || t.title.find("Atmos") != std::string::npos ||
                                 t.title.find("atmos") != std::string::npos))
            return t.ffIndex;
    }
    return -1;
}

// User pressed "Render Atmos objects": kick the background chain.
void Application::requestBinaural() {
    if (m_binauralMovie.empty()) return;
    int idx = atmosTrackIndex();
    if (idx < 0) return;
    m_binauralArmed = true;
    m_extFromPartial = false;
    m_binauralLastPos = -1.0;
    std::string sofa = m_sharedState && m_sharedState->sofa_path[0]
                         ? std::string(m_sharedState->sofa_path)
                         : plat::assetsDir() + "hrtf/default.sofa";
    m_binaural.request(m_binauralMovie, idx, sofa, Settings::roomPreset(),
                       m_player ? m_player->getDuration() : 0.0,
                       (BinauralRenderer::Solo)m_binauralSolo);
}

// Spatial-tab control: only shown when the current file has a TrueHD
// Atmos track. Renders the object bed through truehdd + the DSP into a
// cached binaural sidecar and swaps mpv's audio to it.
void Application::renderAtmosObjectsUi() {
    int idx = atmosTrackIndex();
    if (idx < 0) return;   // nothing object-based to offer

    ImGui::SeparatorText("Atmos objects");
    auto st = m_binaural.state();
    const bool external = m_player && m_player->usingExternalBinaural();
    if (external &&
        (st == BinauralRenderer::State::Ready ||
         st == BinauralRenderer::State::Rendering)) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                           "Object-based binaural active");
        if (st == BinauralRenderer::State::Rendering) {
            ImGui::ProgressBar(m_binaural.progress(), ImVec2(-1, 0));
            ImGui::TextDisabled(
                "Still rendering ahead (%.0f min ready). Seeking past the "
                "rendered part falls back to the 7.1 bed until it catches up.",
                m_binaural.renderedSeconds() / 60.0);
        } else {
            ImGui::TextDisabled("Rendered through truehdd + the full DSP.");
        }
        // Audition selector: the only way to hear the objects (or the bed)
        // alone on this path — the mix happens inside the render, so the
        // realtime filter's debug solos can't touch it. Switching re-renders
        // that layer (streaming, so it's ~1 min) and caches it separately.
        static const char* kMixNames[] = { "Full mix", "Bed only", "Objects only" };
        int mix = (m_binauralSolo >= 0 && m_binauralSolo <= 2) ? m_binauralSolo : 0;
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("Audition", &mix, kMixNames, 3) && mix != m_binauralSolo) {
            m_binauralSolo = mix;
            requestBinaural();   // new cache key → streaming re-render + swap
        }
        if (m_binauralSolo != 0)
            ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f),
                               "Auditioning one layer — not the full mix.");

        if (ImGui::Button("Back to 7.1 bed (real-time)")) {
            m_binauralArmed = false;
            m_binaural.cancel();
            m_player->revertInternalAudio();
        }
    } else if (st == BinauralRenderer::State::Rendering) {
        ImGui::Text("Rendering Atmos objects...");
        ImGui::ProgressBar(m_binaural.progress(), ImVec2(-1, 0));
        ImGui::TextDisabled("Streaming render: playback switches to objects "
                            "automatically once it's far enough ahead. "
                            "Cached for next time.");
        if (ImGui::Button("Cancel")) { m_binauralArmed = false; m_binaural.cancel(); }
    } else {
        ImGui::TextWrapped("This track carries Atmos objects. The real-time "
                           "path spatializes the 7.1 bed; render the true "
                           "objects for full height/movement precision.");
        if (st == BinauralRenderer::State::Failed)
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f),
                               "Last render failed: %s", m_binaural.error().c_str());
        if (ImGui::Button("Render Atmos objects (binaural)"))
            requestBinaural();
    }
}

// Per-frame: when a render finishes, swap mpv's audio to the sidecar.
void Application::updateBinaural() {
    if (!m_player) return;

    // A pre-rendered binaural sidecar is spatial audio as well.  Stereo mode
    // must not allow a pending render to switch it back in on a later frame.
    if (!atomic_load(&m_sharedState->hrtf_enabled)) {
        if (m_player->usingExternalBinaural())
            m_player->revertInternalAudio();
        if (m_binauralArmed) {
            m_binauralArmed = false;
            m_binaural.cancel();
        }
        return;
    }

    if (!m_binauralArmed) return;

    const auto st = m_binaural.state();
    const double pos      = m_player->getPosition();
    const double rendered = m_binaural.renderedSeconds();
    const bool   external = m_player->usingExternalBinaural();

    // How far ahead the render head must be before we trust it for live
    // playback, and how close the playhead may get before we bail back to
    // the bed-based filter. The gap between the two is the hysteresis that
    // keeps a borderline render speed from flapping.
    constexpr double kLeadIn      = 30.0;
    constexpr double kFallBehind  = 5.0;
    constexpr double kSeekJump    = 10.0;

    const bool seeked = m_binauralLastPos >= 0.0 &&
                        std::fabs(pos - m_binauralLastPos) > kSeekJump;
    m_binauralLastPos = pos;

    switch (st) {
    case BinauralRenderer::State::Rendering:
        if (!external) {
            // Hot-swap mid-render as soon as the sidecar is safely ahead.
            if (rendered > pos + kLeadIn && !m_binaural.resultPath().empty()) {
                m_player->useExternalBinaural(m_binaural.resultPath());
                m_extFromPartial = true;
            }
        } else {
            // A seek re-opens the growing file (its remembered size is
            // stale), and outrunning the render falls back to beds. Either
            // way the !external branch re-attaches when it's safe again.
            if (seeked || pos > rendered - kFallBehind)
                m_player->revertInternalAudio();
        }
        break;

    case BinauralRenderer::State::Ready:
        if (!external) {
            m_player->useExternalBinaural(m_binaural.resultPath());
            m_extFromPartial = false;
        } else if (m_extFromPartial) {
            // Attached while the file was still growing: re-open it once so
            // the demuxer learns the real duration and seeks work anywhere.
            m_player->revertInternalAudio();
            m_player->useExternalBinaural(m_binaural.resultPath());
            m_extFromPartial = false;
        }
        break;

    case BinauralRenderer::State::Failed:
        if (external) m_player->revertInternalAudio();
        break;

    case BinauralRenderer::State::Idle:
        break;
    }
}

void Application::processInput() {
    // Handle pending file drops
    if (!m_pendingFile.empty() && m_player) {
        if (FILE* dbg = fopen((plat::cacheDir() + "app_debug.log").c_str(), "a")) {
            fprintf(dbg, "loadFile dispatch: '%s'\n", m_pendingFile.c_str());
            fclose(dbg);
        }
        checkpointCurrentPlayback();
        if (m_pendingStartOver) ResumeStore::clear(m_pendingFile);
        m_autoResumeOnLoad = m_pendingAutoResume;
        m_pendingStartOver = false;
        m_pendingAutoResume = false;
        m_currentMediaPath = m_pendingFile;
        m_resumePromptPending = false;
        m_resumePopupOpen = false;
        m_resumeTrackingReady = false;
        m_resumeCheckpointElapsed = 0.0f;
        m_resumeLastPosition = 0.0;
        m_resumeLastPaused = true;
        m_resumeLastAudioTrack = -1;
        m_resumeLastSubtitleTrack = -1;
        m_resumeLastSubtitlesVisible = true;
        m_player->loadFile(m_pendingFile);
        m_controlPanel->loadSidecar(m_pendingFile);
        Settings::pushRecent(m_pendingFile);
        // New file → forget any previous object-render arming.
        m_binauralMovie = m_pendingFile;
        m_binauralArmed = false;
        m_binaural.cancel();
        m_pendingFile.clear();
    }

    updateBinaural();

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

    // Escape in video fullscreen: close the playback menu first if it's
    // up, otherwise exit fullscreen.
    if (m_videoFullscreen && glfwGetKey(handle, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        if (!m_escKeyHeld) {
            if (m_playbackMenuOpen) closePlaybackMenu();
            else                    toggleVideoFullscreen();
            m_escKeyHeld = true;
        }
    } else if (glfwGetKey(handle, GLFW_KEY_ESCAPE) == GLFW_RELEASE) {
        m_escKeyHeld = false;
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

    // F3 used to toggle the legacy Control Panel drawer.  That panel
    // moved into Preferences (Spatial / Headphone EQ tabs), so the
    // shortcut is gone — Ctrl+, opens the same controls now.

    // The patched GLFW backend feeds every standard gamepad key (D-pad,
    // sticks, face buttons, shoulders and triggers) from this same selected
    // device. Keep selection/logging here for diagnostics and for the file
    // dialog's TV-specific Y/X shortcuts; do not synthesize arrow keys on top
    // of ImGui navigation, which used to produce double or skipped moves.
    int pad = -1;
    for (int j = GLFW_JOYSTICK_1; j <= GLFW_JOYSTICK_LAST; j++) {
        if (glfwJoystickPresent(j) && glfwJoystickIsGamepad(j)) { pad = j; break; }
    }
    // Report connect/disconnect once rather than every frame.  "Nothing
    // happens when I press buttons" is otherwise indistinguishable from a
    // controller GLFW has no mapping for, which is the usual cause.
    if (pad != m_activePad) {
        if (pad >= 0)
            fprintf(stderr, "[Input] gamepad %d: %s\n", pad, glfwGetGamepadName(pad));
        else if (m_activePad >= 0)
            fprintf(stderr, "[Input] gamepad disconnected\n");
        m_activePad = pad;
    }
    if (pad >= 0) {
        GLFWgamepadstate gp;
        if (glfwGetGamepadState(pad, &gp)) {
            ImGuiIO& io = ImGui::GetIO();
            // File-dialog convenience bindings — only active while the
            // ImGuiFileDialog popup is up so they don't pollute the
            // rest of the UI.  Y forwards to Enter, mirroring the
            // dialog's keyboard "confirm / open folder" gesture; X
            // forwards to Backspace, mirroring "go up to parent".
            // Done by polling GLFW directly (rather than IsKeyDown on
            // ImGui's gamepad keys) because ImGui_ImplGlfw populates
            // those during NewFrame, which runs *after* this
            // processInput pass — IsKeyDown here would lag a frame.
            if (ImGuiFileDialog::Instance()->IsOpened()) {
                const bool curY = gp.buttons[GLFW_GAMEPAD_BUTTON_Y] == GLFW_PRESS;
                const bool curX = gp.buttons[GLFW_GAMEPAD_BUTTON_X] == GLFW_PRESS;
                if (curY != m_dialogYHeld) io.AddKeyEvent(ImGuiKey_Enter,     curY);
                if (curX != m_dialogXHeld) io.AddKeyEvent(ImGuiKey_Backspace, curX);
                m_dialogYHeld = curY;
                m_dialogXHeld = curX;
            } else if (m_dialogYHeld || m_dialogXHeld) {
                // Dialog just closed while a button was still held —
                // release the synthetic keys so they don't get stuck.
                if (m_dialogYHeld) io.AddKeyEvent(ImGuiKey_Enter,     false);
                if (m_dialogXHeld) io.AddKeyEvent(ImGuiKey_Backspace, false);
                m_dialogYHeld = false;
                m_dialogXHeld = false;
            }
        }
    }
}

void Application::update(float dt) {
    if (m_player)
        m_player->update();

    // Checkpoint frequently enough to survive a compositor/process kill,
    // plus immediately at semantic boundaries. The atomic ResumeStore write
    // means a power loss can lose at most the newest checkpoint, never the
    // previous valid database.
    if (m_player && !m_currentMediaPath.empty()) {
        if (m_player->consumeReachedEnd()) {
            WatchHistoryStore::finish(m_currentMediaPath, m_player->getDuration());
            ResumeStore::clear(m_currentMediaPath);
            m_resumePromptPending = false;
            m_resumePopupOpen = false;
        } else if (m_resumeTrackingReady && !m_resumePromptPending &&
                   m_player->hasVideo()) {
            const double pos = m_player->getPosition();
            const bool paused = m_player->isPaused();
            const int audioTrack = m_player->getResumeAudioTrackId();
            const int subtitleTrack = m_player->getCurrentSubtitleTrackId();
            const bool subtitlesVisible = m_player->areSubtitlesVisible();
            m_resumeCheckpointElapsed += dt;
            const double plausibleAdvance = std::max(2.0, (double)dt * 5.0);
            const bool seeked = std::fabs(pos - m_resumeLastPosition) > plausibleAdvance;
            const bool pauseChanged = paused != m_resumeLastPaused;
            const bool trackChanged =
                audioTrack != m_resumeLastAudioTrack ||
                subtitleTrack != m_resumeLastSubtitleTrack ||
                subtitlesVisible != m_resumeLastSubtitlesVisible;
            if (m_resumeCheckpointElapsed >= 5.0f || seeked || pauseChanged ||
                trackChanged) {
                checkpointCurrentPlayback();
                m_resumeCheckpointElapsed = 0.0f;
            }
            m_resumeLastPosition = pos;
            m_resumeLastPaused = paused;
            m_resumeLastAudioTrack = audioTrack;
            m_resumeLastSubtitleTrack = subtitleTrack;
            m_resumeLastSubtitlesVisible = subtitlesVisible;
        }
    }

    m_transportBar->setFullscreenState(m_videoFullscreen);

    // Sync spatial object positions from sidecar to shared state
    if (m_controlPanel)
        m_controlPanel->updateObjectPositions();

    // Transport-bar auto-hide.  Reset the idle timer on any user input
    // — mouse motion, mouse click, keyboard or gamepad button.  When
    // the timer crosses kTransportHideDelay we flip m_transportVisible
    // and the dock layout rebuilds in renderUI to give the freed
    // height back to the video.  ImGui's queue-driven input means we
    // can read this state cheaply: io.MouseDelta gives us frame-level
    // motion, IsKeyDown over the named-key range covers everything
    // the keyboard / gamepad can produce.  The playback overlay menu
    // counts as "interacting" so the bar stays hidden while the menu
    // is up — pop the menu and the timer starts ticking again.
    {
        ImGuiIO& io = ImGui::GetIO();
        bool activity = false;
        if (std::fabs(io.MouseDelta.x) > 1.0f ||
            std::fabs(io.MouseDelta.y) > 1.0f) {
            activity = true;
        }
        if (!activity) {
            for (int b = 0; b < 5; b++) {
                if (ImGui::IsMouseDown(b)) { activity = true; break; }
            }
        }
        if (!activity) {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                if (ImGui::IsKeyDown((ImGuiKey)k)) { activity = true; break; }
            }
        }

        if (activity || m_playbackMenuOpen) {
            m_transportIdleTimer = 0.0f;
        } else {
            m_transportIdleTimer += dt;
        }

        const bool wantVisible = (m_transportIdleTimer < kTransportHideDelay) &&
                                  !m_playbackMenuOpen;
        if (wantVisible != m_transportVisible) {
            m_transportVisible = wantVisible;
            // The renderUI layout-rebuild guard keys off this flag so
            // it picks up the swap on its next frame.
        }
    }

    // Open the audio-track picker the first frame after a fresh load.
    // mpv has been kept paused so we can take our time before the first
    // audible frame.
    if (m_player && m_trackPicker && m_player->consumeFreshFileLoaded()) {
        m_resumePromptPending =
            ResumeStore::find(m_currentMediaPath, &m_resumeEntry);

        // A per-file resume choice is more specific than the global language
        // preference. Restore only IDs that still exist in this exact file;
        // ResumeStore's size+mtime fingerprint already guards replacements.
        bool restoredAudio = false;
        if (m_resumePromptPending && m_resumeEntry.audioTrackId >= 0) {
            for (const auto& t : m_player->getAudioTracks()) {
                if (!t.isExternal && t.id == m_resumeEntry.audioTrackId) {
                    m_player->setAudioTrack(t.id);
                    restoredAudio = true;
                    break;
                }
            }
        }
        if (!restoredAudio) {
            const std::string& prefAud = Settings::preferredAudioLang();
            if (!prefAud.empty()) {
                for (const auto& t : m_player->getAudioTracks())
                    if (Settings::langMatches(t.lang, prefAud)) {
                        m_player->setAudioTrack(t.id);
                        break;
                    }
            }
        }

        bool restoredSubtitle = false;
        if (m_resumePromptPending && m_resumeEntry.subtitleTrackId == 0) {
            m_player->setSubtitleTrack(0);
            restoredSubtitle = true;
        } else if (m_resumePromptPending &&
                   m_resumeEntry.subtitleTrackId > 0) {
            for (const auto& s : m_player->getSubtitleTracks()) {
                if (s.id == m_resumeEntry.subtitleTrackId) {
                    m_player->setSubtitleTrack(s.id);
                    m_player->setSubtitlesVisible(
                        m_resumeEntry.subtitlesVisible);
                    restoredSubtitle = true;
                    break;
                }
            }
        }
        if (!restoredSubtitle) {
            const std::string& prefSub = Settings::preferredSubLang();
            if (!prefSub.empty()) {
                for (const auto& s : m_player->getSubtitleTracks())
                    if (Settings::langMatches(s.lang, prefSub)) {
                        m_player->setSubtitleTrack(s.id);
                        break;
                    }
            }
        }
        // HRTF_UI_OPEN=subs opens the subtitle side instead — a hook for
        // screenshotting the picker without driving the UI by hand.
        const char* uiOpen = std::getenv("HRTF_UI_OPEN");
        const bool wantSubs = uiOpen && strcmp(uiOpen, "subs") == 0;
        m_resumeTrackingReady = true;
        m_resumeLastPosition = m_player->getPosition();
        m_resumeLastPaused = m_player->isPaused();
        m_resumeLastAudioTrack = m_player->getResumeAudioTrackId();
        m_resumeLastSubtitleTrack = m_player->getCurrentSubtitleTrackId();
        m_resumeLastSubtitlesVisible = m_player->areSubtitlesVisible();

        if (!m_player->getAudioTracks().empty()) {
            m_trackPicker->open(wantSubs ? TrackPicker::Mode::Subtitle
                                         : TrackPicker::Mode::Audio,
                                /*isAutoLoad=*/!m_resumePromptPending);
        } else if (!m_resumePromptPending) {
            // Audio-less or unusual media has no picker to resume it.
            m_player->play();
        }

        // HRTF_AUTO_OBJECTS=1 arms the object render on load, as if the
        // user had pressed "Render Atmos objects" — automation hook.
        if (const char* ao = std::getenv("HRTF_AUTO_OBJECTS");
            ao && *ao == '1')
            requestBinaural();
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

// Composite the ImGui frame into the active output framebuffer. In HDR
// mode that's the D3D11 interop FBO (scRGB backbuffer, cleared each frame
// since the lock discards its contents); in SDR mode it's the default
// framebuffer, matching the original behavior exactly.
void Application::finalizeFrame() {
    if (m_finalFbo != 0) {
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(m_window->getHandle(), &fbw, &fbh);
        glBindFramebuffer(GL_FRAMEBUFFER, m_finalFbo);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    m_imgui->endFrame();
}

void Application::renderUI() {
    // Gamepad polling is handled by imgui_impl_glfw inside NewFrame —
    // no manual call needed here.
    m_imgui->beginFrame();

    // Track mouse movement for auto-hiding transport in fullscreen.
    // The threshold is intentionally large — sub-pixel jitter from the
    // touchpad / touchscreen / accumulated rounding would otherwise reset
    // the inactivity timer every frame and make the cursor pop back in
    // immediately after being hidden.  We also only refresh the baseline
    // (m_lastMouseX/Y) once the threshold is actually crossed, so a slow
    // drift can never sneak above it through accumulation.
    ImVec2 mousePos = ImGui::GetMousePos();
    if (m_videoFullscreen) {
        const float threshold = m_cursorHidden ? 8.0f : 2.0f;
        const float dx = mousePos.x - m_lastMouseX;
        const float dy = mousePos.y - m_lastMouseY;
        if (fabsf(dx) > threshold || fabsf(dy) > threshold) {
            m_fullscreenCursorTimer = 3.0f;
            m_lastMouseX = mousePos.x;
            m_lastMouseY = mousePos.y;
        }
        float dt = ImGui::GetIO().DeltaTime;
        m_fullscreenCursorTimer -= dt;
        if (m_fullscreenCursorTimer < 0) m_fullscreenCursorTimer = 0;
    } else {
        m_lastMouseX = mousePos.x;
        m_lastMouseY = mousePos.y;
    }

    // Hide the OS cursor through ImGui rather than calling
    // glfwSetInputMode directly: imgui_impl_glfw resets the GLFW cursor
    // mode every frame from ImGui::GetMouseCursor(), so a direct call
    // gets overwritten and the cursor pops back in.  Setting the ImGui
    // cursor to None each frame the cursor should be hidden makes the
    // backend issue the correct GLFW_CURSOR_HIDDEN call for us.
    //
    // Two paths fold into the same hide:
    //   - F11 cinema mode: hidden once m_fullscreenCursorTimer expires.
    //   - Docked playback: hidden when the transport bar auto-hid AND
    //     the playback overlay isn't up (during the overlay menu the
    //     user explicitly needs the cursor to operate it).
    const bool hasMediaForCursor = m_player && m_player->hasVideo();
    const bool hideCursor =
        m_videoFullscreen
            ? (m_fullscreenCursorTimer <= 0.0f && !m_playbackMenuOpen)
            : (hasMediaForCursor && !m_transportVisible &&
                                    !m_playbackMenuOpen);
    if (hideCursor) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        m_cursorHidden = true;
    } else {
        m_cursorHidden = false;
    }

    // --- Video fullscreen mode ---
    if (m_videoFullscreen) {
        // The docked path has its own routing block below, but fullscreen
        // returns before reaching it. Route the same controller actions here
        // so Start/B, Play, seeking, volume and track selection don't vanish
        // when cinema mode is active.
        const bool prefsOpenNow = m_prefsDialog && m_prefsDialog->isOpen();
        const bool anyPopupNow = ImGui::IsPopupOpen(nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const bool startEdge = ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false);
        const bool bEdge = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        if ((startEdge || bEdge) && !prefsOpenNow && !anyPopupNow) {
            if (m_playbackMenuOpen) closePlaybackMenu();
            else                    openPlaybackMenu();
        }
        handlePlaybackShortcuts(prefsOpenNow, anyPopupNow);

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
            if (m_hdrActive)
                ImGui::GetWindowDrawList()->AddCallback(
                    setHdrVideoColorMode, &m_hdrVideoScale, sizeof(m_hdrVideoScale));
            ImGui::Image((ImTextureID)(intptr_t)m_videoTexture, vpSize,
                         ImVec2(0, 1), ImVec2(1, 0));
            if (m_hdrActive)
                ImGui::GetWindowDrawList()->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

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
        // Modal layers still work over fullscreen video — without this
        // the transport bar's menu/open/picker buttons silently did
        // nothing in fullscreen (the dialogs only rendered in docked
        // mode).
        renderModalLayers();
        finalizeFrame();
        return;
    }

    // --- Normal docked mode ---

    // Top-level keyboard / gamepad routing.  The same physical buttons
    // mean different things depending on whether a file is loaded:
    //
    //   No media (home screen):
    //     Alt / Start  → toggle the developer menu bar
    //     Esc          → dismiss the menu bar (when no popup is open)
    //
    //   Media playing:
    //     Start / B    → toggle the in-playback overlay menu (and
    //                    pause/resume the video alongside it).  Skipped
    //                    when Preferences or any popup is up so those
    //                    layers can consume the cancel key first.
    //     Alt          → still toggles the dev menu bar, for parity.
    {
        const bool altEdge =
            ImGui::IsKeyPressed(ImGuiKey_LeftAlt,  false) ||
            ImGui::IsKeyPressed(ImGuiKey_RightAlt, false);
        const bool startEdge =
            ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false);
        const bool bEdge =
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        const bool escEdge =
            ImGui::IsKeyPressed(ImGuiKey_Escape, false);

        const bool hasMediaNow  = m_player && m_player->hasVideo();
        const bool prefsOpenNow = m_prefsDialog && m_prefsDialog->isOpen();
        const bool anyPopupNow  = ImGui::IsPopupOpen(nullptr,
                                       ImGuiPopupFlags_AnyPopupId |
                                       ImGuiPopupFlags_AnyPopupLevel);

        if (hasMediaNow) {
            // Playback context.  Start always toggles the menu; B and Esc
            // only do so when nothing else owns the cancel gesture, so
            // popups in Preferences / dropdowns close first.  (In video
            // fullscreen Esc exits fullscreen instead — that path returns
            // before reaching here.)
            bool consumedPlay = false;
            if (m_playbackMenuOpen && !prefsOpenNow && !anyPopupNow &&
                ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                closePlaybackMenu(/*forcePlay=*/true);
                consumedPlay = true;
            } else if (startEdge ||
                ((bEdge || escEdge) && !prefsOpenNow && !anyPopupNow)) {
                if (m_playbackMenuOpen) closePlaybackMenu();
                else                    openPlaybackMenu();
            }
            if (altEdge) m_menuBarVisible = !m_menuBarVisible;

            if (!consumedPlay)
                handlePlaybackShortcuts(prefsOpenNow, anyPopupNow);
        } else {
            // Home / Preferences context — keep the original menu-bar
            // muscle memory.
            if (altEdge || startEdge) {
                m_menuBarVisible = !m_menuBarVisible;
            } else if (escEdge && m_menuBarVisible && !anyPopupNow) {
                m_menuBarVisible = false;
            }
        }
    }

    // Top menu bar — exposes panels and the persisted-config controls.
    bool dirty = Settings::isDirty(m_sharedState,
                                    m_showControlPanel, m_show3DViz);
    if (m_menuBarVisible && ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(ICON_LC_FILE "  File")) {
            if (ImGui::MenuItem(ICON_LC_FOLDER_OPEN "  Open...", "Ctrl+O")) {
                openFileDialog();
            }
            if (ImGui::MenuItem(ICON_LC_CAPTIONS "  Load Subtitle...",
                                 nullptr, false,
                                 m_player && m_player->hasVideo())) {
                IGFD::FileDialogConfig cfg;
                cfg.path = Settings::fileDialogStartDirectory();
                cfg.flags = ImGuiFileDialogFlags_Modal |
                            ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;
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
                // Reset changes the persisted output-mode flag; apply the
                // matching filter chain immediately as the mode buttons do.
                if (m_player)
                    m_player->setSpatialAudioEnabled(
                        atomic_load(&m_sharedState->hrtf_enabled) != 0);
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
            openFileDialog();
        }
    }

    // No file loaded → show the TV-mode home screen instead of an
    // empty dockspace.  Dialogs (file picker / preferences / recent)
    // render via renderModalLayers on top of whatever is showing, so
    // the home buttons can open them normally.
    const bool hasMedia = m_player && m_player->hasVideo();
    const bool prefsOpen = m_prefsDialog && m_prefsDialog->isOpen();
    if (!hasMedia) {
        // Skip the home screen when the preferences page is up — the
        // page is full-screen, drawing the home buttons underneath
        // would only confuse focus / nav routing.  Recent dialog stays
        // because it's a modal opened *from* the home and small enough
        // to overlay cleanly.
        if (!prefsOpen) renderHomeScreen();
        renderModalLayers();
        finalizeFrame();
        return;
    }

    // Media is loaded — arm the home screen's nav-seeding flag so the
    // next time playback ends and we drop back to the landing, the
    // gamepad cursor lands on the first button again.
    m_homeFreshlyShown = true;

    // Enable docking
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // Rebuild the dock layout whenever the 3D Visualizer's visibility
    // *or* the transport's visibility flips.  The transport-bar
    // auto-hide drops the bottom dock node and the video reclaims the
    // freed vertical space; without rebuilding here the empty
    // transport node would just sit there as a 150 px gap below the
    // video.
    static bool layoutBuilt    = false;
    static bool prevShowViz    = false;
    static bool prevTransport  = true;
    if (!layoutBuilt ||
        prevShowViz   != m_show3DViz ||
        prevTransport != m_transportVisible) {

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        ImGuiID leftCol  = dockspace_id;
        ImGuiID rightCol = 0;
        if (m_show3DViz) {
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left,
                                         0.75f, &leftCol, &rightCol);
        }

        ImGuiID videoSlot = 0;
        ImGuiID transportSlot = 0;
        if (m_transportVisible) {
            // Left column: video on top, transport docked below.  The
            // transport hosts three rows of content (header,
            // full-width timeline, controls), so a fixed pixel height
            // converted to a ratio is more honest than a flat ratio.
            // Reserve enough height for the responsive toolbar's second row.
            // On wide canvases this is simply breathing room; on narrower
            // panes it prevents wrapped controls from being clipped.
            const float transportPx = 190.0f * ImGuiLayer::effectiveScale();
            const float dockPx      = ImGui::GetMainViewport()->Size.y -
                                       ImGui::GetFrameHeight();
            float transportRatio = transportPx / dockPx;
            if (transportRatio < 0.08f) transportRatio = 0.08f;
            if (transportRatio > 0.45f) transportRatio = 0.45f;

            ImGui::DockBuilderSplitNode(leftCol, ImGuiDir_Down,
                                         transportRatio,
                                         &transportSlot, &videoSlot);
            ImGui::DockBuilderDockWindow("Video",     videoSlot);
            ImGui::DockBuilderDockWindow("Transport", transportSlot);
        } else {
            // Auto-hide path: only the video panel is docked, taking
            // the whole left column.  The Transport window simply
            // isn't rendered this frame (see below).
            videoSlot = leftCol;
            ImGui::DockBuilderDockWindow("Video", videoSlot);
        }

        if (m_show3DViz)
            ImGui::DockBuilderDockWindow("HRTF Visualizer", rightCol);

        // Strip the per-window tab bar from the video / transport
        // nodes so a single docked window reads as a frame, not a
        // tabbed panel.  Otherwise the dock paints a "Video" tab
        // header above the image even though there's only ever one
        // window in that node — visible chrome the user doesn't need.
        if (videoSlot) {
            if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(videoSlot))
                n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar |
                                 ImGuiDockNodeFlags_NoWindowMenuButton |
                                 ImGuiDockNodeFlags_NoCloseButton;
        }
        if (transportSlot) {
            if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(transportSlot))
                n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar |
                                 ImGuiDockNodeFlags_NoWindowMenuButton |
                                 ImGuiDockNodeFlags_NoCloseButton;
        }

        ImGui::DockBuilderFinish(dockspace_id);
        layoutBuilt   = true;
        prevShowViz   = m_show3DViz;
        prevTransport = m_transportVisible;
    }

    // Video panel — no chrome, edge-to-edge.  The dock node already
    // strips the tab bar; we strip the title bar / scrollbar / border
    // and zero out the window padding so the ImGui::Image below sits
    // pixel-flush against the panel rect.  Without this you'd see a
    // "Video" titlebar plus a few pixels of inset framing around the
    // frame, even when the transport is hidden.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("Video", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    ImVec2 videoSize = ImGui::GetContentRegionAvail();

    if (videoSize.x > 0 && videoSize.y > 0) {
        int newW = (int)videoSize.x;
        int newH = (int)videoSize.y;
        resizeFBOs(newW, newH, m_vizWidth, m_vizHeight);

#ifdef HAVE_MPV
        if (m_player && m_player->hasVideo()) {
            m_player->renderToFBO(m_videoFBO, m_videoWidth, m_videoHeight);
            if (m_hdrActive)
                ImGui::GetWindowDrawList()->AddCallback(
                    setHdrVideoColorMode, &m_hdrVideoScale, sizeof(m_hdrVideoScale));
            ImGui::Image((ImTextureID)(intptr_t)m_videoTexture, videoSize,
                         ImVec2(0, 1), ImVec2(1, 0));
            if (m_hdrActive)
                ImGui::GetWindowDrawList()->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
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

    // Split the viz panel: 3D image on the left, speaker list sidebar on
    // the right.  The per-speaker controls (colours, position sliders,
    // RMS bars, Test buttons) only really make sense alongside the 3D
    // view, so they live here instead of in the Control Panel.
    ImVec2 totalAvail = ImGui::GetContentRegionAvail();
    const float minSidebar = 240.0f;
    const float maxSidebar = 320.0f;
    float sidebarW = totalAvail.x * 0.30f;
    if (sidebarW < minSidebar) sidebarW = minSidebar;
    if (sidebarW > maxSidebar) sidebarW = maxSidebar;
    if (sidebarW > totalAvail.x * 0.5f) sidebarW = totalAvail.x * 0.5f;
    float vizW = totalAvail.x - sidebarW - ImGui::GetStyle().ItemSpacing.x;
    if (vizW < 1.0f) vizW = 1.0f;

    ImGui::BeginChild("##viz_image", ImVec2(vizW, 0), false,
                      ImGuiWindowFlags_NoScrollbar);
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
    ImGui::EndChild();   // ##viz_image

    ImGui::SameLine();

    ImGui::BeginChild("##viz_speakers", ImVec2(sidebarW, 0), true);
    if (m_controlPanel) m_controlPanel->renderSpeakerList();
    ImGui::EndChild();

    ImGui::End();
    } // if (m_show3DViz)

    // (Control Panel as a docked window has been removed; its content
    // lives on the Preferences page now under the Spatial / Headphone
    // EQ tabs.  The ControlPanel object is still alive because it owns
    // the SOFA / IR / EQ scanning + spatial-sidecar loading code.)

    // Transport bar — auto-hidden after kTransportHideDelay seconds of
    // user idleness, restored on any input.  Skipping the render
    // (combined with the dock rebuild above) is what gives the freed
    // height back to the video.
    if (m_transportVisible)
        m_transportBar->render();

    // In-playback overlay menu — paint above the dock so it dims the
    // (paused) video naturally.  Track picker / Preferences /
    // Recent dialog still render on top of it because they're each a
    // separate ImGui window opened later in the frame; that gives us
    // the back-stack we want: Preferences → playback menu → video.
    renderModalLayers();

    finalizeFrame();
}

/*
 * Modal layers shared by every UI mode (docked, home screen and video
 * fullscreen): the file dialogs, the in-playback menu, the track picker,
 * Preferences and the Recent list.  The fullscreen branch used to return
 * before any of these rendered, so its transport-bar buttons (menu, open,
 * audio/subtitle pickers) silently did nothing.
 */
void Application::renderModalLayers() {
    // The file explorer is a primary TV screen, not a small desktop popup.
    // Open it near full-screen by default while retaining resize handles and
    // a small safe margin. Users can grow it to virtually the whole work area.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 vpSize = viewport->WorkSize;
    const float tv = ImGuiLayer::effectiveScale();
    ImVec2 dlgMin(std::min(720.0f * tv, vpSize.x * 0.80f),
                  std::min(480.0f * tv, vpSize.y * 0.78f));
    ImVec2 dlgDefault(vpSize.x * 0.94f, vpSize.y * 0.92f);
    ImVec2 dlgMax(vpSize.x * 0.99f, vpSize.y * 0.98f);
    // NOTE: GetFilePathName() is the SAVE-dialog accessor — it recomposes
    // the name through extension logic that collapses consecutive dots
    // ("2004..2160p" became "2004.2160p"), silently producing a path that
    // doesn't exist. GetSelection() returns the picked entries untouched
    // and is the documented OPEN-dialog accessor.
    auto pickedPath = []() -> std::string {
        auto sel = ImGuiFileDialog::Instance()->GetSelection();
        if (!sel.empty()) return sel.begin()->second;
        return ImGuiFileDialog::Instance()->GetFilePathName(
            IGFD_ResultMode_KeepInputFile);
    };
    auto* fileDialog = ImGuiFileDialog::Instance();
    if (fileDialog->IsOpened("open_media"))
        ImGui::SetNextWindowSize(dlgDefault, ImGuiCond_Appearing);
    if (fileDialog->Display("open_media", ImGuiWindowFlags_NoCollapse, dlgMin, dlgMax)) {
        if (ImGuiFileDialog::Instance()->IsOk() && m_player) {
            m_pendingFile = pickedPath();
        }
        // Breadcrumb log: the exact dialog outcome, for diagnosing "Open
        // does nothing" reports without a console attached.
        if (FILE* dbg = fopen((plat::cacheDir() + "app_debug.log").c_str(), "a")) {
            fprintf(dbg, "open_media closed: ok=%d path='%s'\n",
                    fileDialog->IsOk() ? 1 : 0,
                    m_pendingFile.c_str());
            fclose(dbg);
        }
        Settings::setLastBrowseDirectory(fileDialog->GetCurrentPath());
        fileDialog->Close();
    }
    if (!m_mediaDialogRequestedPath.empty()) {
        m_pendingFile = m_mediaDialogRequestedPath;
        m_pendingStartOver = m_mediaDialogRequestedStartOver;
        m_pendingAutoResume = m_mediaDialogRequestedAutoResume;
        Settings::setLastBrowseDirectory(fileDialog->GetCurrentPath());
        fileDialog->Close();
        m_mediaDialogRequestedPath.clear();
        m_mediaDialogRequestedStartOver = false;
        m_mediaDialogRequestedAutoResume = false;
        m_showMediaDetailsPopup = false;
    }
    if (fileDialog->IsOpened("open_subtitle"))
        ImGui::SetNextWindowSize(dlgDefault, ImGuiCond_Appearing);
    if (fileDialog->Display("open_subtitle", ImGuiWindowFlags_NoCollapse, dlgMin, dlgMax)) {
        if (fileDialog->IsOk() && m_player) {
            m_player->loadSubtitleFile(pickedPath());
        }
        Settings::setLastBrowseDirectory(fileDialog->GetCurrentPath());
        fileDialog->Close();
    }

    if (m_playbackMenuOpen)
        renderPlaybackMenu();

    // Track picker modal — fires after a fresh load while playback is
    // still paused, so the user picks the audio stream up-front.
    if (m_trackPicker)
        m_trackPicker->render();

    renderResumeDialog();

    // Preferences modal (Settings > Preferences / Ctrl+,)
    if (m_prefsDialog)
        m_prefsDialog->render();

    // Recent-files modal opened from the home screen.
    if (m_showRecentDialog)
        renderRecentDialog();

    // Add/remove/rename actions happen inside ImGuiFileDialog::Display().
    // Comparing its opaque serialization here catches all of them in the
    // same frame, including a dialog that was just closed.
    persistFileDialogBookmarks();
}

void Application::persistFileDialogBookmarks() {
    auto* dialog = ImGuiFileDialog::Instance();
    const std::string current = dialog->SerializePlaces();
    if (current == m_fileDialogBookmarksSnapshot) return;

    const double now = glfwGetTime();
    if (now < m_bookmarkSaveRetryAfter) return;
    if (BookmarkStore::save(current)) {
        m_fileDialogBookmarksSnapshot = current;
        m_bookmarkSaveRetryAfter = 0.0;
    } else {
        // A transient filesystem problem should not lose the in-memory edit,
        // but also should not produce a write/log storm every render frame.
        m_bookmarkSaveRetryAfter = now + 2.0;
    }
}

// Shared by the menu, the Ctrl+O hotkey and the home-screen "Open"
// button so all three paths land on the same ImGuiFileDialog.
void Application::renderMediaFileStatus(const IGFD::FileInfos* info) {
    if (!info || !info->fileType.isFile()) return;
    const std::string path = info->filePath +
        std::filesystem::path::preferred_separator + info->fileNameExt;
    WatchHistoryStore::Entry history;
    const bool hasHistory = WatchHistoryStore::find(path, &history);
    const float fraction = hasHistory && history.duration > 0.0
        ? static_cast<float>(std::clamp(history.furthestPosition /
                                        history.duration, 0.0, 1.0)) : 0.0f;
    const bool started = hasHistory && history.furthestPosition >= 60.0 &&
                         fraction >= 0.02f;
    if (!hasHistory || (!history.watched && !started)) return;
    char label[32];
    if (history.watched) std::snprintf(label, sizeof(label), "Watched");
    else std::snprintf(label, sizeof(label), "%.0f%%", fraction * 100.0f);
    if (history.watched)
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                              ImVec4(0.28f, 0.70f, 0.46f, 1.0f));
    ImGui::ProgressBar(history.watched ? 1.0f : fraction,
                       ImVec2(-1.0f, ImGui::GetTextLineHeight()), label);
    if (history.watched) ImGui::PopStyleColor();
}

bool Application::mediaFileVisible(const IGFD::FileInfos* info) {
    if (m_mediaFileFilter == 0 || !info || !info->fileType.isFile()) return true;
    const std::string path = info->filePath +
        std::filesystem::path::preferred_separator + info->fileNameExt;
    WatchHistoryStore::Entry history;
    const bool hasHistory = WatchHistoryStore::find(path, &history);
    const double fraction = hasHistory && history.duration > 0.0
        ? history.furthestPosition / history.duration : 0.0;
    const bool started = hasHistory && history.furthestPosition >= 60.0 &&
                         fraction >= 0.02;
    if (m_mediaFileFilter == 1) return !hasHistory || (!started && !history.watched);
    if (m_mediaFileFilter == 2) return started && !history.watched;
    return hasHistory && history.watched;
}

bool Application::renderMediaFileToolbar() {
    static const char* views[] = {"All files", "Unwatched", "In progress", "Watched"};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Show");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::min(230.0f * ImGuiLayer::effectiveScale(),
                                     ImGui::GetContentRegionAvail().x));
    const bool changed = ImGui::Combo("##watch_filter", &m_mediaFileFilter, views, 4);
    if (!m_mediaInlineDetails) {
        ImGui::SameLine();
        const bool requested = ImGui::Button("Details  [R1]") ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadR1) ||
            (ImGui::IsKeyPressed(ImGuiKey_I) && !ImGui::GetIO().WantTextInput);
        if (requested) {
            auto* dialog = ImGuiFileDialog::Instance();
            const std::string highlighted = dialog->GetHighlightedFilePath();
            if (!highlighted.empty()) m_mediaDetailsPath = highlighted;
            else {
                const auto selection = dialog->GetSelection();
                if (!selection.empty()) m_mediaDetailsPath = selection.begin()->second;
            }
            if (!m_mediaDetailsPath.empty() &&
                std::filesystem::u8path(m_mediaDetailsPath).parent_path() !=
                    std::filesystem::u8path(dialog->GetCurrentPath()))
                m_mediaDetailsPath.clear();
            if (!m_mediaDetailsPath.empty()) m_showMediaDetailsPopup = true;
        }
    }
    return changed;
}

void Application::renderMediaDetailsPopup() {
    if (!m_showMediaDetailsPopup) return;
    ImGui::OpenPopup("File details##media");
    const ImVec2 viewport = ImGui::GetMainViewport()->WorkSize;
    const float tv = ImGuiLayer::effectiveScale();
    ImGui::SetNextWindowSize(
        ImVec2(std::min(540.0f * tv, viewport.x * 0.86f),
               std::min(600.0f * tv, viewport.y * 0.84f)), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("File details##media", nullptr,
                               ImGuiWindowFlags_NoCollapse)) {
        renderMediaDetails();
        if (ImGui::Button("Back", ImVec2(-1.0f, 42.0f * tv)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
            m_showMediaDetailsPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::renderMediaDetails() {
    auto* dialog = ImGuiFileDialog::Instance();
    if (!m_showMediaDetailsPopup) {
        const std::string highlighted = dialog->GetHighlightedFilePath();
        if (!highlighted.empty()) m_mediaDetailsPath = highlighted;
        else {
            const auto selection = dialog->GetSelection();
            if (!selection.empty()) m_mediaDetailsPath = selection.begin()->second;
        }
        const std::string directory = dialog->GetCurrentPath();
        if (!m_mediaDetailsPath.empty() &&
            std::filesystem::u8path(m_mediaDetailsPath).parent_path() !=
                std::filesystem::u8path(directory))
            m_mediaDetailsPath.clear();
    }

    ImGui::TextUnformatted("DETAILS");
    ImGui::Separator();
    if (m_mediaDetailsPath.empty()) {
        ImGui::TextWrapped("Select a file to see its playback history.");
        return;
    }

    const std::string name =
        std::filesystem::u8path(m_mediaDetailsPath).filename().u8string();
    ImGui::TextWrapped("%s", name.c_str());
    ImGui::Spacing();
    WatchHistoryStore::Entry history;
    const bool hasHistory = WatchHistoryStore::find(m_mediaDetailsPath, &history);
    const double duration = hasHistory ? history.duration : 0.0;
    const double fraction = duration > 0.0
        ? std::clamp(history.furthestPosition / duration, 0.0, 1.0) : 0.0;
    const bool started = hasHistory && history.furthestPosition >= 60.0 &&
                         fraction >= 0.02;
    ImGui::TextUnformatted(!hasHistory || (!started && !history.watched)
        ? "Not watched" : history.watched ? "Watched" : "In progress");
    if (hasHistory && (started || history.watched)) {
        char label[40];
        if (duration > 0.0) {
            std::snprintf(label, sizeof(label), "%d%% reached",
                          static_cast<int>(std::lround(fraction * 100.0)));
            ImGui::ProgressBar(static_cast<float>(fraction),
                               ImVec2(-1.0f, ImGui::GetTextLineHeight() * 1.3f), label);
        } else if (history.manuallyWatched) {
            ImGui::TextDisabled("Marked as watched manually");
        }
        if (duration > 0.0) {
            ImGui::Text("Last position: %s / %s",
                        resumeTimeLabel(history.position).c_str(),
                        resumeTimeLabel(duration).c_str());
            ImGui::Text("Remaining: %s",
                        resumeTimeLabel(std::max(0.0, duration - history.position)).c_str());
        }
    }
    if (hasHistory && history.lastPlayedAt > 0) {
        const auto timestamp = static_cast<std::time_t>(history.lastPlayedAt);
        char date[64] = {};
        if (const std::tm* local = std::localtime(&timestamp))
            std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", local);
        ImGui::Text("Last played: %s", date);
    }
    const std::uint64_t bytes = plat::fileSize(m_mediaDetailsPath);
    if (bytes > 0) {
        const char* unit = "B";
        double amount = static_cast<double>(bytes);
        for (const char* next : {"KB", "MB", "GB", "TB"}) {
            if (amount < 1000.0) break;
            amount /= 1000.0;
            unit = next;
        }
        ImGui::Text("Size: %.2f %s", amount, unit);
    }
    ImGui::TextWrapped("%s", m_mediaDetailsPath.c_str());
    ImGui::Separator();

    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 42.0f * ImGuiLayer::effectiveScale();
    ResumeStore::Entry resume;
    const bool canResume = ResumeStore::find(m_mediaDetailsPath, &resume);
    if (canResume && ImGui::Button("Resume playback", ImVec2(width, height))) {
        m_mediaDialogRequestedPath = m_mediaDetailsPath;
        m_mediaDialogRequestedAutoResume = true;
    }
    if (ImGui::Button("Play from beginning", ImVec2(width, height))) {
        m_mediaDialogRequestedPath = m_mediaDetailsPath;
        m_mediaDialogRequestedStartOver = true;
    }
    ImGui::Spacing();
    if (ImGui::Button(hasHistory && history.watched ? "Mark as unwatched" :
                      "Mark as watched", ImVec2(width, height))) {
        const bool watched = !(hasHistory && history.watched);
        if (WatchHistoryStore::setWatched(m_mediaDetailsPath, watched)) {
            ResumeStore::clear(m_mediaDetailsPath);
            if (m_mediaFileFilter != 0) dialog->RefreshFileFilter();
        }
    }
}

void Application::openFileDialog() {
    m_mediaDetailsPath.clear();
    m_showMediaDetailsPopup = false;
    m_mediaFileFilter = 0;
    IGFD::FileDialogConfig cfg;
    cfg.path  = Settings::fileDialogStartDirectory();
    // CaseInsensitiveExtentionFiltering is critical: without it the
    // dialog drops files whose on-disk extension casing differs from
    // the filter (e.g. "Movie.MKV" doesn't match ".mkv").  That
    // accounts for the missing-files reports on Windows where capture
    // tools and rippers tend to emit uppercase extensions.
    cfg.flags = ImGuiFileDialogFlags_Modal |
                ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering |
                ImGuiFileDialogFlags_HideColumnType |
                ImGuiFileDialogFlags_HideColumnDate;
    cfg.userDatas = this;
    cfg.fileStatus = [](const IGFD::FileInfos* info, IGFD::UserDatas user) {
        static_cast<Application*>(user)->renderMediaFileStatus(info);
    };
    cfg.fileFilter = [](const IGFD::FileInfos* info, IGFD::UserDatas user) {
        return static_cast<Application*>(user)->mediaFileVisible(info);
    };
    cfg.fileToolbar = [](IGFD::UserDatas user) {
        return static_cast<Application*>(user)->renderMediaFileToolbar();
    };
    cfg.filePopup = [](IGFD::UserDatas user) {
        static_cast<Application*>(user)->renderMediaDetailsPopup();
    };
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    const float tv = ImGuiLayer::effectiveScale();
    m_mediaInlineDetails = work.x >= 900.0f * tv;
    if (m_mediaInlineDetails) {
        cfg.sidePaneWidth = std::clamp(work.x * 0.26f,
                                       220.0f * tv, 500.0f * tv);
        cfg.sidePane = [](const char*, IGFD::UserDatas user, bool*) {
            static_cast<Application*>(user)->renderMediaDetails();
        };
    }
    ImGuiFileDialog::Instance()->OpenDialog(
        "open_media",
        "Open media file",
        "Video/Audio{.mkv,.mp4,.avi,.webm,.mov,.m4v,"
                    ".ts,.m2ts,.flac,.wav,.mp3,.opus,.ogg,.aac,.ac3,.thd},"
        "All files{.*}",
        cfg);
}

// Big-button TV-mode landing.  Drawn instead of the docked panels
// whenever there's no media loaded.  Four buttons, gamepad-navigable
// thanks to ImGuiConfigFlags_NavEnableGamepad already being on.
void Application::renderHomeScreen() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos  = vp->WorkPos;
    ImVec2 size = vp->WorkSize;

    // Pull the window to the front and request focus the first time
    // the home screen appears (app start, or after a file finishes /
    // is closed).  Without this, ImGui's nav system has no target and
    // the gamepad d-pad / left stick can't move the focus cursor onto
    // the buttons.  NoBringToFrontOnFocus was previously set here for
    // the same reason it has to be removed: it suppressed nav routing.
    if (m_homeFreshlyShown) ImGui::SetNextWindowFocus();

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##home", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);

    // Block geometry: logo + title + subtitle + 4 big buttons.  Centred
    // both axes so the layout stays tidy on whatever viewport size the host
    // window happens to be.
    const float tv         = ImGuiLayer::effectiveScale();
    const float desiredBtn = 180.0f * tv;
    const float btnGap     = 24.0f * tv;
    const float titleScale = 2.4f;
    const float subScale   = 1.1f;

    const float logoH     = m_logoTex ? 208.0f * tv : 0.0f;
    const float logoGap   = m_logoTex ? 10.0f  * tv : 0.0f;
    const float titleH    = ImGui::GetFontSize() * titleScale;
    const float subH      = ImGui::GetFontSize() * subScale;
    // Tiles use a responsive grid.  Four columns are preferred, but at a
    // narrow aspect ratio (or a very high Comfort setting) they become a
    // 2x2 grid before they can leave the viewport.
    const float sideMargin = 32.0f * tv;
    const float fourColMax = (size.x - sideMargin * 2.0f - btnGap * 3.0f) / 4.0f;
    const int columns = fourColMax >= desiredBtn * 0.72f ? 4 : 2;
    const int rows = 4 / columns;
    const float widthLimited = (size.x - sideMargin * 2.0f -
                                btnGap * (columns - 1)) / columns;
    const float btnSize = std::max(1.0f,
                                   std::min(desiredBtn, widthLimited));
    const float tilesH = btnSize * rows + btnGap * (rows - 1);
    const float blockH = logoH + logoGap + titleH + 12.0f * tv + subH +
                         36.0f * tv + tilesH;
    const float yStart    = (size.y - blockH) * 0.5f;

    // Logo, above the wordmark.
    if (m_logoTex) {
        const float w = logoH * (m_logoH > 0 ? (float)m_logoW / (float)m_logoH : 1.0f);
        ImGui::SetCursorPos(ImVec2((size.x - w) * 0.5f, yStart));
        ImGui::Image((ImTextureID)(intptr_t)m_logoTex, ImVec2(w, logoH));
    }

    // Wordmark — "MPV" in the accent, " Sofa" in plain ink, centred as one
    // unit.  Same split the style lab uses for the brand.
    const float wordmarkY = yStart + logoH + logoGap;
    {
        const char* head = "MPV";
        const char* tail = " Sofa";
        ImGui::PushFont(ImGuiLayer::fonts().display,
                        ImGui::GetStyle().FontSizeBase * titleScale);
        const ImVec2 hs = ImGui::CalcTextSize(head);
        const ImVec2 ts = ImGui::CalcTextSize(tail);
        ImGui::SetCursorPos(ImVec2((size.x - (hs.x + ts.x)) * 0.5f, wordmarkY));
        ImGui::TextColored(ImGuiLayer::accentColor(), "%s", head);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(tail);
        ImGui::PopFont();
    }

    // Subtitle
    {
        const char* sub = "Spatial audio cinema for headphones";
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * subScale);
        ImVec2 ts = ImGui::CalcTextSize(sub);
        ImGui::SetCursorPos(ImVec2((size.x - ts.x) * 0.5f,
                                    wordmarkY + titleH + 12.0f * tv));
        ImGui::TextDisabled("%s", sub);
        ImGui::PopFont();

        // The Polaroid spectrum stripe, centred under the wordmark.  Drawn
        // blue→red like the film packaging; the active accent is the one
        // segment at full height so the theme choice is visible at a glance.
        const int   segs = 6;
        const float stripeW = 200.0f * tv, segW = stripeW / segs;
        const float sx = pos.x + (size.x - stripeW) * 0.5f;
        const float sy = pos.y + wordmarkY + titleH + 12.0f * tv + subH +
                         12.0f * tv;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int active = Settings::appearanceConfig().accent;
        for (int i = 0; i < segs; i++) {
            const int idx = segs - 1 - i;            // blue … red
            ImVec4 col = ImGuiLayer::accentPreview(idx,
                                                   Settings::appearanceConfig().mode);
            const float h = ((idx == active) ? 5.0f : 3.0f) * tv;
            if (idx != active) col.w = 0.55f;
            dl->AddRectFilled(ImVec2(sx + segW * i, sy),
                              ImVec2(sx + segW * (i + 1), sy + h),
                              ImGui::GetColorU32(col));
        }
    }

    // 4 buttons
    {
        const float totalW = btnSize * columns + btnGap * (columns - 1);
        const float xStart = (size.x - totalW) * 0.5f;
        const float yButtons = wordmarkY + titleH + 12.0f * tv + subH +
                               36.0f * tv;

        ImGui::SetCursorPos(ImVec2(xStart, yButtons));

        if (homeButton(ICON_LC_FOLDER_OPEN, "Open", btnSize))
            openFileDialog();
        // Seed the nav cursor on the first button when home has just
        // become visible.  FocusItem() (internal API) is stronger than
        // SetItemDefaultFocus(): the latter only takes effect on the
        // first appearance of the window and is silently ignored when
        // an unrelated window (e.g. the main menu bar drawn just above)
        // already owns NavWindow, which is exactly what was happening
        // here — A/B fired on whatever menu bar item nav had latched
        // onto, the d-pad moved focus around inside the menu bar where
        // nothing was visible, and the home buttons looked unresponsive.
        if (m_homeFreshlyShown) ImGui::FocusItem();
        if (columns > 1) ImGui::SameLine(0.0f, btnGap);
        if (homeButton(ICON_LC_HISTORY,     "Recent", btnSize))
            m_showRecentDialog = true;
        if (columns == 2) {
            ImGui::SetCursorPos(ImVec2(xStart, yButtons + btnSize + btnGap));
        } else {
            ImGui::SameLine(0.0f, btnGap);
        }
        if (homeButton(ICON_LC_SETTINGS,    "Settings", btnSize)) {
            if (m_prefsDialog) m_prefsDialog->open();
        }
        ImGui::SameLine(0.0f, btnGap);
        if (homeButton(ICON_LC_DOOR_OPEN,   "Exit", btnSize))
            m_running = false;
    }

    // ImGui hides the nav cursor by default until the user issues a
    // directional input.  On a TV-mode landing where the d-pad / stick
    // *is* the primary input, that initial-hidden state means the user
    // sees no focus indicator and assumes nav is broken.  Force it on.
    ImGui::SetNavCursorVisible(true);

    m_homeFreshlyShown = false;
    ImGui::End();
}

// In-playback overlay menu: same shape as the home tiles, drawn on top
// of the (paused) video so the user can pop into Open / Recent /
// Settings / Exit without losing their seat in cinema mode.
void Application::openPlaybackMenu() {
    if (m_playbackMenuOpen) return;
    m_playbackMenuOpen         = true;
    m_playbackMenuFreshlyShown = true;
    if (m_player && !m_player->isPaused()) {
        m_player->pause();
        m_playbackMenuPausedUs = true;
    } else {
        m_playbackMenuPausedUs = false;
    }
}

void Application::closePlaybackMenu(bool forcePlay) {
    if (!m_playbackMenuOpen) return;
    m_playbackMenuOpen = false;
    if ((forcePlay || m_playbackMenuPausedUs) && m_player) {
        // Resume explicitly.  forcePlay is used by the visible Resume action
        // and Space; Esc/B preserve a pause that existed before the menu.
        m_player->play();
    }
    m_playbackMenuPausedUs = false;
}

// Transport bindings for gamepad and keyboard.
//
// Called from renderUI() rather than processInput() because ImGui::IsKeyPressed
// only reports this frame's edges after NewFrame(); processInput() runs before
// it and would read the previous frame.  The gamepad keys themselves come from
// imgui_impl_glfw's own poll inside NewFrame — deliberately not re-written here,
// since a second writer racing it is what broke d-pad navigation once before.
//
// Suppressed while Preferences or any popup is up so the same buttons keep
// driving menu navigation, and while the on-screen menu is open so A activates
// the focused tile instead of toggling playback underneath it.
void Application::handlePlaybackShortcuts(bool prefsOpen, bool anyPopup) {
    if (!m_player || prefsOpen || anyPopup || m_playbackMenuOpen) return;

    // Any transport input counts as activity: in cinema mode the bar and
    // cursor hide on a timer that only mouse motion used to refresh, which
    // left the transport unreachable for a gamepad-only user.
    auto poke = [this]() { m_fullscreenCursorTimer = 3.0f; };

    // When visible UI owns the nav cursor, A/Space belongs exclusively to
    // the focused widget. Previously the same A press both activated a
    // ±10-second button and fired this global play/pause shortcut, changing
    // playback state and stealing focus for the central control.
    const ImGuiContext* imgui = ImGui::GetCurrentContext();
    const bool controlsVisible = m_videoFullscreen
        ? m_fullscreenCursorTimer > 0.0f
        : m_transportVisible;
    const bool focusedWidgetOwnsActivate =
        controlsVisible && imgui && imgui->NavId != 0;
    const bool play = !focusedWidgetOwnsActivate && (
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||   // A
        ImGui::IsKeyPressed(ImGuiKey_Space,           false));
    if (play) {
        const bool pausing = !m_player->isPaused();
        m_player->togglePause();
        poke();
        if (pausing && m_transportBar) {
            // Make the state change visually obvious to a gamepad user and
            // leave the next A/Enter press on the central transport control.
            m_transportVisible = true;
            m_transportIdleTimer = 0.0f;
            m_transportBar->requestPlayPauseFocus();
        }
    }

    // Seek: triggers for the coarse jump, d-pad left/right for the fine one.
    // Repeat is allowed on the triggers so holding one scrubs.
    struct { ImGuiKey key; double secs; bool repeat; } kSeeks[] = {
        {ImGuiKey_GamepadL2,        -10.0, true},
        {ImGuiKey_GamepadR2,         10.0, true},
        {ImGuiKey_LeftArrow,         -5.0, true},
        {ImGuiKey_RightArrow,         5.0, true},
    };
    for (const auto& s : kSeeks) {
        if (ImGui::IsKeyPressed(s.key, s.repeat)) { m_player->seekRelative(s.secs); poke(); }
    }

    // Chapters on the bumpers.  Falls back to a long seek when the file has
    // none, so the buttons never feel dead.
    const bool haveChapters = !m_player->getChapters().empty();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        haveChapters ? m_player->prevChapter() : m_player->seekRelative(-60.0);
        poke();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
        haveChapters ? m_player->nextChapter() : m_player->seekRelative(60.0);
        poke();
    }

    // Volume on the d-pad's vertical axis, mute on Y.
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        m_player->setVolume(m_player->getVolume() + 5.0); poke();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        m_player->setVolume(m_player->getVolume() - 5.0); poke();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {   // Y
        m_player->toggleMute(); poke();
    }

    // X opens the track picker — the one thing you reach for mid-film that
    // isn't transport.
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) && m_trackPicker) {
        m_trackPicker->open();
        poke();
    }
}

void Application::renderPlaybackMenu() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos  = vp->WorkPos;
    ImVec2 size = vp->WorkSize;

    if (m_playbackMenuFreshlyShown) ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    // Dim the underlying video without fully hiding it — the user
    // needs to remember where they were so the cinema feel survives
    // the menu pop.  0.85 backdrop alpha matches the rest of the
    // app's modal styling.
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##playback_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);

    const float tv         = ImGuiLayer::effectiveScale();
    const float desiredBtn = 180.0f * tv;
    const float btnGap     = 24.0f * tv;
    const float titleScale = 1.6f;
    constexpr int tileCount = 5;

    const float titleH    = ImGui::GetFontSize() * titleScale;
    const float sideMargin = 32.0f * tv;
    const float usableW = std::max(1.0f, size.x - sideMargin * 2.0f);
    const int columns = std::clamp(
        (int)((usableW + btnGap) / (desiredBtn * 0.72f + btnGap)),
        1, tileCount);
    const int rows = (tileCount + columns - 1) / columns;
    const float widthLimited = (size.x - sideMargin * 2.0f -
                                btnGap * (columns - 1)) / columns;
    const float btnSize = std::max(1.0f,
                                   std::min(desiredBtn, widthLimited));
    const float tilesH = btnSize * rows + btnGap * (rows - 1);
    const float blockH = titleH + 36.0f * tv + tilesH;
    const float yStart    = (size.y - blockH) * 0.5f;

    // Subtitle hint at the top of the block.
    {
        const char* hint = "Paused — choose Resume or press Esc";
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * titleScale);
        ImVec2 ts = ImGui::CalcTextSize(hint);
        ImGui::SetCursorPos(ImVec2((size.x - ts.x) * 0.5f, yStart));
        ImGui::TextDisabled("%s", hint);
        ImGui::PopFont();
    }

    // Responsive action grid.  Resume is a real first-class action rather
    // than relying on the hidden transport beneath this overlay.
    const float totalW   = btnSize * columns + btnGap * (columns - 1);
    const float xStart   = (size.x - totalW) * 0.5f;
    const float yButtons = yStart + titleH + 36.0f * tv;

    const char* icons[tileCount] = {
        ICON_LC_PLAY, ICON_LC_FOLDER_OPEN, ICON_LC_HISTORY,
        ICON_LC_SETTINGS, ICON_LC_DOOR_OPEN
    };
    const char* labels[tileCount] = {
        "Resume", "Open", "Recent", "Settings", "Exit"
    };
    for (int i = 0; i < tileCount; ++i) {
        const int row = i / columns;
        const int col = i % columns;
        const int rowBegin = row * columns;
        const int rowCount = std::min(columns, tileCount - rowBegin);
        const float rowW = btnSize * rowCount + btnGap * (rowCount - 1);
        const float rowX = (size.x - rowW) * 0.5f;
        ImGui::SetCursorPos(ImVec2(rowX + col * (btnSize + btnGap),
                                  yButtons + row * (btnSize + btnGap)));
        const bool clicked = homeButton(icons[i], labels[i], btnSize);
        if (m_playbackMenuFreshlyShown && i == 0) ImGui::FocusItem();
        if (!clicked) continue;

        switch (i) {
            case 0: closePlaybackMenu(/*forcePlay=*/true); break;
            case 1: closePlaybackMenu(); openFileDialog(); break;
            case 2: m_showRecentDialog = true; break;
            case 3: if (m_prefsDialog) m_prefsDialog->open(); break;
            case 4: m_running = false; break;
            default: break;
        }
    }

    m_playbackMenuFreshlyShown = false;
    ImGui::End();
}

bool Application::homeButton(const char* icon, const char* label, float size) {
    ImGui::PushID(label);
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // ImGuiButtonFlags_EnableNav is required for InvisibleButton to
    // participate in gamepad / keyboard directional navigation.
    // Without it, InvisibleButton calls ItemAdd with ImGuiItemFlags_NoNav
    // (see imgui_widgets.cpp:846), so the d-pad can't move focus
    // between the home tiles even though the inputs reach ImGui.
    //
    // InvisibleButton also unconditionally calls RenderNavCursor() when
    // focused (imgui_widgets.cpp:851), which paints ImGui's default
    // dotted/inset focus rectangle on top of our tile.  We already draw
    // a thicker custom ring below to indicate focus, so suppress the
    // auto cursor by pushing a fully transparent NavCursor colour.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    ImGui::InvisibleButton("##btn", ImVec2(size, size),
                            ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    const bool active  = ImGui::IsItemActivated();
    const bool emph    = hovered || focused;
    const float tv = ImGuiLayer::effectiveScale();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 accent = ImGuiLayer::accentColor();
    const ImU32 bg     = ImGui::GetColorU32(emph ? ImGuiCol_FrameBgHovered
                                                 : ImGuiCol_FrameBg);
    // Focus is the accent, not white — it's the one place the theme's
    // intense colour earns its keep, and it reads from across a room.
    const ImU32 border = emph ? ImGui::GetColorU32(accent)
                              : ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 textCol= ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 pmax(origin.x + size, origin.y + size);

    if (emph) {
        ImVec4 glow = accent;
        glow.w = 0.20f;
        dl->AddRect(ImVec2(origin.x - 5.0f * tv, origin.y - 5.0f * tv),
                    ImVec2(pmax.x + 5.0f * tv, pmax.y + 5.0f * tv),
                    ImGui::GetColorU32(glow), 15.0f * tv, 0, 7.0f * tv);
    }
    dl->AddRectFilled(origin, pmax, bg, 12.0f * tv);
    dl->AddRect(origin, pmax, border, 12.0f * tv, 0,
                (emph ? 2.5f : 1.0f) * tv);

    // PushFont takes an *unscaled* base size; passing GetFontSize() here used
    // to apply the 4K factor twice (explicitly forbidden by ImGui 1.92) and
    // made the glyph nearly as tall as the tile.  Keep it at a restrained
    // 2.2x body size and cap the final rendered height to 28% of the tile.
    const float globalScale = std::max(0.01f, ImGuiLayer::effectiveScale());
    const float iconBase = std::min(ImGui::GetStyle().FontSizeBase * 2.2f,
                                    size * 0.28f / globalScale);
    ImGui::PushFont(nullptr, iconBase);
    ImVec2 iconSz = ImGui::CalcTextSize(icon);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(origin.x + (size - iconSz.x) * 0.5f,
                       origin.y + size * 0.30f - iconSz.y * 0.5f),
                textCol, icon);
    ImGui::PopFont();

    // Label centred in the lower portion at the body font size.
    ImVec2 labelSz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(origin.x + (size - labelSz.x) * 0.5f,
                       origin.y + size * 0.74f - labelSz.y * 0.5f),
                textCol, label);

    ImGui::PopID();
    return active;
}

void Application::renderRecentDialog() {
    if (!m_showRecentDialog) return;

    ImGui::OpenPopup("##recent_dialog");

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float tv = ImGuiLayer::effectiveScale();
    ImVec2 sz(std::min(800.0f * tv, vp->Size.x * 0.8f),
              std::min(600.0f * tv, vp->Size.y * 0.8f));
    ImVec2 p(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f,
             vp->Pos.y + (vp->Size.y - sz.y) * 0.5f);
    ImGui::SetNextWindowPos(p, ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);

    bool open = true;
    if (!ImGui::BeginPopupModal("##recent_dialog", &open,
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoResize)) {
        if (!open) m_showRecentDialog = false;
        return;
    }

    ImGui::Text(ICON_LC_HISTORY "  Recent files");
    ImGui::Separator();
    ImGui::Spacing();

    const auto& list = Settings::recentFiles();
    if (list.empty()) {
        ImGui::TextDisabled("No recently played files yet.");
    } else {
        const float footerH = ImGui::GetFrameHeightWithSpacing() +
                              ImGui::GetStyle().ItemSpacing.y * 2 + 4.0f;
        ImGui::BeginChild("##rlist", ImVec2(0, -footerH), false);
        std::string picked;
        for (const auto& path : list) {
            // Display: filename in bold-ish + dim full path below.
            const auto slash = path.find_last_of("\\/");
            std::string name = (slash == std::string::npos)
                                  ? path : path.substr(slash + 1);
            ImGui::PushID(path.c_str());
            if (ImGui::Selectable(name.c_str(), false, 0,
                                   ImVec2(0, ImGui::GetFrameHeight() * 1.6f))) {
                picked = path;
            }
            ImGui::SameLine(0, 0);
            // The full path under the filename, truncated by clip rect.
            ImGui::Dummy(ImVec2(0, 0));  // restore cursor
            ImGui::SetCursorPosX(ImGui::GetStyle().ItemSpacing.x);
            ResumeStore::Entry resume;
            WatchHistoryStore::Entry history;
            const bool resumable = ResumeStore::find(path, &resume);
            const bool known = WatchHistoryStore::find(path, &history);
            if (known && history.watched && resumable) {
                ImGui::TextDisabled("%s  |  Watched  |  Resume replay at %s",
                    path.c_str(), resumeTimeLabel(resume.position).c_str());
            } else if (known && history.watched) {
                ImGui::TextDisabled("%s  |  Watched", path.c_str());
            } else if (resumable) {
                const int percent = resume.duration > 0.0
                    ? static_cast<int>(std::clamp(
                        resume.position * 100.0 / resume.duration, 0.0, 100.0))
                    : 0;
                ImGui::TextDisabled("%s  |  Continue at %s (%d%%)",
                    path.c_str(), resumeTimeLabel(resume.position).c_str(), percent);
            } else {
                ImGui::TextDisabled("%s", path.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (!picked.empty()) {
            m_pendingFile = picked;
            m_showRecentDialog = false;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::Separator();
    const float btnW = 100.0f * tv;
    if (ImGui::BeginTable("##recent_footer", 2,
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        if (ImGui::Button("Clear list")) Settings::clearRecents();
        ImGui::TableNextColumn();
        const float closeX = ImGui::GetCursorPosX() +
                             ImGui::GetContentRegionAvail().x - btnW;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), closeX));
        if (ImGui::Button("Close", ImVec2(btnW, 0))) {
            m_showRecentDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndTable();
    }

    ImGui::EndPopup();
    if (!open) m_showRecentDialog = false;
}

void Application::checkpointCurrentPlayback() {
    if (!m_player || !m_resumeTrackingReady || m_resumePromptPending ||
        m_currentMediaPath.empty()) return;
    ResumeStore::checkpoint(m_currentMediaPath,
                            m_player->getPosition(),
                            m_player->getDuration(),
                            m_player->getResumeAudioTrackId(),
                            m_player->getCurrentSubtitleTrackId(),
                            m_player->areSubtitlesVisible());
    if (m_player->hasVideo())
        WatchHistoryStore::checkpoint(m_currentMediaPath,
                                      m_player->getPosition(),
                                      m_player->getDuration(),
                                      Settings::playbackConfig().watchedThresholdPercent);
}

static std::string resumeTimeLabel(double seconds) {
    const int total = std::max(0, (int)std::llround(seconds));
    const int h = total / 3600;
    const int m = (total / 60) % 60;
    const int s = total % 60;
    char out[32];
    if (h > 0) snprintf(out, sizeof(out), "%d:%02d:%02d", h, m, s);
    else       snprintf(out, sizeof(out), "%d:%02d", m, s);
    return out;
}

void Application::renderResumeDialog() {
    if (!m_resumePromptPending || !m_player) return;
    if (m_trackPicker && m_trackPicker->isOpen()) return;

    if (m_autoResumeOnLoad) {
        m_player->seek(std::max(0.0, m_resumeEntry.position - 3.0));
        m_player->play();
        m_resumePromptPending = false;
        m_autoResumeOnLoad = false;
        return;
    }

    if (!m_resumePopupOpen) {
        m_player->pause();
        ImGui::OpenPopup("##resume_dialog");
        m_resumePopupOpen = true;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float tv = ImGuiLayer::effectiveScale();
    const ImVec2 sz(std::min(560.0f * tv, vp->Size.x * 0.86f),
                    std::min(220.0f * tv, vp->Size.y * 0.60f));
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f,
               vp->Pos.y + (vp->Size.y - sz.y) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);

    if (ImGui::BeginPopupModal("##resume_dialog", nullptr,
                               ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize)) {
        ImGui::Text(ICON_LC_HISTORY "  Continue watching?");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("You stopped at %s.",
                           resumeTimeLabel(m_resumeEntry.position).c_str());
        ImGui::Spacing();

        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (ImGui::Button("Continue", ImVec2(w, 52.0f * tv))) {
            // A short rewind restores conversational context and avoids
            // resuming in the middle of a word.
            m_player->seek(std::max(0.0, m_resumeEntry.position - 3.0));
            m_player->play();
            m_resumePromptPending = false;
            m_resumePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Start over", ImVec2(w, 52.0f * tv))) {
            ResumeStore::clear(m_currentMediaPath);
            m_player->seek(0.0);
            m_player->play();
            m_resumePromptPending = false;
            m_resumePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::shutdown() {
    checkpointCurrentPlayback();
    persistFileDialogBookmarks();
    if (ImGuiFileDialog::Instance()->IsOpened())
        Settings::setLastBrowseDirectory(
            ImGuiFileDialog::Instance()->GetCurrentPath());
    // Persist settings on clean exit if the user has touched anything.
    if (m_sharedState &&
        Settings::isDirty(m_sharedState, m_showControlPanel, m_show3DViz)) {
        Settings::save(m_sharedState, m_showControlPanel, m_show3DViz);
    }

    // Stop the media server child (the Job object would also reap it,
    // but an explicit stop keeps the exit orderly).
    if (m_mediaServer) m_mediaServer->stop();

    if (m_videoFBO) glDeleteFramebuffers(1, &m_videoFBO);
    if (m_logoTex)      glDeleteTextures(1, &m_logoTex);
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
