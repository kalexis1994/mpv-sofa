#include "Direct3DHdrPresenter.h"

#include <glad/glad.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdarg>
#include <vector>

// WGL_NV_DX_interop / interop2 — declared manually so we don't depend on a
// particular wglext.h being present. Loaded via wglGetProcAddress.
#ifndef WGL_ACCESS_WRITE_DISCARD_NV
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
#endif
typedef HANDLE(WINAPI* PFNWGLDXOPENDEVICENVPROC)(void* dxDevice);
typedef BOOL  (WINAPI* PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE(WINAPI* PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void* dxObject,
                                                     GLuint name, GLenum type, GLenum access);
typedef BOOL  (WINAPI* PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL  (WINAPI* PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE* hObjects);
typedef BOOL  (WINAPI* PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE* hObjects);

namespace {
void hlog(const char* fmt, ...) {
    if (FILE* f = fopen("hdr_debug.log", "a")) {
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
}
PFNWGLDXOPENDEVICENVPROC        p_wglDXOpenDeviceNV        = nullptr;
PFNWGLDXCLOSEDEVICENVPROC       p_wglDXCloseDeviceNV       = nullptr;
PFNWGLDXREGISTEROBJECTNVPROC    p_wglDXRegisterObjectNV    = nullptr;
PFNWGLDXUNREGISTEROBJECTNVPROC  p_wglDXUnregisterObjectNV  = nullptr;
PFNWGLDXLOCKOBJECTSNVPROC       p_wglDXLockObjectsNV       = nullptr;
PFNWGLDXUNLOCKOBJECTSNVPROC     p_wglDXUnlockObjectsNV     = nullptr;

bool loadInteropFns() {
    if (p_wglDXOpenDeviceNV) return true;
    auto load = [](const char* n) { return (void*)wglGetProcAddress(n); };
    p_wglDXOpenDeviceNV       = (PFNWGLDXOPENDEVICENVPROC)      load("wglDXOpenDeviceNV");
    p_wglDXCloseDeviceNV      = (PFNWGLDXCLOSEDEVICENVPROC)     load("wglDXCloseDeviceNV");
    p_wglDXRegisterObjectNV   = (PFNWGLDXREGISTEROBJECTNVPROC)  load("wglDXRegisterObjectNV");
    p_wglDXUnregisterObjectNV = (PFNWGLDXUNREGISTEROBJECTNVPROC)load("wglDXUnregisterObjectNV");
    p_wglDXLockObjectsNV      = (PFNWGLDXLOCKOBJECTSNVPROC)     load("wglDXLockObjectsNV");
    p_wglDXUnlockObjectsNV    = (PFNWGLDXUNLOCKOBJECTSNVPROC)   load("wglDXUnlockObjectsNV");
    return p_wglDXOpenDeviceNV && p_wglDXRegisterObjectNV &&
           p_wglDXLockObjectsNV && p_wglDXUnlockObjectsNV && p_wglDXCloseDeviceNV;
}

template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Windows' per-display "SDR content brightness" slider, in nits. This is the
// system's own calibration for how bright SDR white should appear on an HDR
// display, so the app's UI can match every other window instead of needing
// its own brightness setting. 0 on failure.
float querySdrWhiteForWindow(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return 0.0f;

    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS)
        return 0.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(),
                           &nModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return 0.0f;

    for (UINT32 i = 0; i < nPaths; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;

        // DISPLAYCONFIG_SDR_WHITE_LEVEL declared locally — the enum value
        // (11) is stable but absent from some mingw headers.
        struct {
            DISPLAYCONFIG_DEVICE_INFO_HEADER header;
            ULONG SDRWhiteLevel;   // in units of 1/1000, ×80 nits
        } wl{};
        wl.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)11;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) != ERROR_SUCCESS) continue;
        return (float)wl.SDRWhiteLevel / 1000.0f * 80.0f;
    }
    return 0.0f;
}
}  // namespace

Direct3DHdrPresenter::Direct3DHdrPresenter() {}
Direct3DHdrPresenter::~Direct3DHdrPresenter() { shutdown(); }

bool Direct3DHdrPresenter::createDevice() {
    UINT flags = 0;
#ifndef NDEBUG
    // flags |= D3D11_CREATE_DEVICE_DEBUG;  // requires the debug layer
#endif
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   want, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "[HDR] D3D11CreateDevice failed (0x%08lx)\n", hr);
        return false;
    }
    m_device = dev;
    m_context = ctx;
    return true;
}

