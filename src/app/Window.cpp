#include "Window.h"
#include <GLFW/glfw3.h>
#include <cstdio>

Window::Window() = default;

Window::~Window() {
    if (m_window)
        glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool Window::init(const std::string& title, int width, int height, WindowMode mode) {
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    // Create hidden so the user doesn't see a flash of the wrong mode
    // before we apply the requested one — setMode below shows the
    // configured window.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }

    m_width  = width;
    m_height = height;
    m_savedW = width;
    m_savedH = height;
    m_mode   = WindowMode::Windowed;   // we just made a windowed window

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // VSync

    glfwSetWindowUserPointer(m_window, this);
    glfwSetDropCallback(m_window, glfwDropCallback);
    glfwSetFramebufferSizeCallback(m_window, glfwResizeCallback);

    if (mode != WindowMode::Windowed) {
        setMode(mode);
    }
    glfwShowWindow(m_window);

    // Force the new window to take keyboard / mouse focus.  Under the
    // Windows GUI subsystem (no console parent) the OS sometimes
    // shows the window without giving it the foreground focus, so the
    // first user click would land "outside" any control and the OS
    // would just snap focus on click — the visible flicker the user
    // saw on the first interaction.  Explicit focus + RequestAttention
    // covers both immediate-after-launch and the case where the
    // launcher (Steam, shortcut, ...) has already grabbed focus.
    glfwFocusWindow(m_window);
    glfwRequestWindowAttention(m_window);

    return true;
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::swapBuffers() {
    glfwSwapBuffers(m_window);
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window);
}

void Window::setMode(WindowMode mode) {
    if (!m_window || mode == m_mode) return;

    // Save windowed geometry on the way out of Windowed, so a later
    // swap back lands at the same place / size the user had.
    if (m_mode == WindowMode::Windowed) {
        glfwGetWindowPos (m_window, &m_savedX, &m_savedY);
        glfwGetWindowSize(m_window, &m_savedW, &m_savedH);
    }

    GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* vidmode = glfwGetVideoMode(monitor);
    int monX = 0, monY = 0;
    glfwGetMonitorPos(monitor, &monX, &monY);

    switch (mode) {
        case WindowMode::Fullscreen: {
            // Exclusive fullscreen — GLFW reattaches to the monitor
            // and applies the native vidmode.  Decoration / resizable
            // are reset for the eventual round-trip back to Windowed.
            glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowAttrib(m_window, GLFW_RESIZABLE, GLFW_TRUE);
            glfwSetWindowMonitor(m_window, monitor,
                                 0, 0,
                                 vidmode->width, vidmode->height,
                                 vidmode->refreshRate);
            break;
        }
        case WindowMode::Borderless: {
            // Borderless windowed: detach from the monitor (nullptr),
            // strip decorations + resize blocking, and snap to monitor
            // origin at full vidmode size.  Behaves identically to
            // Fullscreen for the user, but Alt-Tab / overlays / window
            // managers stay snappy because it's a regular OS window.
            glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowAttrib(m_window, GLFW_RESIZABLE, GLFW_FALSE);
            glfwSetWindowMonitor(m_window, nullptr,
                                 monX, monY,
                                 vidmode->width, vidmode->height, 0);
            break;
        }
        case WindowMode::Windowed: {
            glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowAttrib(m_window, GLFW_RESIZABLE, GLFW_TRUE);
            int w = m_savedW > 0 ? m_savedW : 1600;
            int h = m_savedH > 0 ? m_savedH : 900;
            int x = m_savedX > 0 ? m_savedX : (monX + (vidmode->width  - w) / 2);
            int y = m_savedY > 0 ? m_savedY : (monY + (vidmode->height - h) / 2);
            glfwSetWindowMonitor(m_window, nullptr, x, y, w, h, 0);
            break;
        }
    }

    m_mode = mode;
    if (mode != WindowMode::Fullscreen) m_savedNonFsMode = mode;
}

void Window::toggleFullscreen() {
    if (m_mode == WindowMode::Fullscreen) {
        // Restore the user's preferred non-fullscreen mode.
        setMode(m_savedNonFsMode);
    } else {
        setMode(WindowMode::Fullscreen);
    }
}

void Window::glfwDropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_dropCallback && count > 0) {
        self->m_dropCallback(paths[0]);
    }
}

void Window::glfwResizeCallback(GLFWwindow* window, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->m_width  = width;
        self->m_height = height;
    }
}
