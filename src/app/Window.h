#pragma once

#include <functional>
#include <string>

struct GLFWwindow;

class Window {
public:
    Window();
    ~Window();

    bool init(const std::string& title, int width, int height);
    void pollEvents();
    void swapBuffers();
    bool shouldClose() const;

    GLFWwindow* getHandle() const { return m_window; }
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    void toggleFullscreen();
    bool isFullscreen() const { return m_fullscreen; }

    using DropCallback = std::function<void(const char* path)>;
    void setDropCallback(DropCallback cb) { m_dropCallback = cb; }

private:
    GLFWwindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_fullscreen = false;
    int m_windowedX = 0, m_windowedY = 0;
    int m_windowedWidth = 0, m_windowedHeight = 0;
    DropCallback m_dropCallback;

    static void glfwDropCallback(GLFWwindow* window, int count, const char** paths);
    static void glfwResizeCallback(GLFWwindow* window, int width, int height);
};