void Direct3DHdrPresenter::queryDisplayHdr() {
    m_displayHdr = false;
    m_displayMaxNits = 0.0f;
    const HMONITOR windowMonitor = MonitorFromWindow(
        (HWND)m_hwnd, MONITOR_DEFAULTTOPRIMARY);

    float sdr = querySdrWhiteForWindow((HWND)m_hwnd);
    m_sdrWhiteNits = (sdr > 0.0f) ? sdr : 200.0f;

    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(((ID3D11Device*)m_device)->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)))
        return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        // Only use the output that currently contains the app window. Picking
        // the first HDR output would combine one monitor's peak with another
        // monitor's SDR-white setting in multi-display setups.
        IDXGIOutput* out = nullptr;
        for (UINT i = 0; adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++) {
            IDXGIOutput6* out6 = nullptr;
            if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void**)&out6))) {
                DXGI_OUTPUT_DESC1 desc{};
                if (SUCCEEDED(out6->GetDesc1(&desc))) {
                    if (desc.Monitor != windowMonitor) {
                        safeRelease(out6);
                        safeRelease(out);
                        continue;
                    }
                    // HDR is *active* only when the output is in the PQ/BT.2020
                    // colour space — i.e. Windows HDR is switched ON for this
                    // display. An HDR-capable panel with Windows HDR off still
                    // reports SDR here, which is what we want: presenting scRGB
                    // HDR into an SDR compositor would look wrong.
                    if (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                        m_displayHdr = true;
                        m_displayMaxNits = desc.MaxLuminance;
                    }
                }
                safeRelease(out6);
            }
            safeRelease(out);
            if (m_displayHdr) break;
        }
        safeRelease(adapter);
    }
    safeRelease(dxgiDev);
}

bool Direct3DHdrPresenter::createSwapchain(int width, int height) {
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (FAILED(((ID3D11Device*)m_device)->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)))
        return false;
    dxgiDev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    safeRelease(adapter);
    safeRelease(dxgiDev);
    if (!factory) return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width  = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // scRGB
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd((ID3D11Device*)m_device,
                                                 (HWND)m_hwnd, &sd, nullptr, nullptr, &sc1);
    factory->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "[HDR] CreateSwapChainForHwnd failed (0x%08lx)\n", hr);
        hlog("createSwapchain: CreateSwapChainForHwnd FAILED 0x%08lx", hr);
        return false;
    }
    hlog("createSwapchain: OK");
    IDXGISwapChain3* sc3 = nullptr;
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3);
    safeRelease(sc1);
    if (!sc3) return false;

    // scRGB colour space — the compositor maps linear FP16 to the display.
    UINT support = 0;
    if (SUCCEEDED(sc3->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    }
    m_swapchain = sc3;
    return true;
}

bool Direct3DHdrPresenter::registerInterop() {
    if (!loadInteropFns()) {
        fprintf(stderr, "[HDR] WGL_NV_DX_interop2 not available\n");
        hlog("registerInterop: WGL_NV_DX_interop2 NOT AVAILABLE");
        return false;
    }
    if (!m_glDevice) {
        m_glDevice = p_wglDXOpenDeviceNV(m_device);
        if (!m_glDevice) {
            fprintf(stderr, "[HDR] wglDXOpenDeviceNV failed\n");
            hlog("registerInterop: wglDXOpenDeviceNV FAILED (err %lu)", GetLastError());
            return false;
        }
    }

    // The swapchain backbuffer (flip model) can't be registered with GL
    // directly (FBO comes back UNSUPPORTED). Register a matching offscreen
    // D3D render target instead, render into that from GL, then CopyResource
    // it onto the backbuffer at present time.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_width; td.Height = m_height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* shared = nullptr;
    HRESULT hr = ((ID3D11Device*)m_device)->CreateTexture2D(&td, nullptr, &shared);
    if (FAILED(hr)) {
        hlog("registerInterop: CreateTexture2D shared FAILED 0x%08lx", hr);
        return false;
    }
    m_backTex = shared;   // reuse the slot for the shared RT

    glGenFramebuffers(1, &m_glFbo);
    glGenTextures(1, &m_glRbo);   // slot reused as a GL texture name

    m_glBackHandle = p_wglDXRegisterObjectNV(m_glDevice, shared, m_glRbo,
                                             GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!m_glBackHandle) {
        fprintf(stderr, "[HDR] wglDXRegisterObjectNV failed\n");
        hlog("registerInterop: wglDXRegisterObjectNV FAILED (err %lu)", GetLastError());
        return false;
    }
    // The interop texture's storage is only valid while locked, so attach
    // and validate the FBO inside a lock.
    HANDLE h = (HANDLE)m_glBackHandle;
    if (!p_wglDXLockObjectsNV(m_glDevice, 1, &h)) {
        hlog("registerInterop: initial lock FAILED (err %lu)", GetLastError());
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_glFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_glRbo, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_wglDXUnlockObjectsNV(m_glDevice, 1, &h);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[HDR] interop FBO incomplete (0x%x)\n", st);
        hlog("registerInterop: FBO incomplete 0x%x", st);
        return false;
    }
    hlog("registerInterop: OK (shared texture)");
    return true;
}

