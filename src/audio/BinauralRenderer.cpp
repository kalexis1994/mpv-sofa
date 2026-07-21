#include "BinauralRenderer.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

std::string exeDirLocal() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash + 1);
}

bool exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

std::string cacheDir() {
    char tmp[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "mpv-sofa-binaural\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

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

uint64_t fileMtime(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
            fad.ftLastWriteTime.dwLowDateTime;
}

// Run a command line (through cmd.exe so pipes work), hidden, blocking.
// Returns the process exit code, or -1 on spawn failure.
int runHidden(const std::string& cmdline) {
    std::string full = "cmd.exe /c " + cmdline;
    std::vector<char> buf(full.begin(), full.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

std::string q(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

BinauralRenderer::BinauralRenderer() {}

BinauralRenderer::~BinauralRenderer() {
    cancel();
    if (m_thread.joinable()) m_thread.join();
}

std::string BinauralRenderer::toolPath(const char* exe) {
    std::string local = exeDirLocal() + exe;
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

void BinauralRenderer::cancel() {
    m_cancel.store(true);
    m_generation.fetch_add(1);
    m_state.store(State::Idle);
}

bool BinauralRenderer::request(const std::string& moviePath, int audioStreamIndex,
                               const std::string& sofaPath, int roomPreset) {
    const std::string ffmpeg  = toolPath("ffmpeg.exe");
    const std::string truehdd = toolPath("truehdd.exe");
    const std::string render  = toolPath("halosound-render.exe");

    const std::string key = hashKey(moviePath, audioStreamIndex, sofaPath,
                                    roomPreset, fileMtime(moviePath));
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // Already serving / building this exact request → no-op.
        if (m_currentKey == key &&
            (m_state.load() == State::Ready || m_state.load() == State::Rendering))
            return true;
    }

    const std::string outWav = cacheDir() + key + ".wav";

    // Cache hit: done instantly.
    if (exists(outWav)) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_currentKey = key;
        m_resultPath = outWav;
        m_state.store(State::Ready);
        m_progress.store(1.0f);
        return true;
    }

    // Fresh render on a background thread.
    if (m_thread.joinable()) { m_cancel.store(true); m_thread.join(); }
    m_cancel.store(false);
    const uint64_t gen = m_generation.load();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_currentKey = key;
        m_resultPath.clear();
        m_error.clear();
    }
    m_progress.store(0.0f);
    m_state.store(State::Rendering);

    const std::string workPrefix = cacheDir() + key + "_work";
    std::string movie = moviePath, sofa = sofaPath;
    int aidx = audioStreamIndex, room = roomPreset;
    (void)gen;
    m_thread = std::thread([this, movie, aidx, sofa, room, outWav, workPrefix]() {
        runChain(movie, aidx, sofa, room, outWav, workPrefix, 0.0);
    });
    return true;
}

void BinauralRenderer::runChain(std::string movie, int aidx, std::string sofa,
                                int room, std::string outWav,
                                std::string workPrefix, double) {
    const std::string ffmpeg  = toolPath("ffmpeg.exe");
    const std::string truehdd = toolPath("truehdd.exe");
    const std::string render  = toolPath("halosound-render.exe");

    const std::string thd  = workPrefix + ".thd";
    const std::string tmpWav = outWav + ".part";

    auto fail = [&](const std::string& msg) {
        DeleteFileA(thd.c_str());
        DeleteFileA(tmpWav.c_str());
        std::lock_guard<std::mutex> lk(m_mutex);
        m_error = msg;
        m_state.store(State::Failed);
    };

    // 1) Extract the raw TrueHD elementary stream.
    m_progress.store(0.05f);
    int rc = runHidden(q(ffmpeg) + " -v error -y -i " + q(movie) +
                       " -map 0:" + std::to_string(aidx) +
                       " -c:a copy -f truehd " + q(thd));
    if (m_cancel.load()) { fail("cancelled"); return; }
    if (rc != 0 || !exists(thd)) { fail("truehd extraction failed"); return; }

    // 2) truehdd → DAMF (objects + positions).
    m_progress.store(0.20f);
    rc = runHidden(q(truehdd) + " decode " + q(thd) +
                   " --output-path " + q(workPrefix) + " --loglevel error");
    if (m_cancel.load()) { fail("cancelled"); return; }
    const std::string damfAudio = workPrefix + ".atmos.audio";
    if (rc != 0 || !exists(damfAudio)) { fail("truehdd decode failed"); return; }

    // 3) halosound-render --damf → binaural f32 → WAV.
    m_progress.store(0.45f);
    rc = runHidden(q(render) + " --sofa " + q(sofa) + " --room " +
                   std::to_string(room) + " --damf " + q(workPrefix) +
                   " | " + q(ffmpeg) + " -v error -f f32le -ar 48000 -ac 2 -i - " +
                   " -c:a pcm_s16le -y " + q(tmpWav));
    if (m_cancel.load()) { fail("cancelled"); return; }
    if (rc != 0 || !exists(tmpWav)) { fail("binaural render failed"); return; }

    // Clean up the big DAMF intermediates; keep only the WAV.
    DeleteFileA(thd.c_str());
    DeleteFileA((workPrefix + ".atmos").c_str());
    DeleteFileA(damfAudio.c_str());
    DeleteFileA((workPrefix + ".atmos.metadata").c_str());
    DeleteFileA((workPrefix + ".done").c_str());

    MoveFileExA(tmpWav.c_str(), outWav.c_str(), MOVEFILE_REPLACE_EXISTING);

    std::lock_guard<std::mutex> lk(m_mutex);
    m_resultPath = outWav;
    m_progress.store(1.0f);
    m_state.store(State::Ready);
}
