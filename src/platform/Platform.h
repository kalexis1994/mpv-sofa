#pragma once

// Thin OS shim for the handful of things the app can't do portably: locating
// its own binary, per-user directories, cheap file stats, and spawning the
// external render tools.
//
// The process API deliberately mirrors the Win32 shape the renderer was
// written against — the caller owns the returned handle and must closeProc()
// it — because that ownership is what makes cancellation correct: the handle
// is the tool itself, so terminating it really stops the work.  A shell
// wrapper would leave the tool orphaned, still holding its output files open,
// and the next render would fail to overwrite them.
//
// Commands are passed as an argv vector rather than a command line.  Win32
// wants a single string and POSIX wants the vector, so one of the two has to
// do the joining; doing it here means the quoting rules live in exactly one
// place instead of at every call site.

#include <cstdint>
#include <string>
#include <vector>

namespace plat {

// Opaque process handle.  nullptr means "failed to spawn".
using Proc = void*;

// ---------------------------------------------------------------------------
// Paths.  Every directory getter returns a trailing separator so callers can
// concatenate without thinking about it.
// ---------------------------------------------------------------------------

// Directory holding the running executable.  Empty if it can't be determined.
std::string exeDir();

// Per-user config directory (XDG_CONFIG_HOME/mpv-sofa on Linux, next to the
// executable on Windows to preserve the portable-install contract).  Created
// on first call.
std::string configDir();

// Per-user cache directory, for regenerable data — the binaural sidecars and
// their work files.  Created on first call.
std::string cacheDir();

// Read-only application assets (hrtf profiles, fonts, shaders).  Falls back to
// the executable directory, which is where the build copies them.
std::string assetsDir();

// Platform executable suffix: ".exe" on Windows, empty elsewhere.
const char* exeSuffix();

// ---------------------------------------------------------------------------
// File helpers.  These exist because the renderer polls a growing file from a
// worker thread and <filesystem> throws on races that we simply don't care
// about — a vanished file is just "size 0, try again next tick".
// ---------------------------------------------------------------------------

bool     exists(const std::string& path);
uint64_t fileSize(const std::string& path);

// Opaque modification stamp.  Only ever used as a cache-key ingredient, so the
// units and epoch don't need to agree across platforms.
uint64_t fileMtime(const std::string& path);

void removeFile(const std::string& path);
bool makeDir(const std::string& path);      // no-op if it already exists
void touchFile(const std::string& path);    // create/truncate to zero bytes

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------

// Spawn argv[0] directly, no shell, with no console window on Windows.
// An empty stdoutPath/stderrPath leaves that stream inherited from the parent;
// otherwise it is truncated and the child writes straight into it.
Proc spawn(const std::vector<std::string>& argv,
           const std::string& stdoutPath,
           const std::string& stderrPath);

// Block until the child exits and return its exit code (1 if unknowable).
int waitExit(Proc p);

// Wait up to timeoutMs (0 = poll).  Returns true when the child has exited,
// writing its code to exitCode if non-null.  Safe to call after it already
// exited — the status is remembered, not re-reaped.
bool tryWait(Proc p, int timeoutMs, int* exitCode);

void terminate(Proc p);
void closeProc(Proc p);

}  // namespace plat