bool Direct3DHdrPresenter::createScratch(int width, int height) {
    glGenFramebuffers(1, &m_scratchFbo);
    glGenTextures(1, &m_scratchTex);
    glBindTexture(GL_TEXTURE_2D, m_scratchTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scratchFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_scratchTex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return st == GL_FRAMEBUFFER_COMPLETE;
}

void Direct3DHdrPresenter::releaseScratch() {
    if (m_scratchTex) { glDeleteTextures(1, &m_scratchTex); m_scratchTex = 0; }
    if (m_scratchFbo) { glDeleteFramebuffers(1, &m_scratchFbo); m_scratchFbo = 0; }
}

void Direct3DHdrPresenter::releaseInterop() {
    if (m_glDevice && m_glBackHandle) {
        if (p_wglDXUnregisterObjectNV) p_wglDXUnregisterObjectNV(m_glDevice, m_glBackHandle);
        m_glBackHandle = nullptr;
    }
    if (m_glRbo) { glDeleteTextures(1, &m_glRbo); m_glRbo = 0; }
    if (m_glFbo) { glDeleteFramebuffers(1, &m_glFbo); m_glFbo = 0; }
    safeRelease(*(ID3D11Texture2D**)&m_backTex);
}

bool Direct3DHdrPresenter::init(GLFWwindow* window, int width, int height) {
    if (!window || width <= 0 || height <= 0) return false;
    m_hwnd = glfwGetWin32Window(window);
    if (!m_hwnd) return false;

    m_width = width;
    m_height = height;
    if (!createDevice()) { shutdown(); return false; }
    queryDisplayHdr();
    if (!createSwapchain(width, height)) { shutdown(); return false; }
    if (!registerInterop()) { shutdown(); return false; }
    if (!createScratch(width, height)) { shutdown(); return false; }

    m_available = true;
    fprintf(stderr, "[HDR] presenter ready (%dx%d, scRGB, displayHDR=%d)\n",
            width, height, m_displayHdr ? 1 : 0);
    return true;
}

void Direct3DHdrPresenter::resize(int width, int height) {
    if (!m_available || width <= 0 || height <= 0) return;
    if (width == m_width && height == m_height) return;
    releaseScratch();
    releaseInterop();
    HRESULT hr = ((IDXGISwapChain3*)m_swapchain)->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { m_available = false; return; }
    m_width = width; m_height = height;
    if (!registerInterop() || !createScratch(width, height)) m_available = false;
}

unsigned int Direct3DHdrPresenter::beginFrame() {
    if (!m_available) return 0;
    HANDLE h = (HANDLE)m_glBackHandle;
    if (!p_wglDXLockObjectsNV(m_glDevice, 1, &h)) return 0;
    m_locked = true;
    // The app renders into the scratch FBO (normal GL orientation).
    return m_scratchFbo;
}

void Direct3DHdrPresenter::present() {
    if (!m_available || !m_locked) return;

    // Blit scratch → interop texture with the Y axis flipped, converting
    // from GL's bottom-left origin to D3D's top-left origin.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_scratchFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_glFbo);
    glBlitFramebuffer(0, 0, m_width, m_height,
                      0, m_height, m_width, 0,   // dst Y inverted → vertical flip
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    HANDLE h = (HANDLE)m_glBackHandle;
    p_wglDXUnlockObjectsNV(m_glDevice, 1, &h);
    m_locked = false;

    // Copy the GL-rendered shared texture onto the swapchain backbuffer.
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(((IDXGISwapChain3*)m_swapchain)->GetBuffer(
            0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        ((ID3D11DeviceContext*)m_context)->CopyResource(back, (ID3D11Texture2D*)m_backTex);
        back->Release();
    }
    ((IDXGISwapChain3*)m_swapchain)->Present(1, 0);
}

void Direct3DHdrPresenter::shutdown() {
    releaseScratch();
    releaseInterop();
    if (m_glDevice && p_wglDXCloseDeviceNV) { p_wglDXCloseDeviceNV(m_glDevice); m_glDevice = nullptr; }
    safeRelease(*(IDXGISwapChain3**)&m_swapchain);
    safeRelease(*(ID3D11DeviceContext**)&m_context);
    safeRelease(*(ID3D11Device**)&m_device);
    m_available = false;
}
