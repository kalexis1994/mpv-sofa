#pragma once

#include <atomic>
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
 *     →  halosound-render --damf (binaural through the same DSP)
 *     →  ffmpeg (f32 → WAV)
 *
 * The result is a binaural stereo sidecar mpv plays as an external audio
 * track (native A/V sync, no live two-clock problem). It's rendered once
 * in the background, cached by (file, audio stream, sofa, room, mtime),
 * so re-opening is instant and only a profile/room change re-renders.
 */
class BinauralRenderer {
public:
    enum class State { Idle, Rendering, Ready, Failed };

    BinauralRenderer();
    ~BinauralRenderer();

    // Kick off (or resolve from cache) a render for one movie + settings.
    // Cheap and non-blocking; the work runs on a background thread.
    // audioStreamIndex is the absolute ffmpeg stream index of the TrueHD
    // track. Returns false if the tools aren't available.
    bool request(const std::string& moviePath, int audioStreamIndex,
                 const std::string& sofaPath, int roomPreset);

    // Abandon any in-flight render and reset to Idle.
    void cancel();

    State  state() const { return m_state.load(); }
    float  progress() const { return m_progress.load(); }   // 0..1, best-effort
    // Valid only in Ready state: the cached WAV to feed mpv.
    std::string resultPath();
    std::string error();

private:
    void runChain(std::string movie, int aidx, std::string sofa, int room,
                  std::string outWav, std::string workPrefix, double durationSec);
    static std::string toolPath(const char* exe);

    std::atomic<State> m_state{State::Idle};
    std::atomic<float> m_progress{0.0f};
    std::atomic<bool>  m_cancel{false};
    std::atomic<uint64_t> m_generation{0};   // invalidates stale threads

    std::mutex  m_mutex;
    std::string m_resultPath;
    std::string m_error;
    std::string m_currentKey;                // dedupe identical requests
    std::thread m_thread;
};
