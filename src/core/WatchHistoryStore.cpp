#include "WatchHistoryStore.h"
#include "core/ResumeStore.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
struct Record {
    std::string path;
    WatchHistoryStore::Entry entry;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;
    // -1: manually unwatched; 0: automatic; 1: manually watched.
    int overrideState = 0;
};

std::map<std::string, Record> records;
std::string databasePath;
bool loaded = false;

std::string keyFor(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    auto absolute = std::filesystem::absolute(std::filesystem::u8path(path), ec);
    if (ec) return path;
    auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    std::string key = ec ? absolute.u8string() : canonical.u8string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return key;
}

std::string encode(const std::string& input) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(input.size() * 2);
    for (unsigned char c : input) {
        result.push_back(digits[c >> 4]);
        result.push_back(digits[c & 15]);
    }
    return result;
}

bool decode(const std::string& input, std::string* output) {
    if (!output || input.size() % 2) return false;
    output->clear();
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < input.size(); i += 2) {
        const int a = digit(input[i]), b = digit(input[i + 1]);
        if (a < 0 || b < 0) return false;
        output->push_back(static_cast<char>((a << 4) | b));
    }
    return true;
}

void ensurePath() {
    if (databasePath.empty()) databasePath = plat::configDir() + "watch-history-v1.tsv";
}

bool validFile(const Record& record, const std::string& path) {
    return plat::exists(path) && plat::fileSize(path) == record.size &&
           plat::fileMtime(path) == record.mtime;
}

Record newRecord(const std::string& path) {
    Record record;
    record.path = keyFor(path);
    record.size = plat::fileSize(path);
    record.mtime = plat::fileMtime(path);
    return record;
}

bool persist() {
    ensurePath();
    const std::string temp = databasePath + ".tmp";
#ifdef _WIN32
    FILE* f = _wfopen(std::filesystem::u8path(temp).c_str(), L"wb");
#else
    FILE* f = std::fopen(temp.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = std::fprintf(f, "mpv-sofa-watch-history\t1\n") > 0;
    for (const auto& pair : records) {
        const Record& r = pair.second;
        std::ostringstream row;
        row.imbue(std::locale::classic());
        row << std::setprecision(17) << encode(r.path) << '\t' << r.size
            << '\t' << r.mtime << '\t' << r.entry.position << '\t'
            << r.entry.furthestPosition << '\t' << r.entry.duration << '\t'
            << r.entry.lastPlayedAt << '\t' << (r.entry.watched ? 1 : 0)
            << '\t' << r.overrideState << '\n';
        const std::string line = row.str();
        if (std::fwrite(line.data(), 1, line.size(), f) != line.size()) {
            ok = false;
            break;
        }
    }
    if (ok && std::fflush(f) != 0) ok = false;
#ifdef _WIN32
    if (ok && _commit(_fileno(f)) != 0) ok = false;
#else
    if (ok && ::fsync(fileno(f)) != 0) ok = false;
#endif
    if (std::fclose(f) != 0) ok = false;
    if (ok) {
#ifdef _WIN32
        const auto from = std::filesystem::u8path(temp).wstring();
        const auto to = std::filesystem::u8path(databasePath).wstring();
        ok = MoveFileExW(from.c_str(), to.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = ::rename(temp.c_str(), databasePath.c_str()) == 0;
        if (ok) {
            const auto parent = std::filesystem::u8path(databasePath).parent_path();
            const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
            if (fd >= 0) { (void)::fsync(fd); ::close(fd); }
        }
#endif
    }
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(temp), ec);
        std::fprintf(stderr, "[WatchHistoryStore] Could not persist history\n");
    }
    return ok;
}

Record* lookup(const std::string& path) {
    const auto it = records.find(keyFor(path));
    if (it == records.end() || !validFile(it->second, path)) return nullptr;
    return &it->second;
}
} // namespace

