#include "MediaServer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#endif

#include <cstdio>
#include <vector>

namespace {

// Directory containing the running executable, with trailing slash.
std::string exeDir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash + 1);
}

bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Locate the server entry point.  Two layouts are supported:
//  - packaged:  <exe>/halosound-server/src/index.js
//  - dev tree:  exe in <repo>/dist, server in <repo>/halosound/server
std::string findServerScript() {
    const std::string base = exeDir();
    const char* candidates[] = {
        "halosound-server\\src\\index.js",
        "..\\halosound\\server\\src\\index.js",
        "..\\..\\halosound\\server\\src\\index.js",
    };
    for (const char* rel : candidates) {
        std::string p = base + rel;
        if (fileExists(p)) return p;
    }
    return {};
}

} // namespace

MediaServer::~MediaServer() {
    stop();
}

bool MediaServer::start(const std::string& mediaDir, int port) {
    m_error.clear();
    if (isRunning()) return true;

    if (mediaDir.empty()) {
        m_error = "No media folder selected";
        return false;
    }
    DWORD attrs = GetFileAttributesA(mediaDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        m_error = "Media folder does not exist: " + mediaDir;
        return false;
    }

    const std::string script = findServerScript();
    if (script.empty()) {
        m_error = "Server files not found (halosound/server)";
        return false;
    }
    if (port < 1 || port > 65534) port = 8080;

    // The server needs the patched ffmpeg.  If a bundled copy sits next
    // to the exe, point the child at it; otherwise the child inherits
    // our environment (FFMPEG_PATH may already be set system-wide).
    const std::string ffmpeg  = exeDir() + "ffmpeg.exe";
    const std::string ffprobe = exeDir() + "ffprobe.exe";
    if (fileExists(ffmpeg))  SetEnvironmentVariableA("FFMPEG_PATH",  ffmpeg.c_str());
    if (fileExists(ffprobe)) SetEnvironmentVariableA("FFPROBE_PATH", ffprobe.c_str());

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "node \"%s\" \"%s\" --port %d --ws-port %d",
             script.c_str(), mediaDir.c_str(), port, port + 1);

    // Job object so the server can never outlive the GUI.
    if (!m_job) {
        m_job = CreateJobObjectA(nullptr, nullptr);
        if (m_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject((HANDLE)m_job, JobObjectExtendedLimitInformation,
                                    &info, sizeof(info));
        }
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        m_error = (err == ERROR_FILE_NOT_FOUND)
            ? "Node.js not found - install it from nodejs.org"
            : "Failed to launch server (error " + std::to_string(err) + ")";
        return false;
    }

    if (m_job) AssignProcessToJobObject((HANDLE)m_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    m_process = pi.hProcess;
    m_port = port;
    return true;
}

void MediaServer::stop() {
    if (m_process) {
        TerminateProcess((HANDLE)m_process, 0);
        WaitForSingleObject((HANDLE)m_process, 2000);
        CloseHandle((HANDLE)m_process);
        m_process = nullptr;
    }
}

bool MediaServer::isRunning() {
    if (!m_process) return false;
    if (WaitForSingleObject((HANDLE)m_process, 0) == WAIT_TIMEOUT) return true;
    // Child exited on its own (crash / port clash) — reap the handle.
    CloseHandle((HANDLE)m_process);
    m_process = nullptr;
    return false;
}

std::string MediaServer::lanAddress() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &size) != NO_ERROR)
        return {};

    std::string best;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            std::string s(ip);
            if (s.rfind("169.254.", 0) == 0) continue;   // link-local
            // Prefer private-range addresses (that's what the TV reaches).
            bool priv = s.rfind("192.168.", 0) == 0 || s.rfind("10.", 0) == 0;
            if (best.empty() || priv) best = s;
            if (priv) return best;
        }
    }
    return best;
}
