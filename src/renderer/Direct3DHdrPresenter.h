#pragma once

// HDR presentation for the desktop app.
//
// The whole app renders through OpenGL (GLFW), and OpenGL on Windows can't
// present an HDR swapchain. So instead of migrating the renderer, the final
// composited frame is handed to a Direct3D 11 flip-model swapchain via
// WGL_NV_DX_interop2 and presented through DXGI in an HDR colour space.
//
// scRGB (R16G16B16A16_FLOAT, linear, 1.0 = 80 nits, Rec.709 primaries with
// out-of-range values allowed) is used because it's the friendliest target
// to composite mixed HDR-video + SDR-UI content into.
//
// The whole thing is additive and fail-safe: if any step (no HDR display,
// no interop extension, device creation failure) doesn't succeed, available()
// stays false and the caller keeps using the plain glfwSwapBuffers SDR path.

#include <cstdint>

struct GLFWwindow;

class Direct3DHdrPresenter {
public:
    Direct3DHdrPresenter();
    ~Direct3DHdrPresenter();

    // Set up D3D11 + the interop swapchain for a GLFW window's HWND.
    // Returns false (and leaves available() false) on any failure.
    bool init(GLFWwindow* window, int width, int height);
    void shutdown();

    bool available() const { return m_available; }

    // Whether the output display currently reports HDR capability
    // (DXGI advertises an HDR colour space + >SDR peak luminance).
    bool displayIsHdr() const { return m_displayHdr; }
    float displayMaxNits() const { return m_displayMaxNits; }

    // Recreate swapchain buffers after a window resize.
    void resize(int width, int height);

    // Begin a frame: locks the D3D backbuffer for GL and returns the GL
    // framebuffer object to composite the final image into. Returns 0 if
    // unavailable (caller falls back to the default framebuffer).
    unsigned int beginFrame();

    // Unlock the shared buffer and present through DXGI. No-op if the
    // matching beginFrame() returned 0.
    void present();

    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    bool createDevice();
    bool createSwapchain(int width, int height);
    void queryDisplayHdr();
    bool registerInterop();
    void releaseInterop();

    bool  m_available = false;
    bool  m_displayHdr = false;
    float m_displayMaxNits = 0.0f;
    int   m_width = 0, m_height = 0;
    bool  m_locked = false;

    void* m_hwnd = nullptr;
    // Opaque D3D/DXGI/GL-interop handles (kept as void* so the header
    // doesn't drag in d3d11.h / dxgi.h across the whole project).
    void* m_device = nullptr;        // ID3D11Device*
    void* m_context = nullptr;       // ID3D11DeviceContext*
    void* m_swapchain = nullptr;     // IDXGISwapChain3*
    void* m_backTex = nullptr;       // ID3D11Texture2D* (current backbuffer)
    void* m_glDevice = nullptr;      // HANDLE from wglDXOpenDeviceNV
    void* m_glBackHandle = nullptr;  // HANDLE from wglDXRegisterObjectNV
    unsigned int m_glFbo = 0;      // FBO wrapping the interop texture
    unsigned int m_glRbo = 0;      // interop GL texture name
    // Scratch FBO the app renders into (normal GL bottom-left origin); its
    // contents are blit-flipped into the interop texture at present, fixing
    // the GL(bottom-left) vs D3D(top-left) vertical inversion.
    unsigned int m_scratchFbo = 0;
    unsigned int m_scratchTex = 0;

    bool createScratch(int width, int height);
    void releaseScratch();
};
