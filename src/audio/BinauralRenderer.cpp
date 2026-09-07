#include "BinauralRenderer.h"

#include "platform/Platform.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

using plat::cacheDir;
using plat::exists;
using plat::fileMtime;
using plat::fileSize;
using plat::removeFile;
using plat::touchFile;

// FNV-1a over the inputs → stable cache key.
std::string hashKey(const std::string& movie, int aidx,
                    const std::string& sofa, int room, uint64_t mtime) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    };
    auto mixi = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) { h ^= (v & 0xff); h *= 1099511628211ull; v >>= 8; }
    };
    mix(movie); mixi((uint64_t)aidx); mix(sofa); mixi((uint64_t)room); mixi(mtime);
    char b[24];
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return b;
}

// Blocking run, for the short extraction step.
int runHidden(const std::vector<std::string>& argv, const std::string& stderrPath) {
    plat::Proc h = plat::spawn(argv, std::string(), stderrPath);
    if (!h) return -1;
    const int rc = plat::waitExit(h);
    plat::closeProc(h);
    return rc;
}

// Last chunk of a (small) log file, flattened to one line — appended to
// the user-facing error so "failed" says why.
std::string tailOf(const std::string& path, size_t maxBytes = 240) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long from = sz > (long)maxBytes ? sz - (long)maxBytes : 0;
    fseek(f, from, SEEK_SET);
    std::string s(sz - from, '\0');
    size_t got = fread(&s[0], 1, s.size(), f);
    fclose(f);
    s.resize(got);
    for (char& c : s) if (c == '\r' || c == '\n') c = ' ';
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

}  // namespace

BinauralRenderer::BinauralRenderer() {}

BinauralRenderer::~BinauralRenderer() {
    cancel();
    if (m_thread.joinable()) m_thread.join();
}

// `name` is the bare tool name; the platform suffix is appended here so call
// sites don't carry ".exe" around.
std::string BinauralRenderer::toolPath(const char* name) {
    const std::string exe   = std::string(name) + plat::exeSuffix();
    const std::string local = plat::exeDir() + exe;
    if (exists(local)) return local;
    // Dev fallback: hrtf-build has truehdd-bin/, ffmpeg-build/bin/, etc.
    return exe;   // rely on PATH
}

std::string BinauralRenderer::resultPath() {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_resultPath;
}

std::string BinauralRenderer::error() {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_error;
}

void BinauralRenderer::killChildren() {
    std::lock_guard<std::mutex> lk(m_procMutex);
    for (void*& h : m_children) {
        if (h) plat::terminate((plat::Proc)h);
        // Handle close happens in the worker that owns them.
    }
}

void BinauralRenderer::cancel() {
    m_cancel.store(true);
    m_generation.fetch_add(1);
    killChildren();
    m_state.store(State::Idle);
    m_renderedSec.store(0.0);
}

bool BinauralRenderer::request(const std::string& moviePath, int audioStreamIndex,
                               const std::string& sofaPath, int roomPreset,
                               double durationSec, Solo solo) {
    // The solo mode folds into roomPreset's slot of the hash by offsetting
    // it far outside the preset range, keeping each mix its own cache entry.
    const std::string key = hashKey(moviePath, audioStreamIndex, sofaPath,
                                    roomPreset + 1000 * (int)solo,
                                    fileMtime(moviePath));
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // Already serving / building this exact request → no-op.
        if (m_currentKey == key &&
            (m_state.load() == State::Ready || m_state.load() == State::Rendering))
            return true;
    }

    const std::string outPcm = cacheDir() + key + ".pcm";

    // Cache hit needs the completion marker: a bare .pcm is a render that
    // was interrupted (crash, exit mid-movie) and must be redone.
    if (exists(outPcm) && exists(outPcm + ".ok")) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_currentKey = key;
        m_resultPath = outPcm;
        m_renderedSec.store(fileSize(outPcm) / kBytesPerSecond);
        m_state.store(State::Ready);
        m_progress.store(1.0f);
        return true;
    }

    // Fresh render on a background thread.
    if (m_thread.joinable()) { m_cancel.store(true); killChildren(); m_thread.join(); }
    m_cancel.store(false);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_currentKey = key;
        m_resultPath.clear();
        m_error.clear();
    }
    m_progress.store(0.0f);
    m_renderedSec.store(0.0);
    m_state.store(State::Rendering);

    const std::string workPrefix = cacheDir() + key + "_work";
    std::string movie = moviePath, sofa = sofaPath;
    int aidx = audioStreamIndex, room = roomPreset;
    double dur = durationSec;
    m_thread = std::thread([this, movie, aidx, sofa, room, outPcm, workPrefix, dur, solo]() {
        runChain(movie, aidx, sofa, room, outPcm, workPrefix, dur, solo);
    });
    return true;
}

