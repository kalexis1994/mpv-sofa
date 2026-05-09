#pragma once

#include <functional>
#include <string>

struct GLFWwindow;

// Three OS-level window modes, modelled on the AAA-game convention:
//   Fullscreen  — exclusive fullscreen on the primary monitor
//   Borderless  — decorationless window sized to the monitor work area
//   Windowed    — regular bordered, resizable window
// The runtime "video fullscreen" toggle (F11) is a *separate* concept
// that hides UI panels for cinema viewing — the OS-level mode here is
// the user's persistent preference.
enum class WindowMode {
    Fullscreen = 0,
    Borderless = 1,
    Windowed   = 2,
};

class Window {
public:
    Window();
    ~Window();

    // mode picks how the window is presented on first show.  Default
    // fullscreen because the app is designed for TV-mode use; the
    // launcher / Preferences page swap in the user's saved choice
    // before we get here.
    bool init(const std::string& title, int width, int height,
              WindowMode mode = WindowMode::Fullscreen);

    void pollEvents();
    void swapBuffers();
    bool shouldClose() const;

    GLFWwindow* getHandle() const { return m_window; }
    int getWidth()  const { return m_width; }
    int getHeight() const { return m_height; }

    // Apply a new mode at runtime — used when Preferences toggles the
    // setting.  No-op when the requested mode matches the current one.
    void       setMode(WindowMode mode);
    WindowMode getMode() const { return m_mode; }

    // Cinema-toggle entry point: switch between Fullscreen and the most
    // recent non-fullscreen mode (Borderless or Windowed).  Wired to
    // F11 in Application.
    void toggleFullscreen();
    bool isFullscreen() const { return m_mode == WindowMode::Fullscreen; }

    using DropCallback = std::function<void(const char* path)>;
    void setDropCallback(DropCallback cb) { m_dropCallback = cb; }

private:
    GLFWwindow* m_window = nullptr;
    int m_width  = 0;
    int m_height = 0;

    WindowMode m_mode             = WindowMode::Windowed;
    WindowMode m_savedNonFsMode   = WindowMode::Windowed;

    // Last known windowed geometry — used to restore the window's
    // position and size when transitioning out of Fullscreen /
    // Borderless back to Windowed.
    int m_savedX = 0,      m_savedY = 0;
    int m_savedW = 1600,   m_savedH = 900;

    DropCallback m_dropCallback;

    static void glfwDropCallback(GLFWwindow* window, int count, const char** paths);
    static void glfwResizeCallback(GLFWwindow* window, int width, int height);
};
