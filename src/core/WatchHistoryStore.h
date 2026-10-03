#pragma once

#include <cstdint>
#include <string>

// Playback history survives completion and is independent of resume points.
namespace WatchHistoryStore {

struct Entry {
    double position = 0.0;
    double furthestPosition = 0.0;
    double duration = 0.0;
    std::uint64_t lastPlayedAt = 0;
    bool watched = false;
    bool manuallyWatched = false;
};

void init();
bool find(const std::string& path, Entry* out);
bool checkpoint(const std::string& path, double position, double duration,
                int watchedThresholdPercent);
bool finish(const std::string& path, double duration);
bool setWatched(const std::string& path, bool watched);
const char* filePath();

} // namespace WatchHistoryStore
