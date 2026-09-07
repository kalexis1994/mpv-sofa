#include "platform/Platform.h"
#include "ImGuiLayer.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <IconsLucide.h>

#include "core/Settings.h"

#include <cstdio>

ImGuiLayer::ImGuiLayer() = default;

ImGuiLayer::~ImGuiLayer() {
    shutdown();
}

// ---------------------------------------------------------------------------
// The "Polaroid" theme — see design/style-lab.html for the full exploration.
//
// Two neutral sets, both deliberately *warm* rather than the usual blue-grey:
// a darkroom black for the dark mode and a paper/cardboard tone for the light
// one.  On top of that sits one accent from the Polaroid spectrum stripe
// (blue / green / yellow / orange / red), which is where all the intensity in
// the UI comes from — everything else stays quiet so the video doesn't have
// to compete with the chrome.
// ---------------------------------------------------------------------------

static ImVec4 hex(unsigned rgb, float a = 1.0f) {
    float r = ((rgb >> 16) & 0xff) / 255.0f;
    float g = ((rgb >>  8) & 0xff) / 255.0f;
    float b = ( rgb        & 0xff) / 255.0f;
    return ImVec4(r, g, b, a);
}

static ImVec4 withAlpha(const ImVec4& c, float a) {
    return ImVec4(c.x, c.y, c.z, a);
}

static ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

namespace {

struct Neutrals {
    unsigned bg, bg2, surface, surface2, line, ink, ink2, ink3;
};

// Dark: "darkroom".  Light: "daylight" (photos on a table).
const Neutrals kDark  = { 0x100E0D, 0x1A1614, 0x221D1A, 0x2C2521,
                          0x3A312C, 0xF5EFE4, 0xB3A79A, 0x7A6F65 };
const Neutrals kLight = { 0xE3D9C8, 0xDED3C0, 0xFDFBF6, 0xF4EEE3,
                          0xDCD2C2, 0x191512, 0x5C5248, 0x8B8075 };

struct AccentDef { const char* name; unsigned dark; unsigned light; };

// The light-mode variants are deepened: the dark-mode colours are tuned to
// glow against near-black and wash out badly on paper.
const AccentDef kAccents[] = {
    { "Rojo",     0xFF2E4D, 0xE01B38 },
    { "Naranja",  0xFF8A1F, 0xD9700D },
    { "Amarillo", 0xFFD426, 0xB98A00 },
    { "Verde",    0x2FC24D, 0x1E9C39 },
    { "Azul",     0x0F8FE8, 0x0A72BE },
    { "Magenta",  0xFF3D9A, 0xDB1F7C },
};
constexpr int kAccentCount = (int)(sizeof(kAccents) / sizeof(kAccents[0]));

ImVec4 g_accent = hex(0xFF2E4D);
ImGuiLayer::Fonts g_fonts;

}  // namespace

const char* ImGuiLayer::accentName(int index) {
    return (index >= 0 && index < kAccentCount) ? kAccents[index].name : nullptr;
}

ImVec4 ImGuiLayer::accentColor() { return g_accent; }

ImVec4 ImGuiLayer::accentPreview(int index, int mode) {
    const int i = (index < 0 || index >= kAccentCount) ? 0 : index;
    return hex(mode == 1 ? kAccents[i].light : kAccents[i].dark);
}

const ImGuiLayer::Fonts& ImGuiLayer::fonts() { return g_fonts; }

