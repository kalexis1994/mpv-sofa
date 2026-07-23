#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

/*
 * Object-based Atmos on the desktop, the robust way.
 *
 * FFmpeg's TrueHD decoder can't losslessly extract the 12-channel Atmos
 * object substream (0x31EC), so instead of fighting it we reuse the exact
 * chain the streaming server already trusts:
 *
 *   ffmpeg (-c:a copy -f truehd)  →  truehdd (objects → DAMF)
 *     →  halosound-render --damf --follow (binaural through the same DSP)
 *
 * truehdd and the render run CONCURRENTLY: --follow tails the growing DAMF
 * the way the TV server streams it, and the render's f32 stdout goes
 * straight into the sidecar file. That makes the sidecar usable while it
 * is still being written — playback starts on the bed-based filter and
 * hot-swaps to objects as soon as the render head is safely ahead of the
 * playhead, instead of waiting for the whole movie.
 *
 * The sidecar is raw PCM (f32le stereo 48 kHz), not WAV, deliberately: a
 * growing file can't carry a correct RIFF size header, and mpv plays raw
 * fine given explicit demuxer parameters. Completion is recorded in a
 * ".ok" marker next to it; cached by (file, audio stream, sofa, room,
 * mtime) so re-opening is instant and only a profile/room change
 * re-renders.
 */
class BinauralRenderer {
public:
    enum class State { Idle, Rendering, Ready, Failed };

    // Sidecar sample format (matches halosound-render's stdout).
    static constexpr int    kRate     = 48000;
    static constexpr int    kChannels = 2;
    static constexpr double kBytesPerSecond = kRate * kChannels * 4.0;  // f32

    BinauralRenderer();
    ~BinauralRenderer();

    // Audition filter for the render: everything, or one layer soloed —
    // the only way to hear the objects alone, since the sidecar mixes
    // bed + objects inside halosound-render (the realtime filter's debug
    // solos never see these channels).
    enum class Solo { None = 0, Bed = 1, Objects = 2 };

    // Kick off (or resolve from cache) a render for one movie + settings.
    // Cheap and non-blocking; the work runs on a background thread.
    // audioStreamIndex is the absolute ffmpeg stream index of the TrueHD
    // track; durationSec drives the progress estimate (0 = unknown).
    // Each Solo mode caches separately. Returns false if the tools aren't
    // available.
    bool request(const std::string& moviePath, int audioStreamIndex,
                 const std::string& sofaPath, int roomPreset,
                 double durationSec, Solo solo = Solo::None);

    // Abandon any in-flight render (kills the tool processes) and reset.
    void cancel();

    State  state() const { return m_state.load(); }
    float  progress() const { return m_progress.load(); }   // 0..1, best-effort

    // Seconds of binaural audio rendered so far — how far into the movie
    // the sidecar can currently play. Grows while Rendering; in Ready it
    // covers the whole file.
    double renderedSeconds() const { return m_renderedSec.load(); }

    // The sidecar path. Valid in Ready, and during Rendering once
    // renderedSeconds() > 0 (the file exists and is growing).
    std::string resultPath();
    std::string error();

private:
    void runChain(std::string movie, int aidx, std::string sofa, int room,
                  std::string outPcm, std::string workPrefix, double durationSec,
                  Solo solo);
    static std::string toolPath(const char* exe);
    void killChildren();

    std::atomic<State> m_state{State::Idle};
    std::atomic<float> m_progress{0.0f};
    std::atomic<double> m_renderedSec{0.0};
    std::atomic<bool>  m_cancel{false};
    std::atomic<uint64_t> m_generation{0};   // invalidates stale threads

    std::mutex  m_mutex;
    std::string m_resultPath;
    std::string m_error;
    std::string m_currentKey;                // dedupe identical requests
    std::thread m_thread;

    // In-flight child processes, so cancel() can terminate them instead of
    // waiting a movie's worth of decode. Guarded by m_procMutex.
    std::mutex m_procMutex;
    void*      m_children[2] = {nullptr, nullptr};   // HANDLEs
};
