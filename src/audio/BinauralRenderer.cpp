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

uint64_t fileSize(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
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

// Spawn a tool DIRECTLY — no cmd.exe wrapper. That matters twice over:
// the returned HANDLE is the tool itself, so terminating it on cancel
// really stops the work (killing a cmd wrapper left the tool orphaned,
// still holding its output files open, and the next render then failed
// to overwrite them), and redirection is done with real file handles so
// each tool's stderr lands in a log we can quote when something fails.
// Empty path = no redirection for that stream.
HANDLE spawnTool(const std::string& cmdline,
                 const std::string& stdoutPath,
                 const std::string& stderrPath) {
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = INVALID_HANDLE_VALUE, hErr = INVALID_HANDLE_VALUE;
    if (!stdoutPath.empty()) {
        hOut = CreateFileA(stdoutPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hOut == INVALID_HANDLE_VALUE) return nullptr;
    }
    if (!stderrPath.empty()) {
        hErr = CreateFileA(stderrPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hErr == INVALID_HANDLE_VALUE) {
            if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
            return nullptr;
        }
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOut != INVALID_HANDLE_VALUE ? hOut
                                                 : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = hErr != INVALID_HANDLE_VALUE ? hErr
                                                 : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

int waitExitCode(HANDLE h) {
    WaitForSingleObject(h, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    return (int)code;
}

// Blocking run, for the short extraction step.
int runHidden(const std::string& cmdline, const std::string& stderrPath) {
    HANDLE h = spawnTool(cmdline, std::string(), stderrPath);
    if (!h) return -1;
    int rc = waitExitCode(h);
    CloseHandle(h);
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

std::string q(const std::string& s) { return "\"" + s + "\""; }

void touch(const std::string& p) {
    HANDLE f = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

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

void BinauralRenderer::killChildren() {
    std::lock_guard<std::mutex> lk(m_procMutex);
    for (void*& h : m_children) {
        if (h) TerminateProcess((HANDLE)h, 1);
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
    const std::string ffmpeg  = toolPath("ffmpeg.exe");
    const std::string truehdd = toolPath("truehdd.exe");
    const std::string render  = toolPath("halosound-render.exe");

    const std::string thd       = workPrefix + ".thd";
    const std::string damfAudio = workPrefix + ".atmos.audio";
    const std::string damfDone  = workPrefix + ".done";
    const std::string logX      = workPrefix + ".extract.log";
    const std::string logD      = workPrefix + ".truehdd.log";
    const std::string logR      = workPrefix + ".render.log";

    auto cleanupWork = [&]() {
        DeleteFileA(thd.c_str());
        DeleteFileA((workPrefix + ".atmos").c_str());
        DeleteFileA(damfAudio.c_str());
        DeleteFileA((workPrefix + ".atmos.metadata").c_str());
        DeleteFileA(damfDone.c_str());
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
        DeleteFileA(outPcm.c_str());
        std::lock_guard<std::mutex> lk(m_mutex);
        m_error = msg;
        m_renderedSec.store(0.0);
        m_state.store(State::Failed);
    };

    // Stale leftovers from an interrupted run would trip the follow reader.
    DeleteFileA(damfDone.c_str());
    DeleteFileA(outPcm.c_str());
    DeleteFileA((outPcm + ".ok").c_str());

    // 1) Extract the raw TrueHD elementary stream. Quick (a remux), so a
    //    blocking run keeps the error handling simple.
    m_progress.store(0.02f);
    int rc = runHidden(q(ffmpeg) + " -v error -y -i " + q(movie) +
                       " -map 0:" + std::to_string(aidx) +
                       " -c:a copy -f truehd " + q(thd), logX);
    if (m_cancel.load()) { fail("cancelled"); return; }
    if (rc != 0 || !exists(thd)) { fail("truehd extraction failed", logX); return; }

    // 2+3) truehdd and the render run TOGETHER, the render tailing the DAMF
    //      via --follow exactly like the TV server does, its f32 stdout
    //      redirected straight into the sidecar. The sidecar becomes
    //      playable while both are still running — that's the whole point.
    HANDLE hDecode = spawnTool(q(truehdd) + " decode " + q(thd) +
                               " --output-path " + q(workPrefix) +
                               " --loglevel error",
                               std::string(), logD);
    if (!hDecode) { fail("could not start truehdd"); return; }

    std::string soloArg;
    if (solo == Solo::Bed)     soloArg = " --solo bed";
    if (solo == Solo::Objects) soloArg = " --solo objects";
    HANDLE hRender = spawnTool(q(render) + " --sofa " + q(sofa) + " --room " +
                               std::to_string(room) + " --damf " + q(workPrefix) +
                               " --follow" + soloArg,
                               outPcm, logR);
    if (!hRender) {
        TerminateProcess(hDecode, 1); CloseHandle(hDecode);
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

        if (!decodeDone &&
            WaitForSingleObject(hDecode, 0) == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(hDecode, &code);
            decodeDone = true;
            if (code != 0) { decodeFailed = true; TerminateProcess(hRender, 1); }
            else touch(damfDone);
        }

        m_renderedSec.store(fileSize(outPcm) / kBytesPerSecond);
        if (durationSec > 0.0) {
            float p = (float)(m_renderedSec.load() / durationSec);
            m_progress.store(p < 0.99f ? p : 0.99f);
        }

        if (WaitForSingleObject(hRender, 250) == WAIT_OBJECT_0) break;
    }

    {
        std::lock_guard<std::mutex> lk(m_procMutex);
        m_children[0] = m_children[1] = nullptr;
    }

    if (m_cancel.load()) {
        TerminateProcess(hDecode, 1);
        TerminateProcess(hRender, 1);
        CloseHandle(hDecode); CloseHandle(hRender);
        fail("cancelled");
        return;
    }

    DWORD renderCode = 1;
    GetExitCodeProcess(hRender, &renderCode);
    if (!decodeDone) {
        // Render exited first — with --follow that only happens on error.
        TerminateProcess(hDecode, 1);
        decodeFailed = true;
    }
    CloseHandle(hDecode); CloseHandle(hRender);

    if (decodeFailed) { fail("truehdd decode failed", logD); return; }
    if (renderCode != 0 || fileSize(outPcm) == 0) { fail("binaural render failed", logR); return; }

    cleanupWork();
    DeleteFileA(logX.c_str());
    DeleteFileA(logD.c_str());
    DeleteFileA(logR.c_str());
    touch(outPcm + ".ok");

    std::lock_guard<std::mutex> lk(m_mutex);
    m_renderedSec.store(fileSize(outPcm) / kBytesPerSecond);
    m_progress.store(1.0f);
    m_state.store(State::Ready);
}