void ImGuiLayer::applyTheme() {
    const Settings::AppearanceConfig& a = Settings::appearanceConfig();
    const bool light = (a.mode == 1);
    const int  ai    = (a.accent < 0 || a.accent >= kAccentCount) ? 0 : a.accent;

    // Start from a stock base so any colour we don't name below still lands
    // on the right side of the light/dark divide.
    if (light) ImGui::StyleColorsLight(); else ImGui::StyleColorsDark();

    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry.  Rounder and roomier than the old theme — the style lab's
    // controls are pill-shaped, and at desktop sizes that reads as friendly
    // rather than toy-like.
    s.WindowRounding    = 10.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 7.0f;
    s.PopupRounding     = 10.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 7.0f;
    s.TabRounding       = 7.0f;

    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 1.0f;   // the hairline that defines cards/pills
    s.PopupBorderSize   = 1.0f;
    s.TabBorderSize     = 0.0f;

    s.WindowPadding     = ImVec2(14, 12);
    s.FramePadding      = ImVec2(11, 7);
    s.ItemSpacing       = ImVec2(9, 8);
    s.ItemInnerSpacing  = ImVec2(7, 6);
    s.IndentSpacing     = 20.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 12.0f;

    const Neutrals& n = light ? kLight : kDark;
    const ImVec4 bg       = hex(n.bg);
    const ImVec4 bg2      = hex(n.bg2);
    const ImVec4 surface  = hex(n.surface);
    const ImVec4 surface2 = hex(n.surface2);
    const ImVec4 line     = hex(n.line);
    const ImVec4 ink      = hex(n.ink);
    const ImVec4 ink2     = hex(n.ink2);
    const ImVec4 ink3     = hex(n.ink3);

    const ImVec4 accent = hex(light ? kAccents[ai].light : kAccents[ai].dark);
    const ImVec4 accentHi = mix(accent, hex(0xFFFFFF), 0.18f);
    g_accent = accent;

    auto* c = s.Colors;
    c[ImGuiCol_Text]                  = ink;
    c[ImGuiCol_TextDisabled]          = ink3;
    c[ImGuiCol_WindowBg]              = bg;
    c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]               = withAlpha(surface, 0.98f);
    c[ImGuiCol_Border]                = line;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = surface;
    c[ImGuiCol_FrameBgHovered]        = surface2;
    c[ImGuiCol_FrameBgActive]         = mix(surface2, accent, 0.22f);

    c[ImGuiCol_TitleBg]               = bg2;
    c[ImGuiCol_TitleBgActive]         = bg2;
    c[ImGuiCol_TitleBgCollapsed]      = bg2;

    c[ImGuiCol_MenuBarBg]             = bg2;

    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = surface2;
    c[ImGuiCol_ScrollbarGrabHovered]  = mix(surface2, accent, 0.35f);
    c[ImGuiCol_ScrollbarGrabActive]   = accent;

    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_SliderGrab]            = accent;
    c[ImGuiCol_SliderGrabActive]      = accentHi;

    c[ImGuiCol_Button]                = surface;
    c[ImGuiCol_ButtonHovered]         = surface2;
    c[ImGuiCol_ButtonActive]          = accent;

    c[ImGuiCol_Header]                = withAlpha(accent, 0.18f);
    c[ImGuiCol_HeaderHovered]         = withAlpha(accent, 0.30f);
    c[ImGuiCol_HeaderActive]          = withAlpha(accent, 0.45f);

    c[ImGuiCol_Separator]             = line;
    c[ImGuiCol_SeparatorHovered]      = withAlpha(accent, 0.6f);
    c[ImGuiCol_SeparatorActive]       = accent;

    c[ImGuiCol_ResizeGrip]            = withAlpha(line, 0.6f);
    c[ImGuiCol_ResizeGripHovered]     = withAlpha(accent, 0.5f);
    c[ImGuiCol_ResizeGripActive]      = accent;

    c[ImGuiCol_Tab]                   = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered]            = withAlpha(accent, 0.22f);
    c[ImGuiCol_TabActive]             = surface;
    c[ImGuiCol_TabUnfocused]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabUnfocusedActive]    = surface;

    c[ImGuiCol_DockingPreview]        = withAlpha(accent, 0.55f);
    c[ImGuiCol_DockingEmptyBg]        = bg;

    c[ImGuiCol_PlotLines]             = accent;
    c[ImGuiCol_PlotLinesHovered]      = accentHi;
    c[ImGuiCol_PlotHistogram]         = accent;
    c[ImGuiCol_PlotHistogramHovered]  = accentHi;

    c[ImGuiCol_TableHeaderBg]         = bg2;
    c[ImGuiCol_TableBorderStrong]     = line;
    c[ImGuiCol_TableBorderLight]      = withAlpha(line, 0.35f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = light ? ImVec4(0, 0, 0, 0.025f)
                                              : ImVec4(1, 1, 1, 0.03f);

    c[ImGuiCol_TextSelectedBg]        = withAlpha(accent, 0.35f);
    c[ImGuiCol_DragDropTarget]        = accent;
    c[ImGuiCol_NavHighlight]          = accent;
    c[ImGuiCol_NavWindowingHighlight] = accent;
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.45f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55f);

    (void)ink2;   // reserved for secondary-text call sites

    // Global UI scale, applied last.
    //
    // Safe to do here — and only here — because everything above re-derives
    // the style from scratch every call (StyleColorsDark/Light resets the
    // colours, and every geometry field is assigned explicitly).  ScaleAllSizes
    // multiplies in place, so applying it to a style that had already been
    // scaled would compound; this way the scale is always relative to the
    // freshly-built base, and Preferences can call applyTheme() on every
    // slider tick without drift.
    const float scale = (a.uiScale > 0.5f && a.uiScale < 4.0f) ? a.uiScale : 1.0f;
    if (scale != 1.0f)
        s.ScaleAllSizes(scale);
    // Fonts scale separately: ScaleAllSizes only touches geometry, and the
    // app pushes explicit per-call sizes (title, display) that derive from
    // FontSizeBase, so scaling the base carries them along too.
    s.FontScaleMain = scale;
}

