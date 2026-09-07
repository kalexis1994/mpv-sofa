#pragma once

// HDR presentation, abstracted over the platform's swapchain API.
//
// The app renders everything through OpenGL, and no OpenGL context can present
// an HDR swapchain on either platform we care about.  So the final composited
// frame is handed to a native swapchain instead: D3D11 via WGL_NV_DX_interop2
// on Windows, Vulkan via external-memory interop on Linux.  Both do the same
// thing — hand back a GL framebuffer to render into, then present it in an HDR
// colour space — which is what this interface captures.
//
// Every implementation is additive and fail-safe.  If anything at all doesn't
// line up (display isn't in HDR mode, the interop extension is missing, device
// creation fails), available() stays false, beginFrame() returns 0, and the
// caller keeps using the plain glfwSwapBuffers SDR path.  That fallback is the
// normal case, not an error path, so it must stay cheap and silent.

#include <memory>

struct GLFWwindow;

class IHdrPresenter {
public:
    virtual ~IHdrPresenter() = default;

    // Set up the native device and swapchain for a GLFW window.
    // Returns false (leaving available() false) on any failure.
    virtual bool init(GLFWwindow* window, int width, int height) = 0;
    virtual void shutdown() = 0;

    virtual bool available() const = 0;

    // Whether the output display is currently in HDR mode.  Cheap to re-check
    // via refreshDisplayHdr(), because the user can toggle it while we run.
    virtual bool  displayIsHdr() const = 0;
    virtual float displayMaxNits() const = 0;

    // The system's calibrated brightness for SDR white on this display, in
    // nits.  The UI is scaled to it so it matches every other window.
    virtual float sdrWhiteNits() const = 0;

    virtual void refreshDisplayHdr() = 0;
    virtual void resize(int width, int height) = 0;

    // Begin a frame: returns the GL framebuffer object to composite into, or
    // 0 when unavailable (caller falls back to the default framebuffer).
    virtual unsigned int beginFrame() = 0;

    // Present the frame.  No-op if the matching beginFrame() returned 0.
    virtual void present() = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
};

// Build the best presenter this platform can offer.  Never returns null: when
// there is no HDR path at all it returns one that simply reports itself
// unavailable, so callers never need a null check on top of available().
std::unique_ptr<IHdrPresenter> createHdrPresenter();
