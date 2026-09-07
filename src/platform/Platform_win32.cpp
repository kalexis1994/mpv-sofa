// Win32 implementation of the platform shim.  See Platform.h for the contract.
//
// This is the behaviour the renderer already had, lifted out of
// BinauralRenderer.cpp unchanged in spirit: spawn the tool directly with no
// cmd.exe wrapper, redirect with inheritable file handles, hand the process
// handle to the caller.

#include "Platform.h"

#include <windows.h>

#include <cstdio>

namespace {

// Quote one argument the way CommandLineToArgvW parses it back.  The old
// inline helper just wrapped the string in quotes, which silently corrupted
// any path containing a quote or ending in a backslash.  Building the command
// line here means that bug can only ever exist in one place.
void appendQuoted(std::string& out, const std::string& arg) {
    const bool needsQuotes =
        arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needsQuotes) { out += arg; return; }

    out += '"';
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == '\\') { ++slashes; ++i; }
        if (i == arg.size()) {
            // Trailing backslashes must be doubled so they don't escape the
            // closing quote.
            out.append(slashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(slashes * 2 + 1, '\\');
        } else {
            out.append(slashes, '\\');
        }
        out += arg[i];
    }
    out += '"';
}

std::string buildCommandLine(const std::vector<std::string>& argv) {
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += ' ';
        appendQuoted(cmd, argv[i]);
    }
    return cmd;
}

std::string dirOfSelf() {
    char buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    std::string p(buf);
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash + 1);
}

}  // namespace

namespace plat {

std::string exeDir() { return dirOfSelf(); }

// Windows keeps the portable-install contract: settings live next to the
// executable, not in %APPDATA%.
std::string configDir() { return dirOfSelf(); }

std::string cacheDir() {
    char tmp[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tmp);
    const std::string dir = std::string(tmp) + "mpv-sofa-binaural\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string assetsDir() { return dirOfSelf() + "assets\\"; }

const char* exeSuffix() { return ".exe"; }

bool exists(const std::string& path) {
    return !path.empty() &&
           GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

uint64_t fileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

uint64_t fileMtime(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
            fad.ftLastWriteTime.dwLowDateTime;
}

void removeFile(const std::string& path) {
    if (!path.empty()) DeleteFileA(path.c_str());
}

bool makeDir(const std::string& path) {
    if (path.empty()) return false;
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

void touchFile(const std::string& path) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

Proc spawn(const std::vector<std::string>& argv,
           const std::string& stdoutPath,
           const std::string& stderrPath) {
    if (argv.empty()) return nullptr;

    std::string cmdline = buildCommandLine(argv);
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');   // CreateProcessA writes into lpCommandLine

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
    si.dwFlags     = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput  = hOut != INVALID_HANDLE_VALUE ? hOut : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError   = hErr != INVALID_HANDLE_VALUE ? hErr : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // The child holds its own inherited duplicates from here on.
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return (Proc)pi.hProcess;
}

int waitExit(Proc p) {
    if (!p) return 1;
    WaitForSingleObject((HANDLE)p, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess((HANDLE)p, &code);
    return (int)code;
}

bool tryWait(Proc p, int timeoutMs, int* exitCode) {
    if (!p) return true;
    const DWORD r = WaitForSingleObject((HANDLE)p, (DWORD)timeoutMs);
    if (r != WAIT_OBJECT_0) return false;
    if (exitCode) {
        DWORD code = 1;
        GetExitCodeProcess((HANDLE)p, &code);
        *exitCode = (int)code;
    }
    return true;
}

void terminate(Proc p) {
    if (p) TerminateProcess((HANDLE)p, 1);
}

void closeProc(Proc p) {
    if (p) CloseHandle((HANDLE)p);
}

}  // namespace plat
