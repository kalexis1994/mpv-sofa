#include "core/ResumeStore.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("mpv-sofa-resume-test-" + std::to_string((long long)getpid()));
    std::filesystem::create_directories(root);
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);

    const auto media = root / "movie.mkv";
    { std::ofstream f(media, std::ios::binary); f << "original media"; }

    ResumeStore::init();
    assert(ResumeStore::checkpoint(media.string(), 123.5, 7200.0,
                                   3, 7, false));
    ResumeStore::Entry e;
    assert(ResumeStore::find(media.string(), &e));
    assert(e.position == 123.5);
    assert(e.duration == 7200.0);
    assert(e.audioTrackId == 3);
    assert(e.subtitleTrackId == 7);
    assert(!e.subtitlesVisible);

    // Replacing the media at the same path must not inherit the old title's
    // position. A size change is enough to exercise the fingerprint guard.
    { std::ofstream f(media, std::ios::binary | std::ios::app); f << " changed"; }
    assert(!ResumeStore::find(media.string(), &e));

    assert(ResumeStore::checkpoint(media.string(), 300.0, 7200.0));
    assert(ResumeStore::clear(media.string()));
    assert(!ResumeStore::find(media.string(), &e));

    std::filesystem::remove_all(root);
    std::cout << "ResumeStore tests passed\n";
    return 0;
}
