#pragma once
// MediaServer — manages the HaloSound media server (halosound/server, a
// Node app) as a child process of the GUI.  The server scans a library
// folder and streams video over HTTP + multichannel PCM over WebSocket
// to the HaloSound TV app; here we only own its lifecycle: spawn with
// the right arguments/environment, report status, and guarantee the
// child dies with us (Job object with KILL_ON_JOB_CLOSE).

#include <string>

class MediaServer {
public:
    ~MediaServer();

    // Spawn the server for `mediaDir` on `port` (WebSocket uses port+1).
    // Returns false and sets lastError() when the server files or the
    // node runtime cannot be found, or the process fails to launch.
    bool start(const std::string& mediaDir, int port);

    // Terminate the child (no-op when not running).
    void stop();

    // True while the child process is alive.
    bool isRunning();

    const std::string& lastError() const { return m_error; }

    // Port passed to the last successful start().
    int port() const { return m_port; }

    // Best-guess LAN IPv4 of this machine ("192.168.1.10"), for showing
    // the user what to type into the TV app.  Empty when unavailable.
    static std::string lanAddress();

private:
    void* m_process = nullptr;   // HANDLE
    void* m_job     = nullptr;   // HANDLE (kills child on close)
    int   m_port    = 8080;
    std::string m_error;
};