// ---------------------------------------------------------------------------
// Fonts
//
// Dosis (SIL OFL, assets/fonts) is the UI typeface: a rounded humanist sans
// that carries the Polaroid warmth without going novelty.  It only covers
// Latin, so Segoe UI is merged underneath for Greek / Cyrillic — track
// titles, chapter names and file paths can be anything.
//
// Centring Lucide against the loaded base font: ImGui anchors merged glyphs
// on the text baseline, but Lucide fills the EM box from baseline upward.  We
// read the base font's ascent/descent live (newer ImGui keeps them on
// ImFontBaked) and shift the icons by (iconMid − textMid) so changing fonts
// or sizes re-centres automatically.
// ---------------------------------------------------------------------------

static const ImWchar kLatinRanges[] = {
    0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement (ñ, é, ç, …)
    0x0100, 0x017F,   // Latin Extended-A
    0x0180, 0x024F,   // Latin Extended-B
    0x2010, 0x205F,   // General Punctuation (ellipsis, dashes, …)
    0,
};
static const ImWchar kFallbackRanges[] = {
    0x0370, 0x03FF,   // Greek
    0x0400, 0x04FF,   // Cyrillic
    0,
};
static const ImWchar kIconRanges[] = { ICON_MIN_LC, ICON_MAX_16_LC, 0 };

// Path to a system font with broad Greek/Cyrillic coverage, used only as a
// merge fallback behind Dosis.  Windows always has Segoe UI; on Linux there is
// no single guaranteed face, so probe the usual suspects rather than pulling
// in fontconfig for one lookup.  Returning nullptr just means those scripts
// render as boxes, which is what the Linux build did before.
static const char* systemFallbackFontPath() {
#ifdef _WIN32
    return "C:/Windows/Fonts/segoeui.ttf";
#else
    static const char* kCandidates[] = {
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
    };
    for (const char* p : kCandidates) {
        if (plat::exists(p)) return p;
    }
    return nullptr;
#endif
}

// Assets live next to the binary in a build tree and under share/ in an
// install, so never reach for them with a bare relative path.
static std::string assetFont(const char* name) {
    return plat::assetsDir() + "fonts/" + name;
}

