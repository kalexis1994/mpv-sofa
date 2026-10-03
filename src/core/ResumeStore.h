#pragma once

#include <cstdint>
#include <string>

// Durable per-file playback checkpoints.  Kept separate from Settings so a
// checkpoint never rewrites (or risks) the user's preferences file.
namespace ResumeStore {

struct Entry {
    double position = 0.0;
    double duration = 0.0;
    std::uint64_t updatedAt = 0;
    // -1 means an older checkpoint did not record this choice. Subtitle 0
    // explicitly means "None"; positive values are mpv track IDs.
    int audioTrackId = -1;
    int subtitleTrackId = -1;
    bool subtitlesVisible = true;
};

// Load the database. Safe to call more than once.
void init();

// Returns a valid checkpoint only when the path still identifies the same
// on-disk file (size + mtime). Stale/replaced-file entries are discarded.
bool find(const std::string& path, Entry* out);

// Persist a checkpoint immediately using an atomic replace. Very early and
// effectively-finished positions are removed instead of offered later.
bool checkpoint(const std::string& path, double position, double duration,
                int audioTrackId = -1, int subtitleTrackId = -1,
                bool subtitlesVisible = true);

// Mark a title watched / explicitly restarted.
bool clear(const std::string& path);

const char* filePath();

} // namespace ResumeStore
