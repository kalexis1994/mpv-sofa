#pragma once

struct GLFWwindow;

class ImGuiLayer {
public:
    ImGuiLayer();
    ~ImGuiLayer();

    void init(GLFWwindow* window);
    void beginFrame();
    void endFrame();
    void shutdown();

private:
    bool m_initialized = false;
};