// Load one Dosis weight at `size`, merging the non-Latin fallback and,
// optionally, the icon set.  Returns nullptr when the file isn't there.
static ImFont* loadUiFont(const char* path, float size, bool withIcons) {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* f = io.Fonts->AddFontFromFileTTF(path, size, nullptr, kLatinRanges);
    if (!f) return nullptr;

    if (const char* fallback = systemFallbackFontPath()) {
        ImFontConfig fb;
        fb.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(fallback, size, &fb, kFallbackRanges);
    }

    if (withIcons) {
        ImFontBaked* baked = f->GetFontBaked(f->LegacySize);
        const float iconSize = size;
        const float textMid  = (baked->Ascent + baked->Descent) * 0.5f;

        ImFontConfig cfg;
        cfg.MergeMode        = true;
        cfg.PixelSnapH       = true;
        cfg.GlyphOffset.y    = iconSize * 0.5f - textMid;
        cfg.GlyphMinAdvanceX = iconSize;
        io.Fonts->AddFontFromFileTTF(assetFont("lucide.ttf").c_str(), iconSize,
                                     &cfg, kIconRanges);
    }
    return f;
}

static void setupFonts() {
    // Dosis runs small for its nominal size (short x-height), so the base is
    // a couple of points above the usual 15.
    const float sz = 17.0f;

    g_fonts.text    = loadUiFont(assetFont("Dosis-Medium.ttf").c_str(),   sz,          true);
    g_fonts.strong  = loadUiFont(assetFont("Dosis-SemiBold.ttf").c_str(), sz,          false);
    g_fonts.title   = loadUiFont(assetFont("Dosis-Bold.ttf").c_str(),      sz * 1.5f,  false);
    // ExtraBold for the wordmark: Dosis is a light-boned face, and at Bold
    // the logo still reads as regular weight next to the UI text.
    g_fonts.display = loadUiFont(assetFont("Dosis-ExtraBold.ttf").c_str(), sz * 2.2f,  false);
    if (!g_fonts.display) g_fonts.display = g_fonts.title;

    if (g_fonts.text) return;

    // Dosis missing (assets not deployed next to the exe): fall back to the
    // system font so the UI still has full glyph coverage, then ProggyClean.
    fprintf(stderr, "[UI] Dosis not found in assets/fonts — using Segoe UI\n");
    ImGuiIO& io = ImGui::GetIO();
    ImFont* base = nullptr;
    if (const char* fallback = systemFallbackFontPath())
        base = io.Fonts->AddFontFromFileTTF(fallback, 15.0f, nullptr, kLatinRanges);
    if (!base) base = io.Fonts->AddFontDefault();

    ImFontBaked* baked = base->GetFontBaked(base->LegacySize);
    ImFontConfig cfg;
    cfg.MergeMode        = true;
    cfg.PixelSnapH       = true;
    cfg.GlyphOffset.y    = 8.0f - (baked->Ascent + baked->Descent) * 0.5f;
    cfg.GlyphMinAdvanceX = 16.0f;
    io.Fonts->AddFontFromFileTTF(assetFont("lucide.ttf").c_str(), 16.0f,
                                 &cfg, kIconRanges);
    g_fonts.text = base;
    g_fonts.strong = g_fonts.title = g_fonts.display = base;
}

void ImGuiLayer::init(GLFWwindow* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // ImGui defaults to writing imgui.ini into the working directory, which
    // for a packaged install is wherever the launcher happened to start us —
    // read-only in the worst case, and litter in the best.  Static because
    // ImGui keeps the pointer rather than copying the string.
    static const std::string iniPath = plat::configDir() + "imgui.ini";
    io.IniFilename = iniPath.c_str();

    // Init the backends BEFORE configuring fonts.  The OpenGL backend
    // sets ImGuiBackendFlags_RendererHasTextures during its Init(), and
    // any font loading / size variants requested later will silently
    // call ImFontAtlas::Build() if that flag isn't yet set — flooding
    // the console with "Called ImFontAtlas::Build() before
    // ImGuiBackendFlags_RendererHasTextures got set!" errors and
    // skipping the dynamic-atlas path the new ImGui expects.
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 410");

    setupFonts();
    applyTheme();

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
