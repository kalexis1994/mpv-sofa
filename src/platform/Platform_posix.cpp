// POSIX implementation of the platform shim.  See Platform.h for the contract.

#include "Platform.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

// A pid alone isn't enough: waitpid() reaps a child exactly once, and the
// renderer's progress loop polls the same handle repeatedly and then asks for
// the exit code again at teardown.  Remembering the status here keeps that
// call pattern working, and matches the Win32 handle it replaces.
struct ProcRec {
    pid_t pid    = -1;
    bool  exited = false;
    int   code   = 1;
};

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = ::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

std::string homeDir() {
    return envOr("HOME", "/tmp");
}

// mkdir -p.  XDG roots usually exist, but the per-app leaf under them may not,
// and on a fresh account neither does.
bool makeDirRecursive(const std::string& path) {
    if (path.empty()) return false;
    std::string acc;
    acc.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        acc += path[i];
        const bool last = (i + 1 == path.size());
        if (path[i] != '/' && !last) continue;
        if (acc == "/") continue;
        std::string dir = acc;
        if (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

std::string ensureDir(std::string path) {
    if (!path.empty() && path.back() != '/') path += '/';
    makeDirRecursive(path);
    return path;
}

void sleepMs(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    ::nanosleep(&ts, nullptr);
}

// Reap without blocking and latch the result.  Returns true once the child is
// known to be gone.
bool reap(ProcRec* r) {
    if (r->exited) return true;
    int status = 0;
    const pid_t got = ::waitpid(r->pid, &status, WNOHANG);
    if (got != r->pid) {
        // ECHILD means somebody else already reaped it (or it never existed).
        // Either way there is nothing left to wait for.
        if (got == -1 && errno == ECHILD) { r->exited = true; r->code = 1; return true; }
        return false;
    }
    r->exited = true;
    r->code   = WIFEXITED(status) ? WEXITSTATUS(status)
                                  : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
    return true;
}

}  // namespace

namespace plat {

std::string exeDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    std::string p(buf);
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash + 1);
}

std::string configDir() {
    return ensureDir(envOr("XDG_CONFIG_HOME", homeDir() + "/.config") + "/mpv-sofa");
}

std::string cacheDir() {
    return ensureDir(envOr("XDG_CACHE_HOME", homeDir() + "/.cache") + "/mpv-sofa");
}

std::string assetsDir() {
    // Run-from-build-tree first (CMake copies assets next to the binary), then
    // the FHS location a packaged install would use.
    const std::string local = exeDir() + "assets/";
    if (exists(local)) return local;
    const std::string shared = exeDir() + "../share/mpv-sofa/assets/";
    if (exists(shared)) return shared;
    return local;
}

const char* exeSuffix() { return ""; }

bool exists(const std::string& path) {
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

uint64_t fileSize(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

uint64_t fileMtime(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
}

void removeFile(const std::string& path) {
    if (!path.empty()) ::unlink(path.c_str());
}

bool makeDir(const std::string& path) {
    return makeDirRecursive(path);
}

void touchFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) ::close(fd);
}

Proc spawn(const std::vector<std::string>& argv,
           const std::string& stdoutPath,
           const std::string& stderrPath) {
    if (argv.empty()) return nullptr;

    posix_spawn_file_actions_t fa;
    if (::posix_spawn_file_actions_init(&fa) != 0) return nullptr;

    // Redirect by opening in the child so the parent never holds a descriptor
    // it would have to remember to close — the Win32 side has to do exactly
    // that dance and it is the easiest thing there to get wrong.
    bool ok = true;
    if (!stdoutPath.empty()) {
        ok = ::posix_spawn_file_actions_addopen(
                 &fa, STDOUT_FILENO, stdoutPath.c_str(),
                 O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0;
    }
    if (ok && !stderrPath.empty()) {
        ok = ::posix_spawn_file_actions_addopen(
                 &fa, STDERR_FILENO, stderrPath.c_str(),
                 O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0;
    }
    if (!ok) {
        ::posix_spawn_file_actions_destroy(&fa);
        return nullptr;
    }

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = -1;
    // posix_spawnp so a bare tool name resolves through PATH, which is what
    // BinauralRenderer::toolPath() falls back to when the tool isn't shipped
    // next to the binary.
    const int rc = ::posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
    ::posix_spawn_file_actions_destroy(&fa);
    if (rc != 0 || pid <= 0) return nullptr;

    ProcRec* r = new ProcRec();
    r->pid = pid;
    return (Proc)r;
}

int waitExit(Proc p) {
    ProcRec* r = (ProcRec*)p;
    if (!r) return 1;
    if (r->exited) return r->code;
    int status = 0;
    if (::waitpid(r->pid, &status, 0) == r->pid) {
        r->code = WIFEXITED(status) ? WEXITSTATUS(status)
                                    : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
    }
    r->exited = true;
    return r->code;
}

bool tryWait(Proc p, int timeoutMs, int* exitCode) {
    ProcRec* r = (ProcRec*)p;
    if (!r) return true;
    // No POSIX primitive waits on a specific pid with a deadline, so poll.
    // The caller uses this both to test for exit and to pace its loop, and at
    // 10 ms granularity the extra wakeups are irrelevant next to the decode.
    const int step = 10;
    int waited = 0;
    for (;;) {
        if (reap(r)) { if (exitCode) *exitCode = r->code; return true; }
        if (waited >= timeoutMs) return false;
        sleepMs(step);
        waited += step;
    }
}

void terminate(Proc p) {
    ProcRec* r = (ProcRec*)p;
    if (!r || r->exited || r->pid <= 0) return;
    // SIGTERM first so the tool can drop its output files cleanly; the render
    // is holding a sidecar the player may still be reading.
    ::kill(r->pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {           // up to ~200 ms
        if (reap(r)) return;
        sleepMs(10);
    }
    ::kill(r->pid, SIGKILL);
    reap(r);
}

void closeProc(Proc p) {
    ProcRec* r = (ProcRec*)p;
    if (!r) return;
    // Don't leave a zombie behind if the caller never waited.
    if (!r->exited && r->pid > 0) {
        int status = 0;
        ::waitpid(r->pid, &status, WNOHANG);
    }
    delete r;
}

}  // namespace plat
