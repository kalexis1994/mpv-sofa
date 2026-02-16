#include "Window.h"
#include <GLFW/glfw3.h>
#include <cstdio>

Window::Window() = default;

Window::~Window() {
    if (m_window)
        glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool Window::init(const std::string& title, int width, int height) {
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }

    m_width = width;
    m_height = height;

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // VSync

    glfwSetWindowUserPointer(m_window, this);
    glfwSetDropCallback(m_window, glfwDropCallback);
    glfwSetFramebufferSizeCallback(m_window, glfwResizeCallback);

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

void Window::toggleFullscreen() {
    if (!m_window) return;

    if (!m_fullscreen) {
        // Save windowed position and size
        glfwGetWindowPos(m_window, &m_windowedX, &m_windowedY);
        glfwGetWindowSize(m_window, &m_windowedWidth, &m_windowedHeight);

        // Switch to fullscreen on the current monitor
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(m_window, monitor, 0, 0,
                             mode->width, mode->height, mode->refreshRate);
    } else {
        // Restore windowed mode
        glfwSetWindowMonitor(m_window, nullptr,
                             m_windowedX, m_windowedY,
                             m_windowedWidth, m_windowedHeight, 0);
    }
    m_fullscreen = !m_fullscreen;
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
        self->m_width = width;
        self->m_height = height;
    }
}
