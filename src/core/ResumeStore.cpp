#include "ResumeStore.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

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
    double position = 0.0;
    double duration = 0.0;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;
    std::uint64_t updatedAt = 0;
    int audioTrackId = -1;
    int subtitleTrackId = -1;
    bool subtitlesVisible = true;
};

std::map<std::string, Record> g_records;
std::string g_path;
bool g_loaded = false;

constexpr double kMinimumResumeSeconds = 15.0;
constexpr double kFinishedSlackSeconds = 10.0;
constexpr std::size_t kMaximumRecords = 100;
constexpr std::uint64_t kMaximumAgeSeconds = 180ull * 24ull * 60ull * 60ull;

std::string canonicalKey(const std::string& input) {
    if (input.empty()) return {};
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(
        std::filesystem::absolute(std::filesystem::u8path(input), ec), ec);
    std::string key = ec ? input : p.u8string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
#endif
    return key;
}

std::string hexEncode(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexDecode(const std::string& value, std::string* out) {
    if (!out || (value.size() & 1)) return false;
    out->clear();
    out->reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int hi = hexDigit(value[i]);
        const int lo = hexDigit(value[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back((char)((hi << 4) | lo));
    }
    return true;
}

void ensurePath() {
    if (g_path.empty()) g_path = plat::configDir() + "resume-state-v1.tsv";
}

bool flushFile(FILE* f) {
    if (!f || fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

FILE* openForAtomicWrite(const std::string& path) {
#ifdef _WIN32
    return _wfopen(std::filesystem::u8path(path).c_str(), L"wb");
#else
    return fopen(path.c_str(), "wb");
#endif
}

bool atomicReplace(const std::string& tmp, const std::string& dst) {
#ifdef _WIN32
    const auto tmpWide = std::filesystem::u8path(tmp).wstring();
    const auto dstWide = std::filesystem::u8path(dst).wstring();
    return MoveFileExW(tmpWide.c_str(), dstWide.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (::rename(tmp.c_str(), dst.c_str()) != 0) return false;
    std::filesystem::path parent = std::filesystem::u8path(dst).parent_path();
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { (void)::fsync(fd); ::close(fd); }
    return true;
#endif
}

void prune() {
    const std::uint64_t now = (std::uint64_t)std::time(nullptr);
    for (auto it = g_records.begin(); it != g_records.end();) {
        const Record& r = it->second;
        const bool expired = r.updatedAt > 0 && now > r.updatedAt &&
                             now - r.updatedAt > kMaximumAgeSeconds;
        const bool missing = !plat::exists(r.path);
        if (expired || missing) it = g_records.erase(it);
        else                    ++it;
    }
    while (g_records.size() > kMaximumRecords) {
        auto oldest = std::min_element(g_records.begin(), g_records.end(),
            [](const auto& a, const auto& b) {
                return a.second.updatedAt < b.second.updatedAt;
            });
        if (oldest == g_records.end()) break;
        g_records.erase(oldest);
    }
}

bool save() {
    ensurePath();
    prune();
    const std::string tmp = g_path + ".tmp";
    FILE* f = openForAtomicWrite(tmp);
    if (!f) return false;
    bool ok = fprintf(f, "mpv-sofa-resume\t2\n") > 0;
    for (const auto& pair : g_records) {
        const Record& r = pair.second;
        if (fprintf(f, "%s\t%.6f\t%.6f\t%llu\t%llu\t%llu\t%d\t%d\t%d\n",
                    hexEncode(r.path).c_str(), r.position, r.duration,
                    (unsigned long long)r.size,
                    (unsigned long long)r.mtime,
                    (unsigned long long)r.updatedAt,
                    r.audioTrackId, r.subtitleTrackId,
                    r.subtitlesVisible ? 1 : 0) < 0) {
            ok = false;
            break;
        }
    }
    if (ok) ok = flushFile(f);
    if (fclose(f) != 0) ok = false;
    if (ok) ok = atomicReplace(tmp, g_path);
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(tmp), ec);
        fprintf(stderr, "[ResumeStore] Could not persist '%s'\n", g_path.c_str());
    }
    return ok;
}

} // namespace

void ResumeStore::init() {
    if (g_loaded) return;
    g_loaded = true;
    ensurePath();
    std::ifstream f(std::filesystem::u8path(g_path), std::ios::binary);
    if (!f) return;

    std::string s;
    bool first = true;
    int formatVersion = 0;
    while (std::getline(f, s)) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
        if (first) {
            first = false;
            if (s == "mpv-sofa-resume\t1") formatVersion = 1;
            else if (s == "mpv-sofa-resume\t2") formatVersion = 2;
            else break;
            continue;
        }
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (true) {
            const std::size_t tab = s.find('\t', begin);
            if (tab == std::string::npos) {
                fields.push_back(s.substr(begin));
                break;
            }
            fields.push_back(s.substr(begin, tab - begin));
            begin = tab + 1;
        }
        if (fields.size() < 6) continue;
        Record r;
        try {
            if (!hexDecode(fields[0], &r.path)) continue;
            r.position  = std::stod(fields[1]);
            r.duration  = std::stod(fields[2]);
            r.size      = std::stoull(fields[3]);
            r.mtime     = std::stoull(fields[4]);
            r.updatedAt = std::stoull(fields[5]);
            if (formatVersion >= 2 && fields.size() >= 9) {
                r.audioTrackId = std::stoi(fields[6]);
                r.subtitleTrackId = std::stoi(fields[7]);
                r.subtitlesVisible = std::stoi(fields[8]) != 0;
            }
        } catch (...) { continue; }
        if (!std::isfinite(r.position) || !std::isfinite(r.duration)) continue;
        g_records[canonicalKey(r.path)] = std::move(r);
    }
    prune();
}

bool ResumeStore::find(const std::string& path, Entry* out) {
    init();
    const std::string key = canonicalKey(path);
    auto it = g_records.find(key);
    if (it == g_records.end()) return false;
    const Record& r = it->second;
    if (!plat::exists(path) || plat::fileSize(path) != r.size ||
        plat::fileMtime(path) != r.mtime || r.position < kMinimumResumeSeconds ||
        (r.duration > 0.0 && r.position >= r.duration - kFinishedSlackSeconds)) {
        g_records.erase(it);
        save();
        return false;
    }
    if (out) {
        out->position = r.position;
        out->duration = r.duration;
        out->updatedAt = r.updatedAt;
        out->audioTrackId = r.audioTrackId;
        out->subtitleTrackId = r.subtitleTrackId;
        out->subtitlesVisible = r.subtitlesVisible;
    }
    return true;
}

bool ResumeStore::checkpoint(const std::string& path, double position,
                             double duration, int audioTrackId,
                             int subtitleTrackId, bool subtitlesVisible) {
    init();
    if (path.empty() || !plat::exists(path) || !std::isfinite(position) ||
        !std::isfinite(duration)) return false;
    if (position < kMinimumResumeSeconds ||
        (duration > 0.0 && position >= duration - kFinishedSlackSeconds))
        return clear(path);

    Record r;
    r.path = canonicalKey(path);
    r.position = position;
    r.duration = std::max(0.0, duration);
    r.size = plat::fileSize(path);
    r.mtime = plat::fileMtime(path);
    r.updatedAt = (std::uint64_t)std::time(nullptr);
    r.audioTrackId = audioTrackId;
    r.subtitleTrackId = subtitleTrackId;
    r.subtitlesVisible = subtitlesVisible;
    g_records[canonicalKey(path)] = std::move(r);
    return save();
}

bool ResumeStore::clear(const std::string& path) {
    init();
    const std::size_t erased = g_records.erase(canonicalKey(path));
    return erased == 0 || save();
}

const char* ResumeStore::filePath() {
    ensurePath();
    return g_path.c_str();
}
