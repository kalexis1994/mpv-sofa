#include "app/Application.h"
#include <cstdio>
#include <cstring>
#include <clocale>

#ifdef _WIN32
#include <windows.h>

// The executable links against the Windows GUI subsystem (no console
// window on launch).  This helper restores stdio routing in two cases:
//
//   forceFresh = true   → AllocConsole(): a brand-new console window
//                          opens alongside the app.  Used when the
//                          user passes --console on the command line.
//   forceFresh = false  → AttachConsole(ATTACH_PARENT_PROCESS): if the
//                          process was launched from an existing
//                          terminal (cmd, PowerShell), reuse it so log
//                          output flows back to the parent prompt
//                          without spawning a second window.  Returns
//                          false when there's no parent console.
static bool acquireConsole(bool forceFresh) {
    if (forceFresh) {
        if (!AllocConsole()) return false;
    } else if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return false;
    }
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONIN$",  "r", stdin);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}
#endif

int main(int argc, char* argv[]) {
    // Force the C runtime's narrow-string functions to round-trip
    // through UTF-8 instead of the system ANSI code page — but ONLY
    // for LC_CTYPE.  ImGuiFileDialog's bundled dirent uses wcstombs_s
    // for wide → narrow filename conversion; without UTF-8 here, file
    // names containing characters outside the user's ACP (Cyrillic,
    // CJK, accented Eastern-European glyphs on a Western Windows
    // install, etc.) come back as "?" and silently disappear from the
    // file list.  We deliberately do NOT touch LC_NUMERIC: libmpv
    // refuses to initialise (returns NULL from mpv_create) when the
    // numeric locale isn't "C", because it parses floats with `.` as
    // the decimal separator.  Calling LC_ALL with ".UTF-8" caused mpv
    // to fail loading any file at all, leaving the home screen with no
    // video — that was the regression introduced by the earlier fix.
    // Requires Windows 10 1903+; older systems return NULL and we
    // continue with the default locale.
    std::setlocale(LC_CTYPE, ".UTF-8");

    bool wantConsole = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--console") == 0) {
            wantConsole = true;
            break;
        }
    }

#ifdef _WIN32
    bool gotConsole = false;
    if (wantConsole)        gotConsole = acquireConsole(true);
    if (!gotConsole)        gotConsole = acquireConsole(false);
    if (!gotConsole) {
        // Headless launch (double-click, shell shortcut, …) — nothing
        // to print to.  Redirect stderr to a log file so diagnostics
        // aren't silently lost.
        FILE* logFile = std::freopen("hrtf_log.txt", "w", stderr);
        if (logFile) {
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::fprintf(stderr, "=== HRTF Spatializer Log ===\n");
        }
    }
#else
    FILE* logFile = std::freopen("hrtf_log.txt", "w", stderr);
    if (logFile) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::fprintf(stderr, "=== HRTF Spatializer Log ===\n");
    }
#endif

    Application app;

    if (!app.init(argc, argv)) {
        std::fprintf(stderr, "Failed to initialize application\n");
        return 1;
    }

    app.run();
    app.shutdown();

    std::fprintf(stderr, "=== Clean shutdown ===\n");
    return 0;
}
