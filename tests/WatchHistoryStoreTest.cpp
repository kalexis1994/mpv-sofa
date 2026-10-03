#include "core/WatchHistoryStore.h"
#include "core/ResumeStore.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "verify") {
        setenv("XDG_CONFIG_HOME", argv[2], 1);
        WatchHistoryStore::Entry persisted;
        const auto movie = std::filesystem::path(argv[2]) / "movie.mkv";
        assert(WatchHistoryStore::find(movie.string(), &persisted));
        assert(persisted.watched && persisted.position == 1000);
        return 0;
    }
    const auto root = std::filesystem::temp_directory_path() /
        ("mpv-sofa-watch-test-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(root);
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    const auto movie = root / "movie.mkv";
    { std::ofstream f(movie, std::ios::binary); f << "original media"; }

    WatchHistoryStore::Entry e;
    assert(WatchHistoryStore::checkpoint(movie.string(), 30, 1000, 90));
    assert(WatchHistoryStore::find(movie.string(), &e) && !e.watched);
    assert(WatchHistoryStore::checkpoint(movie.string(), 500, 1000, 90));
    assert(WatchHistoryStore::find(movie.string(), &e) && !e.watched);
    assert(e.furthestPosition == 500);
    assert(WatchHistoryStore::checkpoint(movie.string(), 900, 1000, 90));
    assert(WatchHistoryStore::find(movie.string(), &e) && e.watched);

    // A replay does not erase the watched state or furthest point.
    assert(WatchHistoryStore::checkpoint(movie.string(), 200, 1000, 90));
    assert(WatchHistoryStore::find(movie.string(), &e) && e.watched);
    assert(e.position == 200 && e.furthestPosition == 900);

    assert(WatchHistoryStore::setWatched(movie.string(), false));
    assert(WatchHistoryStore::find(movie.string(), &e) && !e.watched);
    assert(e.position == 0 && e.furthestPosition == 0);
    assert(WatchHistoryStore::checkpoint(movie.string(), 920, 1000, 90));
    assert(WatchHistoryStore::find(movie.string(), &e) && e.watched);
    assert(WatchHistoryStore::setWatched(movie.string(), false));
    assert(WatchHistoryStore::finish(movie.string(), 1000));
    assert(WatchHistoryStore::find(movie.string(), &e) && e.watched);
    assert(std::filesystem::exists(WatchHistoryStore::filePath()));
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(argv[0], argv[0], "verify", root.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // A replacement at the same path must not inherit the previous film.
    { std::ofstream f(movie, std::ios::binary | std::ios::app); f << " changed"; }
    assert(!WatchHistoryStore::find(movie.string(), &e));

    // Import a checkpoint left by an older installation.
    const auto old = root / "old.mkv";
    { std::ofstream f(old, std::ios::binary); f << "old media"; }
    assert(ResumeStore::checkpoint(old.string(), 120, 1000));
    assert(WatchHistoryStore::find(old.string(), &e));
    assert(e.position == 120 && !e.watched);

    std::filesystem::remove_all(root);
    std::cout << "WatchHistoryStore tests passed\n";
}