void BinauralRenderer::runChain(std::string movie, int aidx, std::string sofa,
                                int room, std::string outPcm,
                                std::string workPrefix, double durationSec,
                                Solo solo) {
    const std::string ffmpeg  = toolPath("ffmpeg");
    const std::string truehdd = toolPath("truehdd");
    const std::string render  = toolPath("halosound-render");

    const std::string thd       = workPrefix + ".thd";
    const std::string damfAudio = workPrefix + ".atmos.audio";
    const std::string damfDone  = workPrefix + ".done";
    const std::string logX      = workPrefix + ".extract.log";
    const std::string logD      = workPrefix + ".truehdd.log";
    const std::string logR      = workPrefix + ".render.log";

    auto cleanupWork = [&]() {
        removeFile(thd);
        removeFile(workPrefix + ".atmos");
        removeFile(damfAudio);
        removeFile(workPrefix + ".atmos.metadata");
        removeFile(damfDone);
    };
    // On failure the tool logs stay on disk next to the cache for a look;
    // the message carries the tail so the UI already says why.
    auto fail = [&](std::string msg, const std::string& logPath = std::string()) {
        if (!logPath.empty()) {
            std::string t = tailOf(logPath);
            if (!t.empty()) msg += " — " + t;
        }
        fprintf(stderr, "[Binaural] FAILED: %s\n", msg.c_str());
        cleanupWork();
        removeFile(outPcm);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_error = msg;
        m_renderedSec.store(0.0);
        m_state.store(State::Failed);
    };

    // Stale leftovers from an interrupted run would trip the follow reader.
    removeFile(damfDone);
    removeFile(outPcm);
    removeFile(outPcm + ".ok");

    // 1) Extract the raw TrueHD elementary stream. Quick (a remux), so a
    //    blocking run keeps the error handling simple.
    m_progress.store(0.02f);
    int rc = runHidden({ffmpeg, "-v", "error", "-y", "-i", movie,
                        "-map", "0:" + std::to_string(aidx),
                        "-c:a", "copy", "-f", "truehd", thd}, logX);
    if (m_cancel.load()) { fail("cancelled"); return; }
    if (rc != 0 || !exists(thd)) { fail("truehd extraction failed", logX); return; }

    // 2+3) truehdd and the render run TOGETHER, the render tailing the DAMF
    //      via --follow exactly like the TV server does, its f32 stdout
    //      redirected straight into the sidecar. The sidecar becomes
    //      playable while both are still running — that's the whole point.
    plat::Proc hDecode = plat::spawn({truehdd, "decode", thd,
                                      "--output-path", workPrefix,
                                      "--loglevel", "error"},
                                     std::string(), logD);
    if (!hDecode) { fail("could not start truehdd"); return; }

    std::vector<std::string> renderArgv = {render, "--sofa", sofa,
                                           "--room", std::to_string(room),
                                           "--damf", workPrefix, "--follow"};
    if (solo == Solo::Bed)     { renderArgv.push_back("--solo"); renderArgv.push_back("bed"); }
    if (solo == Solo::Objects) { renderArgv.push_back("--solo"); renderArgv.push_back("objects"); }
    plat::Proc hRender = plat::spawn(renderArgv, outPcm, logR);
    if (!hRender) {
        plat::terminate(hDecode); plat::closeProc(hDecode);
        fail("could not start render");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_procMutex);
        m_children[0] = hDecode;
        m_children[1] = hRender;
    }
    {
        // The file exists from here on; expose it so the player can hot-swap
        // once enough of it is rendered.
        std::lock_guard<std::mutex> lk(m_mutex);
        m_resultPath = outPcm;
    }

    // Progress loop: rendered seconds = sidecar size / byterate. truehdd's
    // exit is what writes the .done marker the follow reader waits for —
    // the same contract the server uses. A failed decode never writes it,
    // which would leave the follow reader tailing forever, so that case
    // kills the render instead of waiting.
    bool decodeDone = false, decodeFailed = false;
    for (;;) {
        if (m_cancel.load()) break;

        int code = 1;
        if (!decodeDone && plat::tryWait(hDecode, 0, &code)) {
            decodeDone = true;
            if (code != 0) { decodeFailed = true; plat::terminate(hRender); }
            else touchFile(damfDone);
        }

        m_renderedSec.store(fileSize(outPcm) / kBytesPerSecond);
        if (durationSec > 0.0) {
            float p = (float)(m_renderedSec.load() / durationSec);
            m_progress.store(p < 0.99f ? p : 0.99f);
        }

        // Doubles as the loop's pacing sleep and its exit condition.
        if (plat::tryWait(hRender, 250, nullptr)) break;
    }

    {
        std::lock_guard<std::mutex> lk(m_procMutex);
        m_children[0] = m_children[1] = nullptr;
    }

    if (m_cancel.load()) {
        plat::terminate(hDecode);
        plat::terminate(hRender);
        plat::closeProc(hDecode); plat::closeProc(hRender);
        fail("cancelled");
        return;
    }

    int renderCode = 1;
    plat::tryWait(hRender, 0, &renderCode);
    if (!decodeDone) {
        // Render exited first — with --follow that only happens on error.
        plat::terminate(hDecode);
        decodeFailed = true;
    }
    plat::closeProc(hDecode); plat::closeProc(hRender);

    if (decodeFailed) { fail("truehdd decode failed", logD); return; }
    if (renderCode != 0 || fileSize(outPcm) == 0) { fail("binaural render failed", logR); return; }

    cleanupWork();
    removeFile(logX);
    removeFile(logD);
    removeFile(logR);
    touchFile(outPcm + ".ok");

    std::lock_guard<std::mutex> lk(m_mutex);
    m_renderedSec.store(fileSize(outPcm) / kBytesPerSecond);
    m_progress.store(1.0f);
    m_state.store(State::Ready);
}
