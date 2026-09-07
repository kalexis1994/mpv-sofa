// Picks the HDR presenter for this platform, and provides the do-nothing one
// used where there isn't (yet) a native HDR path.

#include "IHdrPresenter.h"

#ifdef _WIN32
#include "Direct3DHdrPresenter.h"
#endif

namespace {

// Reports itself unavailable, which every call site already treats as "use the
// plain SDR swap path".  Keeping it a real object rather than a null pointer
// means callers never grow a null check they'd have to keep in sync.
class NullHdrPresenter final : public IHdrPresenter {
public:
    bool init(GLFWwindow*, int, int) override { return false; }
    void shutdown() override {}
    bool  available() const override { return false; }
    bool  displayIsHdr() const override { return false; }
    float displayMaxNits() const override { return 0.0f; }
    float sdrWhiteNits() const override { return 200.0f; }
    void refreshDisplayHdr() override {}
    void resize(int, int) override {}
    unsigned int beginFrame() override { return 0; }
    void present() override {}
    int width() const override { return 0; }
    int height() const override { return 0; }
};

}  // namespace

std::unique_ptr<IHdrPresenter> createHdrPresenter() {
#ifdef _WIN32
    return std::make_unique<Direct3DHdrPresenter>();
#else
    // Linux gets the Vulkan presenter once it lands; until then the SDR path
    // is the whole story and gamescope tone-maps if a HDR display is attached.
    return std::make_unique<NullHdrPresenter>();
#endif
}