void WatchHistoryStore::init() {
    if (loaded) return;
    loaded = true;
    ensurePath();
    std::ifstream f(std::filesystem::u8path(databasePath), std::ios::binary);
    std::string line;
    if (!std::getline(f, line) || line != "mpv-sofa-watch-history\t1") return;
    while (std::getline(f, line)) {
        std::istringstream stream(line);
        stream.imbue(std::locale::classic());
        std::string pathHex;
        Record r;
        int watched = 0;
        if (!(stream >> pathHex >> r.size >> r.mtime >> r.entry.position >>
              r.entry.furthestPosition >> r.entry.duration >>
              r.entry.lastPlayedAt >> watched >> r.overrideState)) continue;
        if (!decode(pathHex, &r.path) || r.path.empty() ||
            !std::isfinite(r.entry.position) ||
            !std::isfinite(r.entry.furthestPosition) ||
            !std::isfinite(r.entry.duration) ||
            r.entry.position < 0.0 || r.entry.furthestPosition < 0.0 ||
            r.entry.duration < 0.0 || r.overrideState < -1 ||
            r.overrideState > 1) continue;
        r.entry.watched = watched != 0;
        r.entry.manuallyWatched = r.overrideState == 1;
        records[keyFor(r.path)] = std::move(r);
    }
}

bool WatchHistoryStore::find(const std::string& path, Entry* out) {
    init();
    if (Record* r = lookup(path)) {
        if (out) *out = r->entry;
        return true;
    }
    // Existing installations have only resume checkpoints. Import them
    // lazily when the relevant file is encountered in the explorer.
    ResumeStore::Entry old;
    if (!ResumeStore::find(path, &old)) return false;
    if (!checkpoint(path, old.position, old.duration, 90)) return false;
    if (out) *out = lookup(path)->entry;
    return true;
}

bool WatchHistoryStore::checkpoint(const std::string& path, double position,
                                   double duration, int threshold) {
    init();
    if (path.empty() || !plat::exists(path) || !std::isfinite(position) ||
        !std::isfinite(duration) || position < 0.0 || duration < 0.0) return false;
    const std::string key = keyFor(path);
    Record* old = lookup(path);
    Record r = old ? *old : newRecord(path);
    r.entry.position = position;
    r.entry.duration = std::max(r.entry.duration, duration);
    r.entry.furthestPosition = std::max(r.entry.furthestPosition, position);
    r.entry.lastPlayedAt = static_cast<std::uint64_t>(std::time(nullptr));
    const int limit = std::clamp(threshold, 70, 99);
    if (r.overrideState == 0 && r.entry.duration > 0.0 &&
        r.entry.furthestPosition * 100.0 / r.entry.duration >= limit)
        r.entry.watched = true;
    records[key] = std::move(r);
    return persist();
}

bool WatchHistoryStore::finish(const std::string& path, double duration) {
    init();
    if (path.empty() || !plat::exists(path) || !std::isfinite(duration)) return false;
    Record* old = lookup(path);
    Record r = old ? *old : newRecord(path);
    r.entry.duration = std::max(r.entry.duration, std::max(0.0, duration));
    r.entry.position = r.entry.duration;
    r.entry.furthestPosition = r.entry.duration;
    r.entry.lastPlayedAt = static_cast<std::uint64_t>(std::time(nullptr));
    r.entry.watched = true;
    r.overrideState = 0;
    r.entry.manuallyWatched = false;
    records[keyFor(path)] = std::move(r);
    return persist();
}

bool WatchHistoryStore::setWatched(const std::string& path, bool watched) {
    init();
    if (path.empty() || !plat::exists(path)) return false;
    Record* old = lookup(path);
    Record r = old ? *old : newRecord(path);
    r.entry.watched = watched;
    r.overrideState = watched ? 1 : 0;
    r.entry.manuallyWatched = watched;
    if (!watched) {
        r.entry.position = 0.0;
        r.entry.furthestPosition = 0.0;
    }
    records[keyFor(path)] = std::move(r);
    return persist();
}

const char* WatchHistoryStore::filePath() {
    ensurePath();
    return databasePath.c_str();
}
