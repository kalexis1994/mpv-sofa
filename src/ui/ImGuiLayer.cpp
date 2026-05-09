#include "ImGuiLayer.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <IconsLucide.h>

ImGuiLayer::ImGuiLayer() = default;

ImGuiLayer::~ImGuiLayer() {
    shutdown();
}

// Custom palette (user-supplied):
//   #1B3C53  deep navy   — main backgrounds
//   #234C6A  mid navy    — frames, sliders, headers
//   #456882  steel blue  — hover/active accents
//   #D2C1B6  sand        — text, icons, key highlights
static ImVec4 hex(unsigned rgb, float a = 1.0f) {
    float r = ((rgb >> 16) & 0xff) / 255.0f;
    float g = ((rgb >>  8) & 0xff) / 255.0f;
    float b = ( rgb        & 0xff) / 255.0f;
    return ImVec4(r, g, b, a);
}

static void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry — modern, slightly rounded but not bubble-y.
    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 4.0f;

    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.TabBorderSize     = 0.0f;

    s.WindowPadding     = ImVec2(10, 8);
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 6);
    s.IndentSpacing     = 18.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;

    // Palette
    const ImVec4 deep   = hex(0x1B3C53);
    const ImVec4 mid    = hex(0x234C6A);
    const ImVec4 steel  = hex(0x456882);
    const ImVec4 sand   = hex(0xD2C1B6);
    const ImVec4 dim    = hex(0xD2C1B6, 0.55f);
    const ImVec4 deeper = hex(0x12283A);
    const ImVec4 line   = hex(0x456882, 0.45f);

    auto* c = s.Colors;
    c[ImGuiCol_Text]                  = sand;
    c[ImGuiCol_TextDisabled]          = dim;
    c[ImGuiCol_WindowBg]              = deep;
    c[ImGuiCol_ChildBg]               = deep;
    c[ImGuiCol_PopupBg]               = ImVec4(deeper.x, deeper.y, deeper.z, 0.96f);
    c[ImGuiCol_Border]                = line;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = mid;
    c[ImGuiCol_FrameBgHovered]        = steel;
    c[ImGuiCol_FrameBgActive]         = ImVec4(steel.x * 1.15f, steel.y * 1.15f, steel.z * 1.15f, 1.0f);

    c[ImGuiCol_TitleBg]               = deeper;
    c[ImGuiCol_TitleBgActive]         = mid;
    c[ImGuiCol_TitleBgCollapsed]      = deeper;

    c[ImGuiCol_MenuBarBg]             = deeper;

    c[ImGuiCol_ScrollbarBg]           = deeper;
    c[ImGuiCol_ScrollbarGrab]         = mid;
    c[ImGuiCol_ScrollbarGrabHovered]  = steel;
    c[ImGuiCol_ScrollbarGrabActive]   = sand;

    c[ImGuiCol_CheckMark]             = sand;
    c[ImGuiCol_SliderGrab]            = sand;
    c[ImGuiCol_SliderGrabActive]      = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    c[ImGuiCol_Button]                = mid;
    c[ImGuiCol_ButtonHovered]         = steel;
    c[ImGuiCol_ButtonActive]          = sand;

    c[ImGuiCol_Header]                = mid;
    c[ImGuiCol_HeaderHovered]         = steel;
    c[ImGuiCol_HeaderActive]          = steel;

    c[ImGuiCol_Separator]             = line;
    c[ImGuiCol_SeparatorHovered]      = steel;
    c[ImGuiCol_SeparatorActive]       = sand;

    c[ImGuiCol_ResizeGrip]            = ImVec4(steel.x, steel.y, steel.z, 0.4f);
    c[ImGuiCol_ResizeGripHovered]     = steel;
    c[ImGuiCol_ResizeGripActive]      = sand;

    c[ImGuiCol_Tab]                   = mid;
    c[ImGuiCol_TabHovered]            = steel;
    c[ImGuiCol_TabActive]             = steel;
    c[ImGuiCol_TabUnfocused]          = deeper;
    c[ImGuiCol_TabUnfocusedActive]    = mid;

    c[ImGuiCol_DockingPreview]        = ImVec4(steel.x, steel.y, steel.z, 0.7f);
    c[ImGuiCol_DockingEmptyBg]        = deeper;

    c[ImGuiCol_PlotLines]             = sand;
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c[ImGuiCol_PlotHistogram]         = steel;
    c[ImGuiCol_PlotHistogramHovered]  = sand;

    c[ImGuiCol_TableHeaderBg]         = deeper;
    c[ImGuiCol_TableBorderStrong]     = line;
    c[ImGuiCol_TableBorderLight]      = ImVec4(line.x, line.y, line.z, 0.25f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.03f);

    c[ImGuiCol_TextSelectedBg]        = ImVec4(steel.x, steel.y, steel.z, 0.6f);
    c[ImGuiCol_DragDropTarget]        = sand;
    c[ImGuiCol_NavHighlight]          = sand;
    c[ImGuiCol_NavWindowingHighlight] = sand;
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.4f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.45f);
}

static void mergeIconFont() {
    ImGuiIO& io = ImGui::GetIO();
    // Default font first so the merged icon font lands inside its glyph
    // table and shares the line-height.
    io.Fonts->AddFontDefault();

    static const ImWchar icon_ranges[] = { ICON_MIN_LC, ICON_MAX_16_LC, 0 };
    ImFontConfig cfg;
    cfg.MergeMode        = true;
    cfg.PixelSnapH       = true;
    // Slight Y nudge so the line-icons sit visually on the text baseline.
    cfg.GlyphOffset.y    = 1.0f;
    // Lucide outlines render best at a hair under the body font.
    const float iconSize = 14.0f;
    io.Fonts->AddFontFromFileTTF("assets/fonts/lucide.ttf",
                                  iconSize, &cfg, icon_ranges);
}

void ImGuiLayer::init(GLFWwindow* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    mergeIconFont();
    ImGui::StyleColorsDark();
    applyTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 410");

    m_initialized = true;
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void ImGuiLayer::shutdown() {
    if (!m_initialized) return;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
}
