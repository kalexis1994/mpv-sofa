#pragma once

struct GLFWwindow;
struct ImFont;
struct ImVec4;

class ImGuiLayer {
public:
    ImGuiLayer();
    ~ImGuiLayer();

    void init(GLFWwindow* window);
    void beginFrame();
    void endFrame();
    void shutdown();

    // Push the "Polaroid" palette (design/style-lab.html) into the live
    // ImGui style, reading mode + accent from Settings.  Safe to call any
    // time — Preferences calls it when the user changes either.
    static void applyTheme();

    // The accent colour the current theme resolves to, for widgets that
    // draw their own thing (timeline fill, focus glow, spectrum bar).
    static ImVec4 accentColor();
    static const char* accentName(int index);   // index 0..5, nullptr past end

    // The colour accent `index` resolves to under `mode` (0=dark, 1=light),
    // without touching the live style — for drawing the swatch picker.
    static ImVec4 accentPreview(int index, int mode);

    // Dosis, loaded from assets/fonts.  `text` is ImGui's default font and
    // is the only one with the Lucide icons merged in — push the others
    // only around plain text.
    struct Fonts {
        ImFont* text    = nullptr;   // Medium, UI size
        ImFont* strong  = nullptr;   // SemiBold, UI size
        ImFont* title   = nullptr;   // Bold, ~1.5x
        ImFont* display = nullptr;   // Bold, ~2.2x (home screen)
    };
    static const Fonts& fonts();

private:
    bool m_initialized = false;
};
